/*
 * Mini version of cargo 
 *
 * main.cpp
 * - Entrypoint
 */
#include <iostream>
#include <cstring>  // strcmp
#include <map>
#include "debug.h"
#include "manifest.h"
#include "helpers.h"
#include "repository.h"

extern void MiniCargo_Build(const PackageManifest& manifest, ::helpers::path override_path);

struct ProgramOptions
{
    const char* directory = nullptr;
    const char* outfile = nullptr;

    // Directory containing build script outputs
    const char* override_directory = nullptr;

    const char* output_directory = nullptr;

    const char* vendor_dir = nullptr;

    int parse(int argc, const char* argv[]);
    void usage() const;
};

int main(int argc, const char* argv[])
{
    ProgramOptions  opts;
    if( opts.parse(argc, argv) ) {
        return 1;
    }

    try
    {
        // Load package database
        Repository repo;
        // TODO: load repository from a local cache
        if( opts.vendor_dir )
        {
            repo.load_vendored(opts.vendor_dir);
        }

        // 1. Load the Cargo.toml file from the passed directory
        auto dir = ::helpers::path(opts.directory ? opts.directory : ".");
        auto m = PackageManifest::load_from_toml( dir / "Cargo.toml" );

        m.load_dependencies(repo);

        // 3. Build dependency tree
        MiniCargo_Build(m, opts.override_directory ? ::helpers::path(opts.override_directory) : ::helpers::path() );
    }
    catch(const ::std::exception& e)
    {
        ::std::cerr << "EXCEPTION: " << e.what() << ::std::endl;
        ::std::cout << "Press enter to exit..." << ::std::endl;
        ::std::cin.get();
        return 1;
    }

    ::std::cout << "Press enter to exit..." << ::std::endl;
    ::std::cin.get();
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
        }
        else if( arg[1] == '\0' )
        {
            all_free = true;
        }
        else
        {
            // Long arguments
            if( ::std::strcmp(arg, "--script-overrides") == 0 ) {
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
            else {
                ::std::cerr << "Unknown flag " << arg << ::std::endl;
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
        ;
}


void Debug_Print(::std::function<void(::std::ostream& os)> cb)
{
    cb(::std::cout);
    ::std::cout << ::std::endl;
}

