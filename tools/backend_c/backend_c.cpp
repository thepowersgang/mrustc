/*
 * mrustc - Standalone C backend
 */
#include <string>
#include <iostream>
#include <module_tree.hpp>

struct Opts
{
    ::std::string   infile;

    int parse(int argc, const char* argv[]);
    void show_help(const char* prog) const;
};

int main(int argc, const char* argv[])
{
    Opts    opts;
    if( int rv = opts.parse(argc, argv) )
        return rv;

    ModuleTree	tree;

    // 1. Load the tree
    try
    {
        tree.load_file(opts.infile);
        tree.validate();
    }
    catch(const DebugExceptionTodo& /*e*/)
    {
        ::std::cerr << "Loading: TODO Hit" << ::std::endl;
        return 1;
    }
    catch(const DebugExceptionError& /*e*/)
    {
        ::std::cerr << "Loading: Error encountered" << ::std::endl;
        return 1;
    }

    // 2. Emit C code
    //  - Emit types
    //    > Recursively enumerate all types (determining the required emit ordering)
    //  - Emit function/static prototypes
    tree.iterate_statics([&](RcString name, const Static& s) {

        });
    tree.iterate_functions([&](RcString name, const Function& f) {
        });
    //  - Emit statics
    tree.iterate_statics([&](RcString name, const Static& s) {
        });
    //  - Emit functions
    tree.iterate_functions([&](RcString name, const Function& f) {
        });

    return 0;
}

int Opts::parse(int argc, const char* argv[])
{
    bool all_free = false;

    for(int argidx = 1; argidx < argc; argidx ++)
    {
        const char* arg = argv[argidx]; 
        if( arg[0] != '-' || all_free )
        {
            // Free arguments
            // - First is the input file
            if( this->infile == "" )
            {
                this->infile = arg;
            }
            else
            {
                ::std::cerr << "Unexpected option -" << arg << ::std::endl;
                return 1;
            }
        }
        else if( arg[1] != '-' )
        {
            // Short arguments
            if( arg[2] != '\0' ) {
                // Error?
                ::std::cerr << "Unexpected option " << arg << ::std::endl;
                return 1;
            }
            switch(arg[1])
            {
            case 'h':
                this->show_help(argv[0]);
                exit(0);
            default:
                ::std::cerr << "Unexpected option -" << arg[1] << ::std::endl;
                return 1;
            }
        }
        else if( arg[2] != '\0' )
        {
            // Long
            if( ::std::strcmp(arg, "--help") == 0 ) {
                this->show_help(argv[0]);
                exit(0);
            }
            //else if( ::std::strcmp(arg, "--target") == 0 ) {
            //}
            else {
                ::std::cerr << "Unexpected option " << arg << ::std::endl;
                return 1;
            }
        }
        else
        {
            all_free = true;
        }
    }

    if( this->infile == "" )
    {
        this->show_help(argv[0]);
        return 1;
    }

    return 0;
}

void Opts::show_help(const char* prog) const
{
    ::std::cout << "USAGE: " << prog << " <infile> <... args>" << ::std::endl;
}
