/*
* mrustc common tools
* - by John Hodge (Mutabah)
*
* tools/common/jobserver.cpp
* - An interface to (or emulation of) make's jobserver
*/
#define _CRT_SECURE_NO_WARNINGS // No thread issues
#include "jobserver.h"
#include <cstring>
#include <cassert>
#include <string>
#include <sstream>
#include <iostream>
#ifdef _WIN32
# include <Windows.h>
#else
# include <unistd.h>
# include <sys/types.h>
# include <sys/stat.h>
# include <fcntl.h>
# include <thread>
# include <vector>
#endif

#ifdef _WIN32
class JobServer_Client: public JobServer
{
    HANDLE  m_sem_handle;
public:
    JobServer_Client(std::string path, HANDLE sem_handle)
        : m_sem_handle(sem_handle)
    {
    }
    ~JobServer_Client()
    {
        CloseHandle(m_sem_handle);
    }

    bool take_one(unsigned long timeout_ms) override {
        if(WaitForSingleObject(m_sem_handle, timeout_ms) == 0) {
            return true;
        }
        else {
            return false;
        }
    }
    void return_one() override {
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
    int m_fd_read;
    int m_fd_write;
    std::vector<uint8_t>    m_held_tokens;
    //::std::semaphore    m_sem;
public:
    JobServer_Client(int fd_read, int fd_write = -1)
        : m_fd_read(fd_read)
        , m_fd_write(fd_write)
    {
        assert(fd_read >= 0);
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
            assert(m_fd_read >= 0);
            struct timeval  timeout;
            timeout.tv_sec = timeout_ms / 1000;
            timeout.tv_usec = (timeout_ms % 1000) * 1000;
            fd_set  fds;
            FD_ZERO(&fds);
            FD_SET(m_fd_read, &fds);
            if( select(m_fd_read+1, &fds, nullptr, nullptr, &timeout) != 1 ) {
                return false;
            }
        }
        uint8_t token;
        int rv = read(m_fd_read, &token, 1);
        if( rv != 1 ) {
            perror("JobServer_Client::take_one read");
            return false;
        }
        m_held_tokens.push_back(token);
        return true;
    }
    void return_one() override {
        assert(!m_held_tokens.empty());
        auto t = m_held_tokens.back();
        m_held_tokens.pop_back();
        if( write(m_fd_write == -1 ? m_fd_read : m_fd_write, &t, 1) != 1 ) {
            // What can be done if the write fails?
            perror("JobServer_Client write");
        }
    }
};
class JobServer_Server: public JobServer
{
    class ServerInner {
        ::std::string   m_path;
        int m_wr_fd;
        int m_rd_fd;
    public:
        ServerInner(size_t max_jobs)
            : m_path()
            , m_wr_fd(-1)
            , m_rd_fd(-1)
        {
#if 1 && (_POSIX_C_SOURCE >= 200809L)
            char buf[] = "/tmp/mrustc-jobserverXXXXXX";
            m_path = ::std::string(mkdtemp(buf));
            if( mkfifo((m_path + "/fifo").c_str(), 0600) != 0 ) {
                perror("JobServer_Server mkfifo");
                throw std::runtime_error("JobServer_Server mkfifo");
            }
            m_wr_fd = open((m_path + "/fifo").c_str(), O_RDWR|O_CLOEXEC);
            if( m_wr_fd < 0 ) {
                perror("JobServer_Server open");
                throw std::runtime_error("JobServer_Server open");
            }
#else
            // TODO: For `pipe` it would be nice to propagate it to child processes, but that needs minicargo's `os`
            // support to be happy.
            int pipe_fds[2] = {-1,-1};
            if( pipe(pipe_fds) != 0 )
                throw std::runtime_error("pipe failed");
            m_rd_fd = pipe_fds[0];
            m_wr_fd = pipe_fds[1];
#endif
            for(size_t i = 0; i < max_jobs; i ++) {
                uint8_t t = 100;
                if( write(m_wr_fd, &t, 1) != 1 )
                    perror("ServerInner() write");
            }
        }
        ~ServerInner()
        {
            if( m_rd_fd != -1 ) {
                close(m_rd_fd);
            }
            close(m_wr_fd);
            if(!m_path.empty()) {
                if( unlink((m_path + "/fifo").c_str()) != 0 ) {
                    perror("~ServerInner unlink fifo");
                }
                if( rmdir(m_path.c_str()) != 0 ) {
                    perror("~ServerInner unlink dir");
                }
            }
        }
        int get_client_read_fd() const { return m_rd_fd == -1 ? m_wr_fd : m_rd_fd; }
        int get_client_write_fd() const { return m_wr_fd; }
        void dump_desc(::std::ostream& os) const {
            if( m_rd_fd == -1 ) {
                os << "fifo:" << m_path << "/fifo";
            }
            else {
                os << m_rd_fd << "," << m_wr_fd;
            }
        }
    };
    ServerInner  m_server;
    JobServer_Client    m_client;
public:
    JobServer_Server(size_t max_jobs)
        : m_server(max_jobs)
        , m_client(m_server.get_client_read_fd(), m_server.get_client_write_fd())
    {
        ::std::stringstream ss;
        if(const auto* makeflags = getenv("MAKEFLAGS"))
        {
            ss << makeflags << " ";
        }
        ss << "--jobserver-auth=";
        m_server.dump_desc(ss);
        setenv("MAKEFLAGS", ss.str().c_str(), /*overwrite=*/1);
    }
    ~JobServer_Server()
    {
    }
    bool take_one(unsigned long timeout_ms) override {
        return m_client.take_one(timeout_ms);
    }
    void return_one() override {
        return m_client.return_one();
    }
};
#endif


::std::unique_ptr<JobServer> JobServer::create(size_t server_jobs)
{
    const auto* makeflags = getenv("MAKEFLAGS");

    const char* jobserver_auth = nullptr;
    if( makeflags )
    {
        const char* const needle = "--jobserver-auth=";
        auto pos = ::std::strstr(makeflags, needle);
        while( pos != nullptr ) {
            auto e = pos + ::std::strlen(needle);
            if( pos == makeflags || pos[-1] == ' ' ) {
                jobserver_auth = e;
            }
            pos = ::std::strstr(e, needle);
        }
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
            return ::std::make_unique<JobServer_Client>(auth_str, sem_handle);
        }
#else
        // - Named pipe: `fifo:PATH`
        if( std::strncmp(auth_str.c_str(), "fifo:", 5) == 0 ) {
            auto fd = open(auth_str.c_str() + 5, O_RDWR|O_CLOEXEC);
            if(fd > 0) {
                return ::std::make_unique<JobServer_Client>(fd);
            }
        }
        // - Unix pipe pair: `<fd_r>,<fd_w>`
        else {
            int fd_r = -1, fd_w = -1;
            if( ::std::sscanf(auth_str.c_str(), "%d,%d", &fd_r, &fd_w) == 2 ) {
                if( fd_r >= 0 && fd_w >= 0 ) {
                    if( fcntl(fd_r, F_GETFL) == -1 || fcntl(fd_w, F_GETFL) == -1 ) {
                        ::std::cerr << "JobServer: Pipe FDs aren't open, likely missing `+` in makefile" << std::endl;
                    }
                    else {
                        return ::std::make_unique<JobServer_Client>(fd_r, fd_w);
                    }
                }
                else {
                }
            }
        }
#endif
    }
    // If no `-j` option is passed to this application, then don't create a jobserver
    if(server_jobs == 0) {
        return nullptr;
    }
    return ::std::make_unique<JobServer_Server>(server_jobs);
}
