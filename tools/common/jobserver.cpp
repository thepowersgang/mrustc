/*
* mrustc common tools
* - by John Hodge (Mutabah)
*
* tools/common/jobserver.cpp
* - An interface to (or emulation of) make's jobserver
*/
#define _CRT_SECURE_NO_WARNINGS // No thread issues
#include "jobserver.h"
#include <string>
#include <sstream>
#ifdef _WIN32
#include <Windows.h>
#else
#include <>
#include <unistd.h>
#endif

#ifdef _WIN32
class JobServer_Client: public JobServer
{
    HANDLE  m_local_sem_handle;
    HANDLE  m_sem_handle;
public:
    JobServer_Client(size_t max_jobs, std::string path, HANDLE sem_handle)
        : m_local_sem_handle(CreateSemaphore(nullptr, max_jobs, max_jobs, nullptr))
        , m_sem_handle(sem_handle)
    {
    }
    ~JobServer_Client()
    {
        CloseHandle(m_local_sem_handle);
        CloseHandle(m_sem_handle);
    }

    bool take_one(unsigned long timeout_ms) override {
        auto t = timeGetTime();
        if( WaitForSingleObject(m_local_sem_handle, timeout_ms) != 0 ) {
            return false;
        }
        auto dt_ms = timeGetTime() - t;
        auto new_timeout = timeout_ms == INFINITE ? INFINITE :
            (timeout_ms < dt_ms ? 0 : timeout_ms - dt_ms)
            ;
        if(WaitForSingleObject(m_sem_handle, new_timeout) == 0) {
            return true;
        }
        else {
            ReleaseSemaphore(m_local_sem_handle, 1, NULL);
            return false;
        }
    }
    void return_one() override {
        ReleaseSemaphore(m_local_sem_handle, 1, NULL);
        ReleaseSemaphore(m_sem_handle, 1, NULL);
    }
};
class JobServer_Server: public JobServer
{
    std::string m_path;
    HANDLE  m_sem_handle;
public:
    JobServer_Server(size_t max_jobs)
        : m_path(make_path())
        , m_sem_handle(CreateSemaphoreA(nullptr, max_jobs, max_jobs, m_path.c_str()))
    {
        ::std::stringstream ss;
        if(const auto* makeflags = getenv("MAKEFLAGS"))
        {
            ss << makeflags << " ";
        }
        ss << "--jobserver-auth=" << m_path;
        SetEnvironmentVariableA("MAKEFLAGS", ss.str().c_str());
    }
    ~JobServer_Server()
    {
        CloseHandle(m_sem_handle);
    }
    bool take_one(unsigned long timeout_ms) override {
        return (WaitForSingleObject(m_sem_handle, timeout_ms) == 0);
    }
    void return_one() override {
        ReleaseSemaphore(m_sem_handle, 1, NULL);
    }
private:
    static std::string make_path() {
        ::std::stringstream ss;
        ss << "mrustc_job_server-" << GetProcessId(NULL);
        return ss.str();
    }
};
#else
class JobServer_Client: public JobServer
{
    size_t  m_jobs;
    int m_fd_read;
    int m_fd_write;
    std::vector<uint8_t>    m_held_tokens;
    ::std::semaphore    m_sem;
public:
    JobServer_Client(size_t max_jobs, int fd_read, int fd_write = -1)
        : m_jobs(max_jobs)
        , m_fd_read(fd_read)
        , m_fd_write(fd_write)
    {
    }
    ~JobServer_Client()
    {
        if( m_fd_write == -1 ) {
            close(m_fd_read);
        }
    }
    bool take_one(unsigned long timeout_ms) override {
        if(timeout_ms != ~0ul)
        {
            fd_set  fds;
            FD_ZERO(&fds);
            FD_SET(m_fd_read, &fds);
            if( select(m_fd_read+1, &fds, nullptr, nullptr, &timeout) != 1 ) {
                return false;
            }
        }
        uint8_t token;
        int rv = read(m_fd_read, 1, &token);
        if( rv == 0 ) {
            return false;
        }
        m_held_tokens.push_back(token);
        return true;
    }
    void return_one() override {
        assert(!m_held_tokens.empty());
        auto t = m_held_tokens.back();
        m_held_tokens.pop_back();
        write(m_fd_write == -1 ? m_fd_read : m_fd_write, 1, &t);
    }
};
class JobServer_Server: public JobServer
{
    size_t  m_jobs;
    ::std::thread   m_worker;
public:
    JobServer_Server(size_t max_jobs)
    {
    }
    ~JobServer_Server()
    {
    }
};
#endif


::std::unique_ptr<JobServer> JobServer::create(size_t max_jobs)
{
    if( max_jobs >= 1 ) {
        return nullptr;
    }
    const auto* makeflags = getenv("MAKEFLAGS");

    const char* jobserver_auth = nullptr;
    const char* const needle = "--jobserver-auth=";
    auto pos = ::std::strstr(makeflags, needle);
    while( pos != nullptr ) {
        auto e = pos + ::std::strlen(needle);
        if( pos == makeflags || pos[-1] == ' ' ) {
            jobserver_auth = e;
        }
        pos = ::std::strstr(e, needle);
    }

    if( jobserver_auth )
    {
        const auto* p = std::strchr(jobserver_auth, ' ');
        auto len = p ? p - jobserver_auth : strlen(jobserver_auth);
        std::string auth_str(jobserver_auth, len);

        // Found a valid jobserver string!
#ifdef _WIN32
        // - Windows: named semaphore
        auto sem_handle = OpenSemaphoreA(0,FALSE,auth_str.c_str());
        if( sem_handle ) {
            return ::std::make_unique<JobServer_Client>(max_jobs, auth_str, sem_handle);
        }
#else
        // - Named pipe: `fifo:PATH`
        if( std::strncmp(auth_str.c_str(), "fifo:", 5) == 0 ) {
            auto fd = open(auth_str.c_str() + 5, O_READ|O_WRITE);
            if(fd > 0) {
                return ::std::make_unique<JobServer_Client>(max_jobs, fd);
            }
        }
        // - Unix pipe pair: `<fd_r>,<fd_w>`
        else {
            int fd_r = -1, fd_w = -1;
            if( ::std::sscanf("%d,%d", &fd_r, &fd_w) == 2 ) {
                if( fd_r >= 0 && fd_w >= 0 ) {
                    return ::std::make_unique<JobServer_Client>(max_jobs, fd_r, fd_w);
                }
            }
        }
#endif
    }
    return ::std::make_unique<JobServer_Server>(max_jobs);
}
