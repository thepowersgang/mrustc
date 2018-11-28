/*
 * mrustc "minicargo" (minimal cargo clone)
 * - By John Hodge (Mutabah)
 *
 * main.cpp
 * - Entrypoint
 */
#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <cstring>  // strcmp
#include <map>
#include <debug.h>
#include "manifest.h"
#include <helpers.h>
#include "repository.h"
#include "build.h"

struct ProgramOptions
{
    // Input (package) directory
    const char* directory = nullptr;

    // Directory containing build script outputs
    const char* override_directory = nullptr;

    // Directory containing "vendored" (packaged) copies of packages
    const char* vendor_dir = nullptr;

    // Output/build directory
    const char* output_directory = nullptr;

    // Emit Monomorphised MIR instead of C
    bool emit_mmir = false;

    // Target name (if null, defaults to host)
    const char* target = nullptr;

    // Library search directories
    ::std::vector<const char*>  lib_search_dirs;

    // Number of build jobs to run at a time
    unsigned build_jobs = 1;

    // Pause for user input before quitting (useful for MSVC debugging)
    bool pause_before_quit = false;

    int parse(int argc, const char* argv[]);
    void usage() const;
    void help() const;
};

int main(int argc, const char* argv[])
{
    ProgramOptions  opts;
    if( opts.parse(argc, argv) ) {
        return 1;
    }

    {
        Debug_DisablePhase("Load Repository");
        Debug_DisablePhase("Load Root");
        Debug_DisablePhase("Load Dependencies");
        Debug_DisablePhase("Enumerate Build");
        Debug_DisablePhase("Run Build");

        if( const char* e = getenv("MINICARGO_DEBUG") )
        {
            while( *e )
            {
                const char* colon = ::std::strchr(e, ':');
                size_t len = colon ? colon - e : ::std::strlen(e);

                Debug_EnablePhase(::std::string(e, len).c_str());

                if( colon )
                    e = colon + 1;
                else
                    e = e + len;
            }
        }
    }

    try
    {
        Debug_SetPhase("Load Repository");
        // Load package database
        Repository repo;
        // TODO: load repository from a local cache
        if( opts.vendor_dir )
        {
            repo.load_vendored(opts.vendor_dir);
        }

        auto bs_override_dir = opts.override_directory ? ::helpers::path(opts.override_directory) : ::helpers::path();

        // 1. Load the Cargo.toml file from the passed directory
        Debug_SetPhase("Load Root");
        auto dir = ::helpers::path(opts.directory ? opts.directory : ".");
        auto m = PackageManifest::load_from_toml( dir / "Cargo.toml" );

        // 2. Load all dependencies
        Debug_SetPhase("Load Dependencies");
        m.load_dependencies(repo, !bs_override_dir.is_valid());

        // 3. Build dependency tree and build program.
        BuildOptions    build_opts;
        build_opts.build_script_overrides = ::std::move(bs_override_dir);
        build_opts.output_dir = opts.output_directory ? ::helpers::path(opts.output_directory) : ::helpers::path("output");
        build_opts.lib_search_dirs.reserve(opts.lib_search_dirs.size());
        build_opts.emit_mmir = opts.emit_mmir;
        build_opts.target_name = opts.target;
        for(const auto* d : opts.lib_search_dirs)
            build_opts.lib_search_dirs.push_back( ::helpers::path(d) );
        Debug_SetPhase("Enumerate Build");
        auto build_list = BuildList(m, build_opts);
        Debug_SetPhase("Run Build");
        if( !build_list.build(::std::move(build_opts), opts.build_jobs) )
        {
            ::std::cerr << "BUILD FAILED" << ::std::endl;
            if(opts.pause_before_quit) {
                ::std::cout << "Press enter to exit..." << ::std::endl;
                ::std::cin.get();
            }
            return 1;
        }
    }
    catch(const ::std::exception& e)
    {
        ::std::cerr << "EXCEPTION: " << e.what() << ::std::endl;
        if(opts.pause_before_quit) {
            ::std::cout << "Press enter to exit..." << ::std::endl;
            ::std::cin.get();
        }
        return 1;
    }

    if(opts.pause_before_quit) {
        ::std::cout << "Press enter to exit..." << ::std::endl;
        ::std::cin.get();
    }
    return 0;
}

int ProgramOptions::parse(int argc, const char* argv[])
{
    bool all_free = false;
    for(int i = 1; i < argc; i++)
    {
        const char* arg = argv[i];
        if( arg[0] != '-' || all_free )
        {
            // Free arguments
            if( !this->directory ) {
                this->directory = arg;
            }
            else {
            }
        }
        else if( arg[1] != '-' )
        {
            // Short arguments
            switch(arg[1])
            {
            case 'L':
                if(i+1 == argc) {
                    ::std::cerr << "Flag " << arg << " takes an argument" << ::std::endl;
                    return 1;
                }
                this->lib_search_dirs.push_back(argv[++i]);
                break;
            case 'o':
                if(i+1 == argc) {
                    ::std::cerr << "Flag " << arg << " takes an argument" << ::std::endl;
                    return 1;
                }
                this->output_directory = argv[++i];
                break;
            case 'j':
                if( i+1 == argc || argv[i+1][0] == '-' ) {
                    // TODO: Detect number of CPU cores and use that many jobs
                    break;
                }
                this->build_jobs = ::std::strtol(argv[++i], nullptr, 10);
                break;
            case 'Z':
                if( arg[2] != '\0' ) {
                    arg = arg + 2;
                }
                else {
                    if(i+1 == argc) {
                        ::std::cerr << "Flag " << arg << " takes an argument" << ::std::endl;
                        return 1;
                    }
                    arg = argv[++i];
                }
                if( ::std::strcmp(arg, "emit-mmir") == 0 ) {
                    this->emit_mmir = true;
                }
                else {
                    ::std::cerr << "Unknown debug option -Z " << arg << ::std::endl;
                    return 1;
                }
                break;
            case 'n':
                this->build_jobs = 0;
                break;
            case 'h':
                this->help();
                exit(1);
            default:
                ::std::cerr << "Unknown flag -" << arg[1] << ::std::endl;
                return 1;
            }
        }
        else if( arg[2] == '\0' )
        {
            all_free = true;
        }
        else
        {
            // Long arguments
            if( ::std::strcmp(arg, "--help") == 0 ) {
                this->help();
                exit(1);
            }
            else if( ::std::strcmp(arg, "--script-overrides") == 0 ) {
                if(i+1 == argc) {
                    ::std::cerr << "Flag " << arg << " takes an argument" << ::std::endl;
                    return 1;
                }
                this->override_directory = argv[++i];
            }
            else if( ::std::strcmp(arg, "--vendor-dir") == 0 ) {
                if(i+1 == argc) {
                    ::std::cerr << "Flag " << arg << " takes an argument" << ::std::endl;
                    return 1;
                }
                this->vendor_dir = argv[++i];
            }
            else if( ::std::strcmp(arg, "--output-dir") == 0 ) {
                if(i+1 == argc) {
                    ::std::cerr << "Flag " << arg << " takes an argument" << ::std::endl;
                    return 1;
                }
                this->output_directory = argv[++i];
            }
            else if( ::std::strcmp(arg, "--target") == 0 ) {
                if(i+1 == argc) {
                    ::std::cerr << "Flag " << arg << " takes an argument" << ::std::endl;
                    return 1;
                }
                this->target = argv[++i];
            }
            else if( ::std::strcmp(arg, "--pause") == 0 ) {
                this->pause_before_quit = true;
            }
            else {
                ::std::cerr << "Unknown flag " << arg << ::std::endl;
                return 1;
            }
        }
    }

    if( !this->directory /*|| !this->outfile*/ )
    {
        usage();
        exit(1);
    }

    return 0;
}

void ProgramOptions::usage() const
{
    ::std::cerr
        << "Usage: minicargo <package dir>" << ::std::endl
        << ::std::endl
        << "   Build a cargo package using mrustc. Point it at a directory containing Cargo.toml" << ::std::endl
        ;
}

void ProgramOptions::help() const
{
    usage();
    ::std::cerr
        << ::std::endl
        << "--help, -h  : Show this help text\n"
        << "--script-overrides <dir> : Directory containing <package>.txt files containing the build script output\n"
        << "--vendor-dir <dir>       : Directory containing vendored packages (from `cargo vendor`)\n"
        << "--output-dir,-o <dir>    : Specify the compiler output directory\n"
        << "-L <dir>                 : Search for pre-built crates (e.g. libstd) in the specified directory\n"
        << "-j <count>               : Run at most <count> build tasks at once (default is to run only one)\n"
        << "-n                       : Don't build any packages, just list the packages that would be built\n"
        ;
}
