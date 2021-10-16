/*
 * mrustc test runner
 * - by John Hodge (Mutabah)
 *
 * tools/testrunner/main.cpp
 * - Clone of rustc's integration test runner
 *
 *
 * Runs all .rs files in a directory, parsing test options out of comments in the file
 */
#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <cctype>   // std::isblank
#include "../common/debug.h"
#include "../common/path.h"
#ifdef _WIN32
# include <Windows.h>
# define MRUSTC_PATH    "x64\\Release\\mrustc.exe"
#else
# include <sys/types.h>
# include <dirent.h>
# include <sys/stat.h>
# include <unistd.h>
# include <spawn.h>
# include <fcntl.h> // O_*
# include <sys/wait.h>  // waitpid
# include <signal.h>
# define MRUSTC_PATH    "./bin/mrustc"
#endif
#include <algorithm>

struct Options
{
    const char* output_dir = nullptr;
    const char* input_glob = nullptr;
    ::std::vector<::std::string>    test_list;

    bool    debug_enabled;
    ::std::vector<::std::string>    lib_dirs;

    int debug_level = 0;

    const char* exceptions_file = nullptr;
    bool fail_fast = false;

    int parse(int argc, const char* argv[]);

    void usage_short() const;
    void usage_full() const;
};

struct TestDesc
{
    ::std::string   m_name;
    ::std::string   m_path;
    ::std::vector<::std::string>    m_pre_build;
    ::std::vector<::std::string>    m_extra_flags;
    bool ignore;
    bool no_run;
    TestDesc()
        :ignore(false)
        ,no_run(false)
    {
    }
};
struct Timestamp
{
    static Timestamp for_file(const ::helpers::path& p);
#if _WIN32
    uint64_t m_val;

    Timestamp(FILETIME ft):
        m_val( (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime) )
    {
    }
#else
    time_t  m_val;
#endif
    static Timestamp infinite_past() {
#if _WIN32
        return Timestamp { FILETIME { 0, 0 } };
#else
        return Timestamp { 0 };
#endif
    }

    bool operator==(const Timestamp& x) const {
        return m_val == x.m_val;
    }
    bool operator<(const Timestamp& x) const {
        return m_val < x.m_val;
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const Timestamp& x) {
#if _WIN32
        os << ::std::hex << x.m_val << ::std::dec;
#else
        os << x.m_val;
#endif
        return os;
    }
};

bool run_executable(const ::helpers::path& file, const ::std::vector<const char*>& args, const ::helpers::path& outfile, unsigned timeout_seconds);

bool run_compiler(const Options& opts, const ::helpers::path& source_file, const ::helpers::path& output, const ::std::vector<::std::string>& extra_flags, ::helpers::path libdir={}, bool is_dep=false)
{
    ::std::vector<const char*>  args;
    args.push_back("mrustc");

    // Force optimised and debuggable
    args.push_back("-O");

    // Only turn debug on when requested by the caller
    if( opts.debug_enabled )
    {
        args.push_back("-g");
    }

    for(const auto& d : opts.lib_dirs)
    {
        args.push_back("-L");
        args.push_back(d.c_str());
    }
    if(libdir.is_valid())
    {
        args.push_back("-L");
        args.push_back(libdir.str().c_str());
    }

    ::helpers::path logfile;
    if( !is_dep )
    {
        args.push_back("-o");
        args.push_back(output.str().c_str());
        logfile = output + "-build.log";
    }
    else
    {
        args.push_back("--crate-type");
        args.push_back("rlib");
        args.push_back("--out-dir");
        args.push_back(output.str().c_str());

        logfile = output / source_file.basename() + "-build.log";
    }
    args.push_back(source_file.str().c_str());
    for(const auto& s : extra_flags)
        args.push_back(s.c_str());

    return run_executable(MRUSTC_PATH, args, logfile, 0);
}

static bool gTimeout = false;
static bool gInterrupted = false;
void sigalrm_handler(int) {
    gTimeout = true;
}
void sigint_handler(int) {
    gInterrupted = true;
}

int main(int argc, const char* argv[])
{
    Options opts;
    if( int v = opts.parse(argc, argv) )
    {
        return v;
    }

#ifdef _WIN32
#else
    {
        struct sigaction    sa = {0};
        sa.sa_handler = sigalrm_handler;
        sigaction(SIGALRM, &sa, NULL);
        signal(SIGINT, sigint_handler);
    }
#endif

    ::std::vector<::std::string>    skip_list;
    //  > Filter out tests listed in an exceptions file (newline separated, supports comments)
    if( opts.exceptions_file )
    {
        auto exceptions_list = ::helpers::path(opts.exceptions_file);
        ::std::ifstream in(exceptions_list.str());
        if( !in.good() )
        {
            // TODO: Error?
            ::std::cerr << "Unable to open exceptions list " << exceptions_list << ::std::endl;
            return 0;
        }
        else
        {
            while( !in.eof() )
            {
                ::std::string   line;
                ::std::getline(in, line);
                if( line == "" )
                    continue ;
                if( line[0] == '#' )
                    continue ;
                auto p = line.find('#');
                if( p != ::std::string::npos )
                {
                    line.resize(p);
                }
                while(!line.empty() && ::std::isblank(line.back()))
                    line.pop_back();
                if( line == "" )
                    continue ;
                skip_list.push_back(line);
            }
        }
    }
    auto outdir = opts.output_dir ? ::helpers::path(opts.output_dir) : throw "";

    ::std::vector<TestDesc> tests;

    // 1. Take input glob/folder and enumerate .rs files/matches
    // - If input path is a folder, find *.rs
    // - Otherwise, accept glob.
    // 2. Open each file and extract the various flags required.
    // 3. Build each test to its own output subdirectory
    // 4. Run tests
    {
        auto input_path = ::helpers::path(opts.input_glob);
#ifdef _WIN32
        WIN32_FIND_DATA find_data;
        auto mask = input_path / "*.rs";
        HANDLE find_handle = FindFirstFile( mask.str().c_str(), &find_data );
        if( find_handle == INVALID_HANDLE_VALUE ) {
            ::std::cerr << "Unable to find files matching " << mask << ::std::endl;
            return 1;
        }
        do
        {
            auto test_file_path = input_path / find_data.cFileName;
#else
        auto* dp = opendir(input_path.str().c_str());
        if( dp == nullptr )
            throw ::std::runtime_error(::format( "Unable to open vendor directory '", input_path, "'" ));
        while( const auto* dent = readdir(dp) )
        {
            if( dent->d_name[0] == '.' )
                continue ;
            auto test_file_path = input_path / dent->d_name;
            struct stat sb;
            stat(test_file_path.str().c_str(), &sb);
            if( (sb.st_mode & S_IFMT) != S_IFREG) {
                continue ;
            }
#endif
            ::std::ifstream in(test_file_path.str());
            if(!in.good())
                continue ;
            if( opts.debug_level > 0 )
            {
                DEBUG("> " << test_file_path);
            }

            TestDesc    td;
            td.ignore = false;

            do
            {
                ::std::string   line;
                ::std::getline(in, line);
                if( !(line[0] == '/' && line[1] == '/'/* && line[2] == ' '*/) )
                    continue ;
                // TODO Parse a skewer-case ident and check against known set?

                size_t start = (line[2] == ' ' ? 3 : 2);

                if( line.substr(start, 10) == "aux-build:" )
                {
                    td.m_pre_build.push_back( line.substr(start+10) );
                }
                else if( line.substr(start, 11) == "ignore-test" )
                {
                    td.ignore = true;
                }
                else if( line.substr(start, 4+1+7) == "skip-codegen" )
                {
                    td.no_run = true;
                }
                else if( line.substr(start, 14) == "compile-flags:" )
                {
                    auto end = line.find(' ', 3+14);
                    start += 14;
                    do
                    {
                        if( start != end )
                        {
                            auto a = line.substr(start, end-start);
                            if( a != "" )
                            {
                                if( opts.debug_level > 1 )
                                    DEBUG("+" << a);
                                td.m_extra_flags.push_back(::std::move(a));
                            }
                        }
                        if( end == ::std::string::npos )
                            break;
                        start = end + 1;
                        end = line.find(' ', start);
                    } while(1);
                }
            } while( !in.eof() );

            td.m_name = test_file_path.basename();
            td.m_name.pop_back();
            td.m_name.pop_back();
            td.m_name.pop_back();
            td.m_path = test_file_path;

            tests.push_back(td);
#ifndef _WIN32
        }
        closedir(dp);
#else
        } while( FindNextFile(find_handle, &find_data) );
        FindClose(find_handle);
#endif

        // Sort tests before running
        ::std::sort(tests.begin(), tests.end(), [](const auto& a, const auto& b){ return a.m_name < b.m_name; });

        // ---
        const bool SKIP_PASS = (getenv("TESTRUNNER_SKIPPASS") != nullptr);
        const bool NO_COMPILER_DEP = (getenv("TESTRUNNER_NOCOMPILERDEP") != nullptr);
        const auto compiler_ts = Timestamp::for_file(MRUSTC_PATH);
        unsigned n_skip = 0;
        unsigned n_cfail = 0;
        unsigned n_fail = 0;
        unsigned n_ok = 0;
        for(const auto& test : tests)
        {
            if( gInterrupted ) {
                DEBUG(">> Interrupted");
                return 1;
            }
            if( !opts.test_list.empty() && ::std::find(opts.test_list.begin(), opts.test_list.end(), test.m_name) == opts.test_list.end() )
            {
                if( opts.debug_level > 0 )
                    DEBUG(">> NOT SELECTED");
                continue ;
            }
            if( test.ignore )
            {
                if( opts.debug_level > 0 )
                    DEBUG(">> IGNORE " << test.m_name);
                continue ;
            }
            if( ::std::find(skip_list.begin(), skip_list.end(), test.m_name) != skip_list.end() )
            {
                if( opts.debug_level > 0 )
                    DEBUG(">> SKIP " << test.m_name);
                n_skip ++;
                continue ;
            }

            //DEBUG(">> " << test.m_name);
            auto depdir = outdir / "deps-" + test.m_name.c_str();
            auto test_exe = outdir / test.m_name + ".exe";
            auto test_output = outdir / test.m_name + ".out";

            auto test_exe_ts = Timestamp::for_file(test_exe);
            auto test_output_ts = Timestamp::for_file(test_output);
            // (Optional) if the target file doesn't exist, force a re-compile IF the compiler is newer than the
            // executable.
            if( SKIP_PASS )
            {
                // If output is missing (the last run didn't succeed), and the compiler is newer than the executable
                if( test_output_ts == Timestamp::infinite_past() && test_exe_ts < compiler_ts )
                {
                    // Force a recompile
                    test_exe_ts = Timestamp::infinite_past();
                }
            }
            if( test_exe_ts == Timestamp::infinite_past() || (!NO_COMPILER_DEP && !SKIP_PASS && test_exe_ts < compiler_ts) )
            {
                bool pre_build_failed = false;
                for(const auto& file : test.m_pre_build)
                {
#ifdef _WIN32
                    CreateDirectoryA(depdir.str().c_str(), NULL);
#else
                    mkdir(depdir.str().c_str(), 0755);
#endif
                    auto infile = input_path / "auxiliary" / file;
                    if( !run_compiler(opts, infile, depdir, {}, depdir, true) )
                    {
                        DEBUG("COMPILE FAIL " << infile << " (dep of " << test.m_name << ")");
                        n_cfail ++;
                        pre_build_failed = true;
                        break;
                    }
                }
                if( pre_build_failed )
                {
                    if( opts.fail_fast )
                        return 1;
                    else
                        continue;
                }

                // If there's no pre-build files (dependencies), clear the dependency path (cleaner output)
                if( test.m_pre_build.empty() )
                {
                    depdir = ::helpers::path();
                }

                auto compile_logfile = test_exe + "-build.log";
                if( !run_compiler(opts, test.m_path, test_exe, test.m_extra_flags, depdir) )
                {
                    DEBUG("COMPILE FAIL " << test.m_name << ", log in " << compile_logfile);
                    n_cfail ++;
                    if( opts.fail_fast )
                        return 1;
                    else
                        continue;
                }
                test_exe_ts = Timestamp::for_file(test_exe);
            }
            // - Run the test
            if( test.no_run )
            {
                ::std::ofstream(test_output.str()) << "";
                if( opts.debug_level > 0 )
                    DEBUG("No run " << test.m_name);
            }
            else if( test_output_ts < test_exe_ts )
            {
                auto run_out_file_tmp = test_output + ".tmp";
                if( !run_executable(test_exe, { test_exe.str().c_str() }, run_out_file_tmp, 10) )
                {
                    DEBUG("RUN FAIL " << test.m_name);

                    // Move the failing output file
                    auto fail_file = test_output + "_failed";
                    remove(fail_file.str().c_str());
                    rename(run_out_file_tmp.str().c_str(), fail_file.str().c_str());
                    DEBUG("- Output in " << fail_file);

                    n_fail ++;
                    if( opts.fail_fast )
                        return 1;
                    else
                        continue;
                }
                else
                {
                    remove(test_output.str().c_str());
                    rename(run_out_file_tmp.str().c_str(), test_output.str().c_str());
                }
            }
            else
            {
                if( opts.debug_level > 0 )
                    DEBUG("Unchanged " << test.m_name);
            }

            n_ok ++;
        }

        ::std::cout << "TESTS COMPLETED" << ::std::endl;
        ::std::cout << n_ok << " passed, " << n_fail << " failed, " << n_cfail << " errored, " << n_skip << " skipped" << ::std::endl;

        if( n_fail > 0 || n_cfail > 0 )
            return 1;
    }

    return 0;
}

int Options::parse(int argc, const char* argv[])
{
    for(int i = 1; i < argc; i ++)
    {
        const char* arg = argv[i];
        if( arg[0] != '-' )
        {
            if( !this->input_glob ) {
                this->input_glob = arg;
            }
            // TODO: Multiple input globs?
            else {
                this->test_list.push_back(arg);
                //this->usage_short();
                //return 1;
            }
        }
        else if( arg[1] != '-' )
        {
            switch(arg[1])
            {
            case 'o':
                if( this->output_dir ) {
                    this->usage_short();
                    return 1;
                }
                if( i+1 == argc ) {
                    this->usage_short();
                    return 1;
                }
                this->output_dir = argv[++i];
                break;
            case 'v':
                this->debug_level += 1;
                break;
            case 'g':
                this->debug_enabled = true;
                break;
            case 'L':
                if( i+1 == argc ) {
                    this->usage_short();
                    return 1;
                }
                this->lib_dirs.push_back( argv[++i] );
                break;

            default:
                this->usage_short();
                return 1;
            }
        }
        else
        {
            if( 0 == ::std::strcmp(arg, "--help") )
            {
                this->usage_full();
                return 0;
            }
            else if( 0 == ::std::strcmp(arg, "--exceptions") )
            {
                if( this->exceptions_file ) {
                    this->usage_short();
                    return 1;
                }
                if( i+1 == argc ) {
                    this->usage_short();
                    return 1;
                }
                this->exceptions_file = argv[++i];
            }
            else if( 0 == ::std::strcmp(arg, "--output-dir") )
            {
                if( this->output_dir ) {
                    this->usage_short();
                    return 1;
                }
                if( i+1 == argc ) {
                    this->usage_short();
                    return 1;
                }
                this->output_dir = argv[++i];
            }
            else if( 0 == ::std::strcmp(arg, "--fail-fast") )
            {
                this->fail_fast = true;
            }
            else
            {
                this->usage_short();
                return 1;
            }
        }
    }
    return 0;
}

void Options::usage_short() const
{
}
void Options::usage_full() const
{
}

#ifdef _WIN32
namespace {
    class WinapiError {
        DWORD   v;
        WinapiError(DWORD v): v(v) {}
    public:
        static WinapiError get() {
            return WinapiError(GetLastError());
        }
        friend std::ostream& operator<<(std::ostream& os, const WinapiError& x) {
            char* buf;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM, NULL, x.v, 0, (LPSTR)&buf, 0, nullptr);
            os << "0x" << std::hex << x.v << ": " << buf;
            LocalFree(buf);
            return os;
        }
    };
}
#endif

///
bool run_executable(const ::helpers::path& exe_name, const ::std::vector<const char*>& args, const ::helpers::path& outfile, unsigned timeout_seconds)
{
#ifdef _WIN32
    ::std::stringstream cmdline;
    for (const auto& arg : args)
        cmdline << arg << " ";
    auto cmdline_str = cmdline.str();
    cmdline_str.pop_back();
    DEBUG("Calling " << cmdline_str);

    STARTUPINFO si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES|STARTF_FORCEOFFFEEDBACK;
    si.hStdInput = NULL;
    {
        SECURITY_ATTRIBUTES sa = { 0 };
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        si.hStdOutput = CreateFile( outfile.str().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        //DWORD   tmp;
        //WriteFile(si.hStdOutput, cmdline_str.data(), static_cast<DWORD>(cmdline_str.size()), &tmp, NULL);
        //WriteFile(si.hStdOutput, "\n", 1, &tmp, NULL);
    }
    if( !DuplicateHandle(GetCurrentProcess(), si.hStdOutput, GetCurrentProcess(), &si.hStdError, /*dwDesiredAccess(unused)*/0, /*bInheritHandle*/TRUE, DUPLICATE_SAME_ACCESS) ) {
        std::cerr << "DuplicateHandle failed: " << WinapiError::get() << std::endl;
        return false;
    }
    PROCESS_INFORMATION pi = { 0 };
    auto em = SetErrorMode(SEM_NOGPFAULTERRORBOX);
    CreateProcessA(exe_name.str().c_str(), (LPSTR)cmdline_str.c_str(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    SetErrorMode(em);
    CloseHandle(si.hStdOutput);
    // TODO: Use timeout_seconds
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD status = 1;
    GetExitCodeProcess(pi.hProcess, &status);
    if (status != 0)
    {
        DEBUG("Executable exited with non-zero exit status " << status);
        return false;
    }
#else
    Debug_Print([&](auto& os){
        os << "Calling";
        for(const auto& p : args)
            os << " " << p;
        });
    posix_spawn_file_actions_t  file_actions;
    posix_spawn_file_actions_init(&file_actions);
    auto outfile_str = outfile.str();
    if( outfile_str != "" )
    {
        posix_spawn_file_actions_addopen(&file_actions, 1, outfile_str.c_str(), O_CREAT|O_WRONLY|O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&file_actions, 1, 2);
    }

    auto argv = args;
    argv.push_back(nullptr);
    pid_t   pid;
    extern char** environ;
    int rv = posix_spawn(&pid, exe_name.str().c_str(), &file_actions, nullptr, const_cast<char**>(argv.data()), environ);
    if( rv != 0 )
    {
        DEBUG("Error in posix_spawn of " << exe_name << " - " << rv);
        return false;
    }

    posix_spawn_file_actions_destroy(&file_actions);

    int status = -1;
    // NOTE: `alarm(0)` clears any pending alarm, so no need to check before
    // calling
    alarm(timeout_seconds);
    if( waitpid(pid, &status, 0) <= 0 )
    {
        DEBUG(exe_name << " timed out, killing it");
        kill(pid, SIGKILL);
        return false;
    }
    alarm(0);
    if( status != 0 )
    {
        if( WIFEXITED(status) )
            DEBUG(exe_name << " exited with non-zero exit status " << WEXITSTATUS(status));
        else if( WIFSIGNALED(status) )
            DEBUG(exe_name << " was terminated with signal " << WTERMSIG(status));
        else
            DEBUG(exe_name << " terminated for unknown reason, status=" << status);
        return false;
    }
#endif
    return true;
}

Timestamp Timestamp::for_file(const ::helpers::path& path)
{
#if _WIN32
    FILETIME    out;
    auto handle = CreateFile(path.str().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if(handle == INVALID_HANDLE_VALUE) {
        //DEBUG("Can't find " << path);
        return Timestamp::infinite_past();
    }
    if( GetFileTime(handle, NULL, NULL, &out) == FALSE ) {
        //DEBUG("Can't GetFileTime on " << path);
        CloseHandle(handle);
        return Timestamp::infinite_past();
    }
    CloseHandle(handle);
    //DEBUG(Timestamp{out} << " " << path);
    return Timestamp { out };
#else
    struct stat  s;
    if( stat(path.str().c_str(), &s) == 0 )
    {
        return Timestamp { s.st_mtime };
    }
    else
    {
        return Timestamp::infinite_past();
    }
#endif
}


static int giIndentLevel = 0;
void Debug_Print(::std::function<void(::std::ostream& os)> cb)
{
    for(auto i = giIndentLevel; i --; )
        ::std::cout << " ";
    cb(::std::cout);
    ::std::cout << ::std::endl;
}
void Debug_EnterScope(const char* name, dbg_cb_t cb)
{
    for(auto i = giIndentLevel; i --; )
        ::std::cout << " ";
    ::std::cout << ">>> " << name << "(";
    cb(::std::cout);
    ::std::cout << ")" << ::std::endl;
    giIndentLevel ++;
}
void Debug_LeaveScope(const char* name, dbg_cb_t cb)
{
    giIndentLevel --;
    for(auto i = giIndentLevel; i --; )
        ::std::cout << " ";
    ::std::cout << "<<< " << name << ::std::endl;
}
