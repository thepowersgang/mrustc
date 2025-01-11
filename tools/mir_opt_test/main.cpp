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
    helpers::path   test_dir;

    // TODO: List of test globs
    std::vector<std::string>    filters;

    bool parse(int argc, char* argv[]);
    void print_usage() const;
    void print_help() const;
};

namespace {
    MIR::FunctionPointer clone_mir(const StaticTraitResolve& resolve, const MIR::FunctionPointer& fcn);
    bool compare_mir(const MIR::Function& exp, const MIR::Function& have, const HIR::SimplePath& path);
}

int main(int argc, char* argv[])
{
    debug_init_phases("MIRTEST_DEBUG", {
        "Parse",
        "Cleanup",
        "Validate",
        "Borrow Check",
        "Run Tests",
        });

    Options opts;
    if( !opts.parse(argc, argv) )
    {
        return 1;
    }

    ::std::vector<MirOptTestFile>   test_files;
    {
        auto ph = DebugTimedPhase("Parse");
#ifdef _WIN32
        WIN32_FIND_DATA find_data;
        auto mask = opts.test_dir / "*.rs";
        HANDLE find_handle = FindFirstFile( mask.str().c_str(), &find_data );
        if( find_handle == INVALID_HANDLE_VALUE ) {
            ::std::cerr << "Unable to find files matching " << mask << ::std::endl;
            return 1;
        }
        do
        {
            auto test_file_path = opts.test_dir / find_data.cFileName;
#else
        auto* dp = opendir(opts.test_dir.str().c_str());
        if( dp == nullptr ) {
            ::std::cerr << "Unable to open directory " << opts.test_dir << ::std::endl;
        }
        while( const auto* dent = readdir(dp) )
        {
            if( dent->d_name[0] == '.' )
                continue ;
            auto test_file_path = opts.test_dir / dent->d_name;
            struct stat sb;
            stat(test_file_path.str().c_str(), &sb);
            if( (sb.st_mode & S_IFMT) != S_IFREG) {
                continue ;
            }
#endif
            try
            {
                test_files.push_back( MirOptTestFile::load_from_file(test_file_path) );
            }
            catch(const ::std::exception& e)
            {
                ::std::cerr << "Exception: " << e.what() << " when loading test " << test_file_path << ::std::endl;
            }
#ifndef _WIN32
        }
        closedir(dp);
#else
        } while( FindNextFile(find_handle, &find_data) );
        FindClose(find_handle);
#endif
    }

    // Run HIR bind on the loaded code (makes sure that it's ready for use)
    {
        auto ph = DebugTimedPhase("Cleanup");
        for(auto& f : test_files)
        {
            ConvertHIR_Bind(*f.m_crate);
        }
    }

    // Run MIR validation BEFORE attempting optimisaion
    {
        auto ph = DebugTimedPhase("Validate");
        for(auto& f : test_files)
        {
            MIR_CheckCrate(*f.m_crate);
        }
    }

    {
        auto ph = DebugTimedPhase("Borrow Check");
#if 1
        for(auto& f : test_files)
        {
            for(const auto& test : f.m_tests)
            {
                if(!opts.filters.empty())
                {
                    bool found = false;
                    for(const auto& f : opts.filters)
                    {
                        if( f == test.input_function.components().back() )
                        {
                            found = true;
                            break;
                        }
                    }
                    if(!found)
                    {
                        continue;
                    }
                }
                const auto& in_fcn = f.m_crate->get_function_by_path(Span(), test.input_function);
                StaticTraitResolve  resolve(*f.m_crate);
                // TODO: Generics?
                MIR_BorrowCheck(resolve, test.input_function, const_cast<MIR::Function&>(*in_fcn.m_code.m_mir), in_fcn.m_args, in_fcn.m_return);
            }
        }
#else
        for(auto& f : test_files)
        {
            auto& crate = *f.m_crate;
            ::MIR::OuterVisitor    ov(crate, [&](const auto& res, const HIR::ItemPath& p, auto& expr, const auto& args, const auto& ty) {
                    MIR_BorrowCheck(res, p, *expr.m_mir, args, ty);
                }
            );
            ov.visit_crate( crate );
        }
#endif
    }

    // Funally run the tests
    {
        auto ph = DebugTimedPhase("Run Tests");
        for(auto& f : test_files)
        {
            for(const auto& test : f.m_tests)
            {
                if(!opts.filters.empty())
                {
                    bool found = false;
                    for(const auto& f : opts.filters)
                    {
                        if( f == test.input_function.components().back() )
                        {
                            found = true;
                            break;
                        }
                    }
                    if(!found)
                    {
                        continue;
                    }
                }
                const auto& in_fcn = f.m_crate->get_function_by_path(Span(), test.input_function);
                const auto& exp_mir = *f.m_crate->get_function_by_path(Span(), test.output_template_function).m_code.m_mir;

                StaticTraitResolve  resolve(*f.m_crate);
                // TODO: Generics?
                auto cloned_mir = clone_mir(resolve, in_fcn.m_code.m_mir);

                MIR_Optimise(resolve, test.input_function, *cloned_mir, in_fcn.m_args, in_fcn.m_return);

                auto p = HIR::SimplePath(RcString(f.m_filename), test.input_function.components());
                if( !compare_mir(exp_mir, *cloned_mir, p) )
                {
                    MIR_Dump_Fcn(std::cerr, *cloned_mir);
                }
            }
        }
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
            if( !this->test_dir.is_valid() )
            {
                this->test_dir = static_cast<std::string>(arg);
            }
            else
            {
                this->filters.push_back( arg );
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
    return true;
}

void Options::print_usage() const
{
    std::cerr << "Usage: mir_opt_test <dir> [tests ...]" << std::endl;
}
void Options::print_help() const
{
    this->print_usage();
}

namespace {
    MIR::FunctionPointer clone_mir(const StaticTraitResolve& resolve, const MIR::FunctionPointer& fcn)
    {
        return Trans_Monomorphise(resolve, {}, fcn);
    }
    bool compare_mir(const MIR::Function& exp, const MIR::Function& have, const HIR::SimplePath& path)
    {
        if( exp.locals.size() != have.locals.size() ) {
            std::cerr << path << " Mismatch in local count: exp " << exp.locals.size() << " != " << have.locals.size() << std::endl;
            return false;
        }
        for(size_t i = 0; i < exp.locals.size(); i ++)
        {
            if( exp.locals[i] != have.locals[i] )
            {
                std::cerr << path << " Local " << i << " mismatch: exp " << exp.locals[i] << " != " << have.locals[i] << std::endl;
                return false;
            }
        }

        if( exp.drop_flags != have.drop_flags ) {
            std::cerr << path << " Mismatch in drop flags" << std::endl;
            return false;
        }

        if( exp.blocks.size() != have.blocks.size() ) {
            std::cerr << path << " Mismatch in block count: exp " << exp.blocks.size() << " != " << have.blocks.size() << std::endl;
            return false;
        }
        for(size_t bb_idx = 0; bb_idx < exp.blocks.size(); bb_idx ++)
        {
            const auto& bb_e = exp.blocks[bb_idx];
            const auto& bb_h = have.blocks[bb_idx];

            if( bb_e.statements.size() != bb_h.statements.size() ) {
                std::cerr << path << " BB" << bb_idx << " Mismatch in statement count: exp " << bb_e.statements.size() << " != " << bb_h.statements.size() << std::endl;
                return false;
            }
            for(size_t stmt_idx = 0; stmt_idx < bb_e.statements.size(); stmt_idx ++)
            {
                const auto& stmt_e = bb_e.statements[stmt_idx];
                const auto& stmt_h = bb_h.statements[stmt_idx];
                if( stmt_e != stmt_h ) {
                    std::cerr << path << " BB" << bb_idx << "/" << stmt_idx << " Mismatched statements: exp " << stmt_e << " != " << stmt_h << std::endl;
                    return false;
                }
            }

            if( bb_e.terminator != bb_h.terminator )
            {
                std::cerr << path << " BB" << bb_idx << "/TERM Mismatched terminator: exp " << bb_e.terminator << " != " << bb_h.terminator << std::endl;
                return false;
            }
        }
        std::cerr << path << " EQUAL" << std::endl;
        return true;
    }
}
