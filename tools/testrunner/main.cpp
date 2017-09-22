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
#endif

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
};

bool run_executable(const ::helpers::path& file, const ::std::vector<const char*>& args);

bool run_compiler(const ::helpers::path& source_file, const ::helpers::path& output, ::helpers::path libdir={})
{
    ::std::vector<const char*>  args;
    args.push_back("mrustc");
    if(libdir.is_valid())
    {
        args.push_back("-L");
        args.push_back(libdir.str().c_str());
    }
    args.push_back(source_file.str().c_str());
    args.push_back("--out-dir");
    args.push_back(output.str().c_str());

    run_executable(MRUSTC_PATH, args);

    return false;
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
        auto* dp = opendir(path.str().c_str());
        if( dp == nullptr )
            throw ::std::runtime_error(::format( "Unable to open vendor directory '", path, "'" ));
        while( const auto* dent = readdir(dp) )
        {
            auto test_file_path = path / dent->d_name;
#endif
            ::std::ifstream in(test_file_path.str());
            if(!in.good())
                continue ;
            DEBUG("> " << test_file_path);

            TestDesc    td;

            do
            {
                ::std::string   line;
                in >> line;
                if( !(line[0] == '/' && line[1] == '/' && line[2] == ' ') )
                    continue ;

                if( line.substr(3, 10) == "aux-build" )
                {
                    TODO("aux_build " << line);
                }
            } while( !in.eof() );

            td.m_name = test_file_path.basename();
            td.m_name.pop_back();
            td.m_name.pop_back();
            td.m_name.pop_back();
            td.m_path = test_file_path;


            auto test = td;

            tests.push_back(td);
            DEBUG(">> " << test.m_name);
            auto depdir = outdir / "deps-" + test.m_name.c_str();

            for(const auto& file : test.m_pre_build)
            {
                run_compiler(file, depdir);
            }
            run_compiler(test.m_path, outdir, outdir);
            // - Run the test
            run_executable(outdir / test.m_name, {});
#ifndef _WIN32
        }
        closedir(dp);
#else
        } while( FindNextFile(find_handle, &find_data) );
        FindClose(find_handle);
#endif
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
bool run_executable(const ::helpers::path& exe_name, const ::std::vector<const char*>& args)
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
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdError = GetStdHandle(STD_OUTPUT_HANDLE);
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
    return false;
#endif
    return true;
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
