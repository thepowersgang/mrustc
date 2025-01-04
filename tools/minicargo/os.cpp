//
//
//
#ifdef _MSC_VER
# define _CRT_SECURE_NO_WARNINGS    // Allows use of getenv (this program doesn't set env vars)
#endif

#include <iostream>
#include "os.hpp"
#include "debug.h"

#ifndef DISABLE_MULTITHREAD
# include <mutex>
#endif

#ifdef _WIN32
# if defined(__MINGW32__)
#  define DISABLE_MULTITHREAD    // Mingw32 doesn't have c++11 threads
// Mingw doesn't define putenv()
extern "C" {
extern int _putenv_s(const char*, const char*);
}
# endif
# include <Windows.h>
#else
extern "C" {
# include <unistd.h>    // open/chdir/
# include <sys/stat.h>  // mkdir?
# include <fcntl.h>  // O_*
# include <spawn.h> // posix_spawn
# include <sys/wait.h>  // waitpid
# include <limits.h>    // PATH_MAX
extern char **environ;
}
#endif
#ifdef __APPLE__
# include <mach-o/dyld.h>
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__) || (defined(__NetBSD__) && defined(KERN_PROC_PATHNAME)) // NetBSD 8.0+
# include <sys/sysctl.h>
#endif

namespace {
    #ifdef _WIN32
    // Escapes an argument for CommandLineToArgv on Windows
    void argv_quote_windows(const std::string& arg, std::stringstream& cmdline);
    #endif
}

namespace os_support {


Process::~Process()
{
#ifdef _WIN32
    if(m_stderr) {
        CloseHandle(m_stderr);
        m_stderr = nullptr;
    }
#else
    if(m_stderr > 0) {
        close(m_stderr);
        m_stderr = -1;
    }
#endif
}

Process Process::spawn(
    const char* exe_name,
    const StringList& args,
    const StringListKV& env,
    const ::helpers::path& logfile,
    const ::helpers::path& working_directory/*={}*/,
    bool print_command/*=true*/,
    bool capture_stderr/*=false*/
    )
{
    // Disabled: needs a bunch of work on other ends
    if(capture_stderr) {
        ::std::cerr << "TODO: Test and implement `capture_stderr`" << std::endl;
        abort();
    }
    // Create logfile output directory (if the parent path is valid, i.e. the logile path isn't a single component)
    if( logfile.parent().is_valid() ) {
        mkdir(logfile.parent());
    }

    if( getenv("MINICARGO_DUMPENV") )
    {
        ::std::stringstream environ_str;
        for(auto kv : env)
        {
            environ_str << kv.first << "=" << kv.second << ' ';
        }
        std::cout << environ_str.str() << std::endl;
    }

#ifdef _WIN32
    ::std::stringstream cmdline;
    cmdline << exe_name;
    for (const auto& arg : args.get_vec()) {
        argv_quote_windows(arg, cmdline);
    }
    auto cmdline_str = cmdline.str();
    if(print_command)
    {
        ::std::cout << "> " << cmdline_str << ::std::endl;
    }
    else
    {
        DEBUG("Calling " << cmdline_str);
    }

#if 0
    // TODO: Determine required minimal environment, to avoid importing the entire caller environment
    // - MRUSTC_*, CC*
    ::std::stringstream environ_str;
    environ_str << "TEMP=" << getenv("TEMP") << '\0';
    environ_str << "TMP=" << getenv("TMP") << '\0';
    environ_str << "PATH=" << getenv("PATH") << '\0';
    for(auto kv : env)
    {
        environ_str << kv.first << "=" << kv.second << '\0';
    }
    environ_str << '\0';
#else
    // TODO: This will leak variables between calls
    for(auto kv : env)
    {
        DEBUG("putenv " << kv.first << "=" << kv.second);
        _putenv_s(kv.first, kv.second);
    }
#endif

    class WinapiError: public ::std::exception {
        std::string msg;
    public:
        WinapiError(const char* name) {
            msg = format("`", name, "` failed: 0x", std::hex, GetLastError());
        }
        const char* what() const noexcept override {
            return msg.c_str();
        }
    };

    STARTUPINFO si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL;
    HANDLE  stderr_pipe = NULL;
    if(capture_stderr)
    {
        SECURITY_ATTRIBUTES sa = { 0 };
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if( !CreatePipe(&stderr_pipe, &si.hStdError, &sa, 0) ) {
            throw WinapiError("CreatePipe");
        }
    }
    else
    {
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    {
        SECURITY_ATTRIBUTES sa = { 0 };
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        si.hStdOutput = CreateFile( logfile.str().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        if(!si.hStdOutput)  throw WinapiError("CreateFile StdOutput");
        DWORD   tmp;
        WriteFile(si.hStdOutput, cmdline_str.data(), static_cast<DWORD>(cmdline_str.size()), &tmp, NULL);
        WriteFile(si.hStdOutput, "\n", 1, &tmp, NULL);
    }
    PROCESS_INFORMATION pi = { 0 };
    CreateProcessA(exe_name,
        (LPSTR)cmdline_str.c_str(),
        NULL, NULL, TRUE, 0, NULL,
        (working_directory != ::helpers::path() ? working_directory.str().c_str() : NULL),
        &si, &pi
        );
    CloseHandle(si.hStdOutput);
    return Process { pi.hProcess, stderr_pipe };
#else

    class CError: public ::std::exception {
        std::string msg;
    public:
        CError(const char* name) {
            msg = format("`", name, "` failed: errno=", errno, " ", strerror(errno));
        }
        const char* what() const noexcept override {
            return msg.c_str();
        }
    };

    // Create handles such that the log file is on stdout
    // - This string has to outlive `fa`
    ::std::string logfile_str = logfile.str();
    pid_t pid;
    posix_spawn_file_actions_t  fa;
    int stderr_streams[2] {-1,-1};
    if(capture_stderr) {
        if( pipe(stderr_streams) != 0 ) {
            throw CError("pipe");
        }
        fcntl(stderr_streams[0], F_SETFD, FD_CLOEXEC);
        fcntl(stderr_streams[1], F_SETFD, FD_CLOEXEC);
    }
    {
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, logfile_str.c_str(), O_CREAT|O_WRONLY|O_TRUNC, 0644);
        if( capture_stderr ) {
            posix_spawn_file_actions_adddup2(&fa, stderr_streams[1], 2);
        }
        // Note: JobServer FDs should get propagated
    }

    // Generate `argv`
    auto argv = args.get_vec();
    argv.insert(argv.begin(), exe_name);

    if(print_command)
    {
        ::std::cout << ">";
        for(const auto& p : argv)
            ::std::cout  << " " << p;
        ::std::cout << " > " << logfile;
        ::std::cout << ::std::endl;
    }
    else
    {
        Debug_Print([&](auto& os){
            os << "Calling";
            for(const auto& p : argv)
                os << " " << p;
            });
    }
    DEBUG("Environment " << env);
    argv.push_back(nullptr);

    // Generate `envp`
    StringList  envp;
    for(auto p = environ; *p; p++)
    {
        envp.push_back(*p);
    }
    for(auto kv : env)
    {
        envp.push_back(::format(kv.first, "=", kv.second));
    }
    //Debug_Print([&](auto& os){
    //    os << "ENVP=";
    //    for(const auto& p : envp.get_vec())
    //        os << "\n " << p;
    //    });
    envp.push_back(nullptr);

    {
#ifndef DISABLE_MULTITHREAD
        static ::std::mutex s_chdir_mutex;
        ::std::lock_guard<::std::mutex> lh { s_chdir_mutex };
#endif
        auto fd_cwd = open(".", O_DIRECTORY);

        if( working_directory != ::helpers::path() ) {
            if( chdir(working_directory.str().c_str()) != 0 ) {
                set_console_colour(std::cerr, TerminalColour::Red);
                ::std::cerr << "Unable to switch to working dir '" << working_directory << "' - " << strerror(errno);
                set_console_colour(std::cerr, TerminalColour::Default);
                ::std::cerr << ::std::endl;
                throw ::std::runtime_error("Unable to spawn process");
            }
        }
        if( posix_spawn(&pid, exe_name, &fa, /*attr=*/nullptr, (char* const*)argv.data(), (char* const*)envp.get_vec().data()) != 0 )
        {
            set_console_colour(std::cerr, TerminalColour::Red);
            ::std::cerr << "Unable to run process '" << exe_name << "' - " << strerror(errno);
            set_console_colour(std::cerr, TerminalColour::Default);
            ::std::cerr << ::std::endl;
            DEBUG("Unable to spawn executable");
            posix_spawn_file_actions_destroy(&fa);
            throw ::std::runtime_error("Unable to spawn process");
        }
        if( working_directory != ::helpers::path() ) {
            if( fchdir(fd_cwd) != 0 ) {
                ::std::cerr << "Restoring CWD failed" << std::endl;
                exit(1);
            }
        }
    }
    posix_spawn_file_actions_destroy(&fa);
    if(capture_stderr) {
        close(stderr_streams[1]);
    }
    return Process { pid, stderr_streams[0] };
#endif
}

bool Process::wait()
{
    #ifdef _WIN32
    if( this->m_stderr ) {
        throw ::std::runtime_error("capture_stderr with an explicit wait");
    }
    WaitForSingleObject(m_handle, INFINITE);
    DWORD status = 1;
    GetExitCodeProcess(m_handle, &status);
    return handle_status(status);
    #else
    if( this->m_stderr > 0 ) {
        throw ::std::runtime_error("capture_stderr with an explicit wait");
    }
    int status = -1;
    waitpid(m_handle, &status, 0);
    return handle_status(status);
    #endif
}

bool Process::handle_status(int status)
{
    #ifdef _WIN32
    if(status != 0)
    {
        auto dw_status = static_cast<DWORD>(status);
        set_console_colour(std::cerr, TerminalColour::Red);
        if( (dw_status & 0xC000'0000) != 0 ) {
            std::cerr << "Process exited with non-zero exit status 0x" << std::hex << dw_status << std::endl;
        }
        else {
            std::cerr << "Process exited with non-zero exit status " << status << std::endl;
        }
        set_console_colour(std::cerr, TerminalColour::Default);
        return false;
    }
    else
    {
        return true;
    }
    #else
    if( status != 0 )
    {
        set_console_colour(std::cerr, TerminalColour::Red);
        if( WIFEXITED(status) )
            ::std::cerr << "Process exited with non-zero exit status " << WEXITSTATUS(status) << ::std::endl;
        else if( WIFSIGNALED(status) )
            ::std::cerr << "Process was terminated with signal " << WTERMSIG(status) << ::std::endl;
        else
            ::std::cerr << "Process terminated for unknown reason, status=" << status << ::std::endl;
        set_console_colour(std::cerr, TerminalColour::Default);
        return false;
    }
    else 
    {
        return true;
    }
    #endif
}

void set_console_colour(std::ostream& os, TerminalColour colour) {
#if defined(_WIN32) && !defined(__MINGW32__)
    HANDLE  h;
    WORD default_val;
    if( &os == &std::cout ) {
        h = GetStdHandle(STD_OUTPUT_HANDLE);
        default_val = FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }
    else if( &os == &std::cerr ) {
        h = GetStdHandle(STD_ERROR_HANDLE);
        default_val = FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }
    else {
        return ;
    }
    switch(colour)
    {
    case TerminalColour::Default: SetConsoleTextAttribute(h, default_val); break;
    case TerminalColour::Red  : SetConsoleTextAttribute(h, FOREGROUND_INTENSITY | FOREGROUND_RED); break;
    case TerminalColour::Green: SetConsoleTextAttribute(h, FOREGROUND_INTENSITY | FOREGROUND_GREEN); break;
    }
#else
    // TODO: Only enable if printing to a terminal (not to a file)
    switch(colour)
    {
    case TerminalColour::Default:   os << "\x1B[0m";    break;
    case TerminalColour::Red  :     os << "\x1B[31m";   break;
    case TerminalColour::Green:     os << "\x1B[32m";   break;
    }
#endif
};

void mkdir(const helpers::path& p)
{
    #ifdef _WIN32
    CreateDirectoryA(p.str().c_str(), NULL);
    #else
    if( ::mkdir(static_cast<::std::string>(p).c_str(), 0755) != 0 ) {
        if( errno == EEXIST ) {
            // Ignore
        }
        else {
            throw ::std::runtime_error("mkdir failed");
        }
    }
    #endif
}

const helpers::path& get_mrustc_path()
{
    static helpers::path    s_compiler_path;
    if( !s_compiler_path.is_valid() )
    {
        if( const char* override_path = getenv("MRUSTC_PATH") ) {
            s_compiler_path = override_path;
            return s_compiler_path;
        }
        // TODO: Clean this stuff up
#ifdef _WIN32
        char buf[1024];
        size_t s = GetModuleFileName(NULL, buf, sizeof(buf)-1);
        buf[s] = 0;

        ::helpers::path minicargo_path { buf };
        minicargo_path.pop_component();
        // MSVC, minicargo and mrustc are in the same dir
        s_compiler_path = minicargo_path / "mrustc.exe";
#else
        char buf[PATH_MAX];
# ifdef __linux__
        ssize_t s = readlink("/proc/self/exe", buf, sizeof(buf)-1);
        if(s >= 0)
        {
            buf[s] = 0;
        }
        else
# elif defined(__APPLE__)
        uint32_t  s = sizeof(buf);
        if( _NSGetExecutablePath(buf, &s) == 0 )
        {
            // Buffer populated
        }
        else
            // TODO: Buffer too small
# elif defined(__FreeBSD__) || defined(__DragonFly__) || (defined(__NetBSD__) && defined(KERN_PROC_PATHNAME)) // NetBSD 8.0+
        int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
        size_t s = sizeof(buf);
        if ( sysctl(mib, 4, buf, &s, NULL, 0) == 0 )
        {
            // Buffer populated
        }
        else
# else
        #   warning "Can't runtime determine path to minicargo"
# endif
        {
            // On any error, just hard-code as if running from root dir
            strcpy(buf, "tools/bin/minicargo");
        }

        ::helpers::path minicargo_path { buf };
        minicargo_path.pop_component();
        s_compiler_path = (minicargo_path / "mrustc").normalise();
#endif
    }
    return s_compiler_path;
}

}

namespace {
#ifdef _WIN32
// Escapes an argument for CommandLineToArgv on Windows
void argv_quote_windows(const std::string& arg, std::stringstream& cmdline)
{
    if (arg.empty()) return;
    // Add a space to start a new argument.
    cmdline << " ";

    // Don't quote unless we need to
    if (arg.find_first_of(" \t\n\v\"") == arg.npos)
    {
        cmdline << arg;
        return;
    }
    else
    {
        cmdline << '"';
        for (auto ch = arg.begin(); ; ++ch) {
            size_t backslash_count = 0;

            // Count backslashes
            while (ch != arg.end() && *ch == L'\\') {
                ++ch;
                ++backslash_count;
            }

            if (ch == arg.end()) {
                // Escape backslashes, but let the terminating
                // double quotation mark we add below be interpreted
                // as a metacharacter.
                for (int i = 0; i < backslash_count * 2; i++) cmdline << '\\';
                break;
            }
            else if (*ch == L'"')
            {
                // Escape backslashes and the following double quotation mark.
                for (int i = 0; i < backslash_count * 2 + 1; i++) cmdline << '\\';
                cmdline << *ch;
            }
            else
            {
                for (int i = 0; i < backslash_count; i++) cmdline << '\\';
                cmdline << *ch;
            }
        }
        cmdline << '"';
    }
}
#endif
}

