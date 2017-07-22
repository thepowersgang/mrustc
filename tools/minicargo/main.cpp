/*
 * Mini version of cargo 
 *
 * main.cpp
 * - Entrypoint
 */

struct ProgramOptions
{
    const char* directory = nullptr;
    const char* outfile = nullptr;

    // Directory containing build script outputs
    const char* override_directory = nullptr;

    const char* output_directory = nullptr;

    int parse(int argc, const char* argv[]);
};

int main(int argc, const char* argv[])
{
    ProgramOptions  opts;
    if( opts.parse(argc, argv) ) {
        return 1;
    }

    // 1. Load the Cargo.toml file from the passed directory
    // 2. Recursively load dependency manifests
    // 3. Generate makefile for all dependencies

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
            else if( !this->outfile ) {
                this->outfile = arg;
            }
            else {
            }
        }
        else if( argv[1] != '-' )
        {
            // Short arguments
        }
        else if( argv[1] == '\0' )
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
                //this->build_script_override_dir = argv[++i];
            }
            else {
                ::std::cerr << "Unknown flag " << arg << ::std::endl;
            }
        }
    }

    if( !this->directory || !this->outfile )
    {
        usage();
        exit(1);
    }

    return 0;
}

