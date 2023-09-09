/*
 * minicargo - MRustC-specific clone of `cargo`
 * - By John Hodge (Mutabah)
 *
 * jobs.cpp
 * - Logic related to running build tasks
 */
#include <iostream>
#include "jobs.hpp"
#include "debug.h"

#include <cassert>
#include <algorithm>

#ifdef _WIN32
# error TODO Windows
#else
extern "C" {
# include <unistd.h>    // open/chdir/
# include <sys/stat.h>  // mkdir?
# include <fcntl.h>  // O_*
# include <spawn.h> // posix_spawn
# include <sys/wait.h>  // waitpid
}
#endif

namespace {
    enum class TerminalColour {
        Default,
        Red,    // ANSI 1
        Green  // ANSI 2
    };
    void set_console_colour(std::ostream& os, TerminalColour colour);
}

void JobList::run_all()
{
    bool failed = false;
    while( !this->waiting_jobs.empty() && !this->runnable_jobs.empty() && !this->running_jobs.empty() )
    {
        // Wait until a running job stops
        while( this->running_jobs.size() >= this->num_jobs )
        {
            if( !wait_one() ) {
                failed = true;
                break;
            }
        }
        if(failed) {
            break;
        }
        
        // Update the runnable list.
        for(auto& slot : this->waiting_jobs)
        {
            if(slot && slot->is_runnable()) {
                this->runnable_jobs.push_back(std::move(slot));
            }
        }
        if( this->runnable_jobs.empty() ) {
            break;
        }
        
        auto rjob = ::std::move(this->runnable_jobs.front()->start());
        this->runnable_jobs.pop_front();

        auto handle = this->spawn(*rjob);
        this->running_jobs.push_back(std::make_pair(handle, std::move(rjob)));
    }
    while( !this->running_jobs.empty() )
    {
        failed |= !wait_one();
    }
}

JobList::child_t JobList::spawn(const RunnableJob& rjob)
{
#ifdef _WIN32
    ::std::stringstream cmdline;
    cmdline << rjob.exe_name();
    for (const auto& arg : rjob.args().get_vec())
        argv_quote_windows(arg, cmdline);
    auto cmdline_str = cmdline.str();
    if(true)
    {
        ::std::cout << "> " << cmdline_str << ::std::endl;
    }
    else
    {
        DEBUG("Calling " << cmdline_str);
    }

#if 0
    // TODO: Determine required minimal environment, to avoid importing the entire caller environment
    ::std::stringstream environ_str;
    environ_str << "TEMP=" << getenv("TEMP") << '\0';
    environ_str << "TMP=" << getenv("TMP") << '\0';
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

    {
        auto logfile_dir = rjob.logfile().parent();
        if(logfile_dir.is_valid())
        {
            CreateDirectory(logfile_dir.str().c_str(), NULL);
        }
    }

    STARTUPINFO si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    {
        SECURITY_ATTRIBUTES sa = { 0 };
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        si.hStdOutput = CreateFile( static_cast<::std::string>(rjob.logfile()).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        DWORD   tmp;
        WriteFile(si.hStdOutput, cmdline_str.data(), static_cast<DWORD>(cmdline_str.size()), &tmp, NULL);
        WriteFile(si.hStdOutput, "\n", 1, &tmp, NULL);
    }
    PROCESS_INFORMATION pi = { 0 };
    CreateProcessA(rjob.exe_name(),
        (LPSTR)cmdline_str.c_str(),
        NULL, NULL, TRUE, 0, NULL,
        (rjob.working_directory() != ::helpers::path() ? rjob.working_directory().str().c_str() : NULL),
        &si, &pi
        );
    CloseHandle(si.hStdOutput);
    return pi.hProcess;
#else
    // Create logfile output directory
    mkdir(static_cast<::std::string>(rjob.logfile().parent()).c_str(), 0755);

    // Create handles such that the log file is on stdout
    ::std::string logfile_str = rjob.logfile();
    pid_t pid;
    posix_spawn_file_actions_t  fa;
    {
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, logfile_str.c_str(), O_CREAT|O_WRONLY|O_TRUNC, 0644);
    }

    // Generate `argv`
    auto argv = rjob.args().get_vec();
    argv.insert(argv.begin(), rjob.exe_name());

    if(true)
    {
        ::std::cout << ">";
        for(const auto& p : argv)
            ::std::cout  << " " << p;
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
    DEBUG("Environment " << rjob.env());
    argv.push_back(nullptr);

    // Generate `envp`
    StringList  envp;
    extern char **environ;
    for(auto p = environ; *p; p++)
    {
        envp.push_back(*p);
    }
    for(auto kv : rjob.env())
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
        auto fd_cwd = open(".", O_DIRECTORY);

        if( rjob.working_directory() != ::helpers::path() ) {
            if( chdir(rjob.working_directory().str().c_str()) != 0 ) {
                set_console_colour(std::cerr, TerminalColour::Red);
                ::std::cerr << "Unable to switch to working dir '" << rjob.working_directory() << "' - " << strerror(errno);
                set_console_colour(std::cerr, TerminalColour::Default);
                ::std::cerr << ::std::endl;
                return -1;
            }
        }
        if( posix_spawn(&pid, rjob.exe_name(), &fa, /*attr=*/nullptr, (char* const*)argv.data(), (char* const*)envp.get_vec().data()) != 0 )
        {
            set_console_colour(std::cerr, TerminalColour::Red);
            ::std::cerr << "Unable to run process '" << rjob.exe_name() << "' - " << strerror(errno);
            set_console_colour(std::cerr, TerminalColour::Default);
            ::std::cerr << ::std::endl;
            DEBUG("Unable to spawn executable");
            posix_spawn_file_actions_destroy(&fa);
            return -1;
        }
        if( rjob.working_directory() != ::helpers::path() ) {
            if( fchdir(fd_cwd) != 0 ) {
                ::std::cerr << "Restoring CWD failed" << std::endl;
                exit(1);
            }
        }
    }
    posix_spawn_file_actions_destroy(&fa);
    return pid;
#endif
}

bool JobList::wait_one()
{
    rjob_t  job;
    bool rv;
#ifdef _WIN32
    ::std::vector<HANDLE>   handles;
    for(const auto& j : this->running_jobs) {
        handles.push_back(j.first);
    }
    auto wait_rv = WaitForMultipleObjects(handles.size(), handles.data(), FALSE, 0);
    if( !(WAIT_OBJECT_0 <= wait_rv && wait_rv < WAIT_OBJECT_0 + handles.size()) ) {
        return false;
    }

    auto idx = wait_rv - WAIT_OBJECT_0;
    auto handle = this->running_jobs[idx].first;
    job = ::std::move(this->running_jobs[idx].second);
    this->running_jobs.erase(this->running_jobs.begin() + idx);

    DWORD status = 1;
    GetExitCodeProcess(handle, &status);
    if (status != 0)
    {
        set_console_colour(std::cerr, TerminalColour::Red);
        std::cerr << "Process `" << cmdline_str << "` exited with non-zero exit status " << std::hex << status;
        set_console_colour(std::cerr, TerminalColour::Default);
        std::cerr << std::endl;
        rv = false;
    }
    else {
        rv = true;
    }
#else
    int status = -1;
    auto pid = waitpid(0, &status, 0);
    if(pid == (pid_t)-1)
    {
        ::std::cerr << "`waitpid` failed!" << std::endl;
        return false;
    }
    else
    {
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
            rv = false;
        }
        else 
        {
            rv = true;
        }

        auto i = ::std::find_if(this->running_jobs.begin(), this->running_jobs.end(), [&](const auto& e){ return e.first == pid; });
        assert(i != this->running_jobs.end());
        assert(i->second);
        job = ::std::move(i->second);
        this->running_jobs.erase(i);
    }
#endif

    if( !rv )
    {
        ::std::cerr << "FAILING COMMAND: " << job->exe_name();
        for(const auto& p : job->args().get_vec())
            ::std::cerr  << " " << p;
        ::std::cerr << ::std::endl;
        ::std::cerr << "Env: ";
        for(auto kv : job->env())
        {
            ::std::cerr << " " << kv.first << "=" << kv.second;
        }
        ::std::cerr << ::std::endl;
        //::std::cerr << "See " << job->logfile() << " for the compiler output" << ::std::endl;
    }
    return rv;
}

namespace {

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

}