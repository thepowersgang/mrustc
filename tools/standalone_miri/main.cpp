//
//
//
#include <iostream>
#include "module_tree.hpp"
#include "value.hpp"
#include <algorithm>
#include <iomanip>
#include "debug.hpp"
#include "miri.hpp"


struct ProgramOptions
{
    ::std::string   infile;
    //TODO: Architecture file
    //TODO: Loadable FFI descriptions
    //TODO: Logfile

    int parse(int argc, const char* argv[]);
};

int main(int argc, const char* argv[])
{
    ProgramOptions  opts;

    if( opts.parse(argc, argv) )
    {
        return 1;
    }

    auto tree = ModuleTree {};

    tree.load_file(opts.infile);

    auto val_argc = Value( ::HIR::TypeRef{RawType::ISize} );
    ::HIR::TypeRef  argv_ty { RawType::I8 };
    argv_ty.wrappers.push_back(TypeWrapper { TypeWrapper::Ty::Pointer, 0 });
    argv_ty.wrappers.push_back(TypeWrapper { TypeWrapper::Ty::Pointer, 0 });
    auto val_argv = Value(argv_ty);
    val_argc.write_isize(0, 0);
    val_argv.write_usize(0, 0);

    try
    {
        InterpreterThread   root_thread(tree);
        
        ::std::vector<Value>    args;
        args.push_back(::std::move(val_argc));
        args.push_back(::std::move(val_argv));
        Value   rv;
        root_thread.start(tree.find_lang_item("start"), ::std::move(args));
        while( !root_thread.step_one(rv) )
        {
        }

        ::std::cout << rv << ::std::endl;
    }
    catch(const DebugExceptionTodo& /*e*/)
    {
        ::std::cerr << "TODO Hit" << ::std::endl;
        return 1;
    }
    catch(const DebugExceptionError& /*e*/)
    {
        ::std::cerr << "Error encountered" << ::std::endl;
        return 1;
    }

    return 0;
}

int ProgramOptions::parse(int argc, const char* argv[])
{
    bool all_free = false;
    for(int argidx = 1; argidx < argc; argidx ++)
    {
        const char* arg = argv[argidx]; 
        if( arg[0] != '-' || all_free )
        {
            // Free
            if( this->infile == "" )
            {
                this->infile = arg;
            }
            else
            {
                // TODO: Too many free arguments
            }
        }
        else if( arg[1] != '-' )
        {
            // Short
        }
        else if( arg[2] != '\0' )
        {
            // Long
        }
        else
        {
            all_free = true;
        }
    }
    return 0;
}
