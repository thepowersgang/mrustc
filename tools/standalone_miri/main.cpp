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
    //::std::string   archname;
    //TODO: Loadable FFI descriptions
    //::std::vector<const char*>  ffi_api_files;
    //TODO: Logfile
    //::std::string   logfile;
    ::std::vector<const char*>  args;

    int parse(int argc, const char* argv[]);
    void show_help(const char* prog) const;
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

    // Create argc/argv based on input arguments
    val_argc.write_isize(0, 1 + opts.args.size());
    auto argv_alloc = Allocation::new_alloc((1 + opts.args.size()) * POINTER_SIZE);
    argv_alloc->write_usize(0 * POINTER_SIZE, 0);
    argv_alloc->relocations.push_back({ 0 * POINTER_SIZE, RelocationPtr::new_ffi(FFIPointer { "", (void*)(opts.infile.c_str()), opts.infile.size() + 1 }) });
    for(size_t i = 0; i < opts.args.size(); i ++)
    {
        argv_alloc->write_usize((1 + i) * POINTER_SIZE, 0);
        argv_alloc->relocations.push_back({ (1 + i) * POINTER_SIZE, RelocationPtr::new_ffi({ "", (void*)(opts.args[0]), ::std::strlen(opts.args[0]) + 1 }) });
    }
    val_argv.write_usize(0, 0);
    val_argv.allocation->relocations.push_back({ 0 * POINTER_SIZE, RelocationPtr::new_alloc(argv_alloc) });

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
                this->args.push_back(arg);
            }
        }
        else if( arg[1] != '-' )
        {
            // Short
            if( arg[2] != '\0' ) {
                // Error?
            }
            switch(arg[1])
            {
            case 'h':
                this->show_help(argv[0]);
                exit(0);
            default:
                // TODO: Error
                break;
            }
        }
        else if( arg[2] != '\0' )
        {
            // Long
            if( ::std::strcmp(arg, "--help") == 0 ) {
                this->show_help(argv[0]);
                exit(0);
            }
            //else if( ::std::strcmp(arg, "--api") == 0 ) {
            //}
            else {
                // TODO: Error
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
