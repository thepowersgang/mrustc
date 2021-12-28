/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * main.cpp
 * - Program entrypoint
 */
#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include "module_tree.hpp"
#include "value.hpp"
#include <algorithm>
#include <iomanip>
#include "debug.hpp"
#include "miri.hpp"
#include "../../src/common.hpp"
#include <target_version.hpp>

struct ProgramOptions
{
    ::std::string   infile;
    //TODO: Architecture file
    //::std::string   archname;
    //TODO: Loadable FFI descriptions
    //::std::vector<const char*>  ffi_api_files;

    // Output logfile
    ::std::string   logfile;
    // Arguments for the program
    ::std::vector<const char*>  args;

    int parse(int argc, const char* argv[]);
    void show_help(const char* prog) const;
};

TargetVersion	gTargetVersion = TargetVersion::Rustc1_29;

int main(int argc, const char* argv[])
{
    if( const auto* a = getenv("MRUSTC_TARGET_VER") )
    {
        /* */if( strcmp(a, "1.19") == 0 ) { gTargetVersion = TargetVersion::Rustc1_19; }
        else if( strcmp(a, "1.29") == 0 ) { gTargetVersion = TargetVersion::Rustc1_29; }
        else if( strcmp(a, "1.39") == 0 ) { gTargetVersion = TargetVersion::Rustc1_39; }
        else if( strcmp(a, "1.54") == 0 ) { gTargetVersion = TargetVersion::Rustc1_54; }
        else {
            std::cerr << "Unknown target version string: '" << a << "'" << std::endl;
            exit(1);
        }
    }

    ProgramOptions  opts;

    if( opts.parse(argc, argv) )
    {
        return 1;
    }

    // Configure logging
    if( opts.logfile != "" )
    {
        DebugSink::set_output_file(opts.logfile);
    }

    // Load HIR tree
    auto tree = ModuleTree {};
    try
    {
        tree.load_file(opts.infile);
        tree.validate();
    }
    catch(const DebugExceptionTodo& /*e*/)
    {
        ::std::cerr << "TODO Hit" << ::std::endl;
        if(opts.logfile != "")
        {
            ::std::cerr << "- See '" << opts.logfile << "' for details" << ::std::endl;
        }
        return 1;
    }
    catch(const DebugExceptionError& /*e*/)
    {
        ::std::cerr << "Error encountered" << ::std::endl;
        if(opts.logfile != "")
        {
            ::std::cerr << "- See '" << opts.logfile << "' for details" << ::std::endl;
        }
        return 1;
    }


    // Create argc/argv based on input arguments
    auto argv_alloc = Allocation::new_alloc((1 + opts.args.size()) * POINTER_SIZE, "argv");
    argv_alloc->write_ptr_ofs( 0 * POINTER_SIZE, 0, RelocationPtr::new_ffi(FFIPointer::new_const_bytes("argv0", opts.infile.c_str(), opts.infile.size() + 1)) );
    for(size_t i = 0; i < opts.args.size(); i ++)
    {
        argv_alloc->write_ptr_ofs( (1 + i) * POINTER_SIZE, 0, RelocationPtr::new_ffi(FFIPointer::new_const_bytes("argv", opts.args[i], ::std::strlen(opts.args[i]) + 1)) );
    }
    LOG_DEBUG("argv_alloc = " << *argv_alloc);

    // Construct argc/argv values
    auto val_argc = Value::new_isize(1 + opts.args.size());
    auto argv_ty = ::HIR::TypeRef(RawType::I8).wrap(TypeWrapper::Ty::Pointer, 0 ).wrap(TypeWrapper::Ty::Pointer, 0);
    auto val_argv = Value::new_pointer_ofs(argv_ty, 0, RelocationPtr::new_alloc(argv_alloc));

    // Catch various exceptions from the interpreter
    try
    {
        GlobalState global(tree);
        InterpreterThread   root_thread(global);

        ::std::vector<Value>    args;
        args.push_back(::std::move(val_argc));
        args.push_back(::std::move(val_argv));
        Value   rv;
        root_thread.start("main#", ::std::move(args));
        while( !root_thread.step_one(rv) )
        {
        }

        LOG_NOTICE("Return code: " << rv);
    }
    catch(const DebugExceptionTodo& /*e*/)
    {
        ::std::cerr << "TODO Hit" << ::std::endl;
        if(opts.logfile != "")
        {
            ::std::cerr << "- See '" << opts.logfile << "' for details" << ::std::endl;
        }
        return 1;
    }
    catch(const DebugExceptionError& /*e*/)
    {
        ::std::cerr << "Error encountered" << ::std::endl;
        if(opts.logfile != "")
        {
            ::std::cerr << "- See '" << opts.logfile << "' for details" << ::std::endl;
        }
        return 1;
    }
    catch(const std::exception& e)
    {
        ::std::cerr << "Unexpected exception: " << e.what() << ::std::endl;
        if(opts.logfile != "")
        {
            ::std::cerr << "- See '" << opts.logfile << "' for details" << ::std::endl;
        }
        return 1;
    }

    return 0;
}

int ProgramOptions::parse(int argc, const char* argv[])
{
    bool all_free = false;
    // TODO: use getopt? POSIX only
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
                // Any subsequent arguments are passed to the taget
                this->args.push_back(arg);
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
            else if( ::std::strcmp(arg, "--logfile") == 0 ) {
                if( argidx + 1 == argc ) {
                    ::std::cerr << "Option " << arg << " requires an argument" << ::std::endl;
                    return 1;
                }
                const char* opt = argv[++argidx];
                this->logfile = opt;
            }
            //else if( ::std::strcmp(arg, "--api") == 0 ) {
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
    return 0;
}

void ProgramOptions::show_help(const char* prog) const
{
    ::std::cout << "USAGE: " << prog << " <infile> <... args>" << ::std::endl;
}
