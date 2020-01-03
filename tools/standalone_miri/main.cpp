/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * main.cpp
 * - Program entrypoint
 */
#include <iostream>
#include "module_tree.hpp"
#include "value.hpp"
#include <algorithm>
#include <iomanip>
#include "debug.hpp"
#include "miri.hpp"
#include "../../src/common.hpp"

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

int main(int argc, const char* argv[])
{
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
    argv_alloc->write_usize(0 * POINTER_SIZE, Allocation::PTR_BASE);
    argv_alloc->relocations.push_back({ 0 * POINTER_SIZE, RelocationPtr::new_ffi(FFIPointer::new_const_bytes("argv0", opts.infile.c_str(), opts.infile.size() + 1)) });
    for(size_t i = 0; i < opts.args.size(); i ++)
    {
        argv_alloc->write_usize((1 + i) * POINTER_SIZE, Allocation::PTR_BASE);
        argv_alloc->relocations.push_back({ (1 + i) * POINTER_SIZE, RelocationPtr::new_ffi(FFIPointer::new_const_bytes("argv", opts.args[i], ::std::strlen(opts.args[i]) + 1)) });
    }
    LOG_DEBUG("argv_alloc = " << *argv_alloc);

    // Construct argc/argv values
    auto val_argc = Value::new_isize(1 + opts.args.size());
    auto argv_ty = ::HIR::TypeRef(RawType::I8).wrap(TypeWrapper::Ty::Pointer, 0 ).wrap(TypeWrapper::Ty::Pointer, 0);
    auto val_argv = Value::new_pointer(argv_ty, Allocation::PTR_BASE, RelocationPtr::new_alloc(argv_alloc));

    // Catch various exceptions from the interpreter
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


::std::ostream& operator<<(::std::ostream& os, const FmtEscaped& x)
{
    os << ::std::hex;
    for(auto s = x.s; *s != '\0'; s ++)
    {
        switch(*s)
        {
        case '\0':  os << "\\0";    break;
        case '\n':  os << "\\n";    break;
        case '\\':  os << "\\\\";   break;
        case '"':   os << "\\\"";   break;
        default:
            uint8_t v = *s;
            if( v < 0x80 )
            {
                if( v < ' ' || v > 0x7F )
                    os << "\\u{" << ::std::hex << (unsigned int)v << "}";
                else
                    os << v;
            }
            else if( v < 0xC0 )
                ;
            else if( v < 0xE0 )
            {
                uint32_t    val = (uint32_t)(v & 0x1F) << 6;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)(v & 0x3F) << 0;
                os << "\\u{" << ::std::hex << val << "}";
            }
            else if( v < 0xF0 )
            {
                uint32_t    val = (uint32_t)(v & 0x0F) << 12;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)(v & 0x3F) << 6;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)(v & 0x3F) << 0;
                os << "\\u{" << ::std::hex << val << "}";
            }
            else if( v < 0xF8 )
            {
                uint32_t    val = (uint32_t)(v & 0x07) << 18;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)(v & 0x3F) << 12;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)(v & 0x3F) << 6;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)(v & 0x3F) << 0;
                os << "\\u{" << ::std::hex << val << "}";
            }
            break;
        }
    }
    os << ::std::dec;
    return os;
}
