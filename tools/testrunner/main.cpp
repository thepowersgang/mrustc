///
///
///
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include "../minicargo/debug.h"
#include "../minicargo/path.h"
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
# define MRUSTC_PATH    "./bin/mrustc"
#endif
#include <algorithm>

struct Options
{
    const char* output_dir = nullptr;
    const char* input_glob = nullptr;

    const char* exceptions_file = nullptr;

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

bool run_executable(const ::helpers::path& file, const ::std::vector<const char*>& args, const ::helpers::path& outfile);

bool run_compiler(const ::helpers::path& source_file, const ::helpers::path& output, const ::std::vector<::std::string>& extra_flags, ::helpers::path libdir={}, bool is_dep=false)
{
    ::std::vector<const char*>  args;
    args.push_back("mrustc");
    args.push_back("-L");
    args.push_back("output");
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

    return run_executable(MRUSTC_PATH, args, logfile);
}

int main(int argc, const char* argv[])
{
    Options opts;
    if( int v = opts.parse(argc, argv) )
    {
        return v;
    }

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
            DEBUG("> " << test_file_path);

            TestDesc    td;
            td.ignore = false;

            do
            {
                ::std::string   line;
                ::std::getline(in, line);
                if( !(line[0] == '/' && line[1] == '/'/* && line[2] == ' '*/) )
                    continue ;
                // TODO Parse a skewer-case ident and check against known set?

                auto start = (line[2] == ' ' ? 3 : 2);

                if( line.substr(start, 10) == "aux-build:" )
                {
                    td.m_pre_build.push_back( line.substr(start+10) );
                }
                else if( line.substr(start, 11) == "ignore-test" )
                {
                    td.ignore = true;
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
        unsigned n_skip = 0;
        unsigned n_cfail = 0;
        unsigned n_fail = 0;
        unsigned n_ok = 0;
        for(const auto& test : tests)
        {
            if( test.ignore )
            {
                DEBUG(">> IGNORE " << test.m_name);
                continue ;
            }
            if( ::std::find(skip_list.begin(), skip_list.end(), test.m_name) != skip_list.end() )
            {
                DEBUG(">> SKIP " << test.m_name);
                n_skip ++;
                continue ;
            }

            DEBUG(">> " << test.m_name);
            auto depdir = outdir / "deps-" + test.m_name.c_str();
            auto outfile = outdir / test.m_name + ".exe";

            auto test_output_ts = Timestamp::for_file(outfile);
            if( test_output_ts < Timestamp::for_file(MRUSTC_PATH) )
            {
                bool pre_build_failed = false;
                for(const auto& file : test.m_pre_build)
                {
                    mkdir(depdir.str().c_str(), 0755);
                    auto infile = input_path / "auxiliary" / file;
                    if( !run_compiler(infile, depdir, {}, depdir, true) )
                    {
                        DEBUG("COMPILE FAIL " << infile << " (dep of " << test.m_name << ")");
                        n_cfail ++;
                        pre_build_failed = true;
                        break;
                    }
                }
                if( pre_build_failed )
                    continue;

                auto compile_logfile = outdir / test.m_name + "-build.log";
                if( !run_compiler(test.m_path, outfile, test.m_extra_flags, depdir) )
                {
                    DEBUG("COMPILE FAIL " << test.m_name);
                    n_cfail ++;
                    continue;
                }
            }
            // - Run the test
            if( !run_executable(outfile, { outfile.str().c_str() }, outdir / test.m_name + ".out") )
            {
                DEBUG("RUN FAIL " << test.m_name);
                n_fail ++;
                continue;
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
                this->usage_short();
                return 1;
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

///
bool run_executable(const ::helpers::path& exe_name, const ::std::vector<const char*>& args, const ::helpers::path& outfile)
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
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = { 0 };
    CreateProcessA(exe_name.str().c_str(), (LPSTR)cmdline_str.c_str(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CloseHandle(si.hStdOutput);
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
    int rv = posix_spawn(&pid, exe_name.str().c_str(), &file_actions, nullptr, const_cast<char**>(argv.data()), environ);
    if( rv != 0 )
    {
        DEBUG("Error in posix_spawn - " << rv);
        return false;
    }

    posix_spawn_file_actions_destroy(&file_actions);

    int status = -1;
    waitpid(pid, &status, 0);
    if( status != 0 )
    {
        if( WIFEXITED(status) )
            DEBUG(exe_name << " exited with non-zero exit status " << WEXITSTATUS(status) << ", see log " << outfile_str);
        else if( WIFSIGNALED(status) )
            DEBUG(exe_name << " was terminated with signal " << WTERMSIG(status) << ", see log " << outfile_str);
        else
            DEBUG(exe_name << " terminated for unknown reason, status=" << status << ", see log " << outfile_str);
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
