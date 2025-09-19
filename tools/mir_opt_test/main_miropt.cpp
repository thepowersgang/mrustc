/*
 */
#include <path.h>
#include <target_version.hpp>
#include "test_desc.h"
#include <hir_conv/main_bindings.hpp>
#include <mir/operations.hpp>
#include <mir/main_bindings.hpp>
#include <mir/mir.hpp>
#include <trans/monomorphise.hpp>   // used as a MIR clone
#include <debug_inner.hpp>
#include <mir/visit_crate_mir.hpp>
#include <trans/codegen.hpp>

#ifdef _WIN32
# define NOGDI  // Don't include GDI functions (defines some macros that collide with mrustc ones)
# include <Windows.h>
#else
# include <dirent.h>
# include <sys/stat.h>
#endif

TargetVersion	gTargetVersion = TargetVersion::Rustc1_29;

struct Options
{
    helpers::path   input;
    helpers::path   output;

    bool parse(int argc, char* argv[]);
    void print_usage() const;
    void print_help() const;
};

int main(int argc, char* argv[])
{
    debug_init_phases("MIROPT_DEBUG", {
        "Parse",
        "Bind",
        "Cleanup",
        "Validate",
        "Optimise",
        });

    Options opts;
    if( !opts.parse(argc, argv) )
    {
        return 1;
    }

    ::std::unique_ptr<MirOptTestFile>   file;
    {
        auto ph = DebugTimedPhase("Parse");
        file.reset(new MirOptTestFile( MirOptTestFile::load_from_file(opts.input) ));
    }

    // Run HIR bind on the loaded code (makes sure that it's ready for use)
    {
        auto ph = DebugTimedPhase("Bind");
        ConvertHIR_Bind(*file->m_crate);
    }

    // Run MIR validation BEFORE attempting optimisaion
    {
        auto ph = DebugTimedPhase("Validate");
        MIR_CheckCrate(*file->m_crate);
    }


    // Funally run the tests
    {
        auto ph = DebugTimedPhase("Optimise");
        MIR_OptimiseCrate(*file->m_crate, false);
    }

    TransList   tl;
    {
        auto ph = DebugTimedPhase("Enumerate");
        tl = Trans_Enumerate_Public(*file->m_crate);
    }
    TransOptions    opt;
    opt.mode = "monomir";
    {
        auto ph = DebugTimedPhase("Codegen");
        Trans_Codegen(opts.output, CodegenOutput::Object, opt, *file->m_crate, std::move(tl), "");
    }

    return 0;
}

bool Options::parse(int argc, char* argv[])
{
    for(int i = 1; i < argc; i ++)
    {
        auto arg = helpers::string_view(argv[i], strlen(argv[i]));
        if( arg[0] != '-' )
        {
            if( !this->input.is_valid() )
            {
                this->input = static_cast<std::string>(arg);
            }
            else if( !this->output.is_valid() )
            {
                this->output = static_cast<std::string>(arg);
            }
            else
            {
                this->print_usage();
                return false;
            }
        }
        else if( arg[1] != '-' )
        {
            switch(arg[1])
            {
            case 'h':
                this->print_help();
                exit(0);
            default:
                this->print_usage();
                return false;
            }
        }
        else
        {
            if( arg == "--help" )
            {
                this->print_help();
                exit(0);
            }
            else
            {
                this->print_usage();
                return false;
            }
        }
    }
    if( !this->output.is_valid() ) {
        this->print_usage();
        return false;
    }
    return true;
}

void Options::print_usage() const
{
    std::cerr << "Usage: mir_optimise <input> <output>" << std::endl;
}
void Options::print_help() const
{
    this->print_usage();
}

