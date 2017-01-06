/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * main.cpp
 * - Compiler Entrypoint
 */
#include <iostream>
#include <iomanip>
#include <string>
#include <set>
#include "parse/lex.hpp"
#include "parse/parseerror.hpp"
#include "ast/ast.hpp"
#include "ast/crate.hpp"
#include <serialiser_texttree.hpp>
#include <cstring>
#include <main_bindings.hpp>
#include "resolve/main_bindings.hpp"
#include "hir/main_bindings.hpp"
#include "hir_conv/main_bindings.hpp"
#include "hir_typeck/main_bindings.hpp"
#include "hir_expand/main_bindings.hpp"
#include "mir/main_bindings.hpp"
#include "trans/main_bindings.hpp"

#include "expand/cfg.hpp"

int g_debug_indent_level = 0;
bool g_debug_enabled = true;
::std::string g_cur_phase;
::std::set< ::std::string>    g_debug_disable_map;

void init_debug_list()
{
    g_debug_disable_map.insert( "Parse" );
    g_debug_disable_map.insert( "LoadCrates" );
    g_debug_disable_map.insert( "Expand" );

    g_debug_disable_map.insert( "Resolve Use" );
    g_debug_disable_map.insert( "Resolve Index" );
    g_debug_disable_map.insert( "Resolve Absolute" );

    g_debug_disable_map.insert( "HIR Lower" );

    g_debug_disable_map.insert( "Resolve Type Aliases" );
    g_debug_disable_map.insert( "Resolve Bind" );
    g_debug_disable_map.insert( "Resolve UFCS paths" );
    g_debug_disable_map.insert( "Resolve HIR Markings" );
    g_debug_disable_map.insert( "Constant Evaluate" );

    g_debug_disable_map.insert( "Typecheck Outer");
    g_debug_disable_map.insert( "Typecheck Expressions" );

    g_debug_disable_map.insert( "Expand HIR Annotate" );
    g_debug_disable_map.insert( "Expand HIR Closures" );
    g_debug_disable_map.insert( "Expand HIR Calls" );
    g_debug_disable_map.insert( "Expand HIR VTables" );
    g_debug_disable_map.insert( "Expand HIR Reborrows" );
    g_debug_disable_map.insert( "Expand HIR ErasedType" );
    g_debug_disable_map.insert( "Typecheck Expressions (validate)" );

    g_debug_disable_map.insert( "Dump HIR" );
    g_debug_disable_map.insert( "Lower MIR" );
    g_debug_disable_map.insert( "MIR Validate" );
    g_debug_disable_map.insert( "Dump MIR" );
    g_debug_disable_map.insert( "Constant Evaluate Full" );
    g_debug_disable_map.insert( "MIR Cleanup" );
    g_debug_disable_map.insert( "MIR Optimise" );

    g_debug_disable_map.insert( "HIR Serialise" );
    g_debug_disable_map.insert( "Trans Enumerate" );
    g_debug_disable_map.insert( "Trans Codegen" );

    // Mutate this map using an environment variable
    const char* debug_string = ::std::getenv("MRUSTC_DEBUG");
    if( debug_string )
    {
        while( debug_string[0] )
        {
            const char* end = strchr(debug_string, ':');

            if( end ) {
                ::std::string   s { debug_string, end };
                // TODO: Emit a warning when this name wasn't in the map?
                g_debug_disable_map.erase( s );
                debug_string = end + 1;
            }
            else {
                g_debug_disable_map.erase( debug_string );
                break;
            }
        }
    }
}
bool debug_enabled_update() {
    if( g_debug_disable_map.count(g_cur_phase) != 0 ) {
        return false;
    }
    else {
        return true;
    }
}
bool debug_enabled()
{
    return g_debug_enabled;
}
::std::ostream& debug_output(int indent, const char* function)
{
    return ::std::cout << g_cur_phase << "- " << RepeatLitStr { " ", indent } << function << ": ";
}

struct ProgramParams
{
    enum eLastStage {
        STAGE_PARSE,
        STAGE_EXPAND,
        STAGE_RESOLVE,
        STAGE_TYPECK,
        STAGE_BORROWCK,
        STAGE_MIR,
        STAGE_ALL,
    } last_stage = STAGE_ALL;

    ::std::string   infile;
    ::std::string   outfile;
    ::std::string   output_dir = "";
    const char *crate_path = ".";

    ::AST::Crate::Type  crate_type = ::AST::Crate::Type::Unknown;

    ::std::set< ::std::string> features;

    ProgramParams(int argc, char *argv[]);
};

template <typename Rv, typename Fcn>
Rv CompilePhase(const char *name, Fcn f) {
    ::std::cout << name << ": V V V" << ::std::endl;
    g_cur_phase = name;
    g_debug_enabled = debug_enabled_update();
    auto start = clock();
    auto rv = f();
    auto end = clock();
    g_cur_phase = "";
    g_debug_enabled = debug_enabled_update();

    ::std::cout <<"(" << ::std::fixed << ::std::setprecision(2) << static_cast<double>(end - start) / static_cast<double>(CLOCKS_PER_SEC) << " s) ";
    ::std::cout << name << ": DONE";
    ::std::cout << ::std::endl;
    return rv;
}
template <typename Fcn>
void CompilePhaseV(const char *name, Fcn f) {
    CompilePhase<int>(name, [&]() { f(); return 0; });
}

/// main!
int main(int argc, char *argv[])
{
    init_debug_list();
    ProgramParams   params(argc, argv);

    // Set up cfg values
    // TODO: Target spec
    Cfg_SetFlag("unix");
    Cfg_SetFlag("linux");
    Cfg_SetValue("target_os", "linux");
    Cfg_SetValue("target_pointer_width", "64");
    Cfg_SetValue("target_endian", "little");
    Cfg_SetValue("target_arch", "x86_64");
    Cfg_SetValue("target_env", "gnu");
    Cfg_SetValueCb("target_has_atomic", [](const ::std::string& s) {
        if(s == "8")    return true;    // Has an atomic byte
        if(s == "ptr")  return true;    // Has an atomic pointer-sized value
        return false;
        });
    Cfg_SetValueCb("target_feature", [](const ::std::string& s) {
        return false;
        });
    Cfg_SetValueCb("feature", [&params](const ::std::string& s) {
        return params.features.count(s) != 0;
        });



    try
    {
        // Parse the crate into AST
        AST::Crate crate = CompilePhase<AST::Crate>("Parse", [&]() {
            return Parse_Crate(params.infile);
            });

        if( params.last_stage == ProgramParams::STAGE_PARSE ) {
            return 0;
        }

        // Load external crates.
        CompilePhaseV("LoadCrates", [&]() {
            crate.load_externs();
            });

        // Iterate all items in the AST, applying syntax extensions
        CompilePhaseV("Expand", [&]() {
            Expand(crate);
            });

        // Extract the crate type and name from the crate attributes
        auto crate_type = params.crate_type;
        if( crate_type == ::AST::Crate::Type::Unknown ) {
            crate_type = crate.m_crate_type;
        }
        if( crate_type == ::AST::Crate::Type::Unknown ) {
            // Assume to be executable
            crate_type = ::AST::Crate::Type::Executable;
        }
        crate.m_crate_type = crate_type;
        auto crate_name = crate.m_crate_name;
        if( crate_name == "" ) {
            auto s = params.infile.find_last_of('/');
            if( s == ::std::string::npos )
                s = 0;
            else
                s += 1;
            auto e = params.infile.find_first_of('.', s);
            if( e == ::std::string::npos )
                e = params.infile.size() - s;

            crate_name = ::std::string(params.infile.begin() + s, params.infile.begin() + e);
            for(auto& b : crate_name)
            {
                switch(b)
                {
                case '0' ... '9':
                case 'A' ... 'Z':
                case 'a' ... 'z':
                case '_':
                    break;
                case '-':
                    b = '_';
                    break;
                default:
                    break;
                }
            }
            crate.m_crate_name = crate_name;
        }

        if( params.outfile == "" ) {
            switch( crate.m_crate_type )
            {
            case ::AST::Crate::Type::RustLib:
                params.outfile = format(params.output_dir, "lib", crate.m_crate_name, ".hir");
                break;
            case ::AST::Crate::Type::Executable:
                params.outfile = format(params.output_dir, crate.m_crate_name);
                break;
            default:
                params.outfile = format(params.output_dir, crate.m_crate_name, ".o");
                break;
            }
            DEBUG("params.outfile = " << params.outfile);
        }

        // XXX: Dump crate before resolve
        CompilePhaseV("Temp output - Parsed", [&]() {
            Dump_Rust(format(params.outfile, "_0a_exp.rs").c_str(), crate);
            });

        if( params.last_stage == ProgramParams::STAGE_EXPAND ) {
            return 0;
        }

        // Allocator and panic strategies
        if( crate.m_crate_type == ::AST::Crate::Type::Executable )
        {
            // TODO: Detect if an allocator crate is already present.
            crate.load_extern_crate(Span(), "alloc_system");
            crate.load_extern_crate(Span(), "panic_abort");
        }

        // Resolve names to be absolute names (include references to the relevant struct/global/function)
        // - This does name checking on types and free functions.
        // - Resolves all identifiers/paths to references
        CompilePhaseV("Resolve Use", [&]() {
            Resolve_Use(crate); // - Absolutise and resolve use statements
            });
        CompilePhaseV("Resolve Index", [&]() {
            Resolve_Index(crate); // - Build up a per-module index of avalable names (faster and simpler later resolve)
            });
        CompilePhaseV("Resolve Absolute", [&]() {
            Resolve_Absolutise(crate);  // - Convert all paths to Absolute or UFCS, and resolve variables
            });

        // XXX: Dump crate before HIR
        CompilePhaseV("Temp output - Resolved", [&]() {
            Dump_Rust(format(params.outfile, "_1_res.rs").c_str(), crate);
            });

        if( params.last_stage == ProgramParams::STAGE_RESOLVE ) {
            return 0;
        }

        // --------------------------------------
        // HIR Section
        // --------------------------------------
        // Construc the HIR from the AST
        ::HIR::CratePtr hir_crate = CompilePhase< ::HIR::CratePtr>("HIR Lower", [&]() {
            return LowerHIR_FromAST(mv$( crate ));
            });
        // Deallocate the original crate
        crate = ::AST::Crate();

        // Replace type aliases (`type`) into the actual type
        CompilePhaseV("Resolve Type Aliases", [&]() {
            ConvertHIR_ExpandAliases(*hir_crate);
            });
        // Set up bindings and other useful information.
        CompilePhaseV("Resolve Bind", [&]() {
            ConvertHIR_Bind(*hir_crate);
            });
        CompilePhaseV("Resolve UFCS paths", [&]() {
            ConvertHIR_ResolveUFCS(*hir_crate);
            });
        CompilePhaseV("Resolve HIR Markings", [&]() {
            ConvertHIR_Markings(*hir_crate);
            });
        // Basic constant evalulation (intergers/floats only)
        CompilePhaseV("Constant Evaluate", [&]() {
            ConvertHIR_ConstantEvaluate(*hir_crate);
            });

        CompilePhaseV("Dump HIR", [&]() {
            ::std::ofstream os (format(params.outfile, "_2_hir.rs"));
            HIR_Dump( os, *hir_crate );
            });

        // === Type checking ===
        // - This can recurse and call the MIR lower to evaluate constants

        // Check outer items first (types of constants/functions/statics/impls/...)
        // - Doesn't do any expressions except those in types
        CompilePhaseV("Typecheck Outer", [&]() {
            Typecheck_ModuleLevel(*hir_crate);
            });
        // Check the rest of the expressions (including function bodies)
        CompilePhaseV("Typecheck Expressions", [&]() {
            Typecheck_Expressions(*hir_crate);
            });
        // === HIR Expansion ===
        // Annotate how each node's result is used
        CompilePhaseV("Expand HIR Annotate", [&]() {
            HIR_Expand_AnnotateUsage(*hir_crate);
            });
        // - Now that all types are known, closures can be desugared
        CompilePhaseV("Expand HIR Closures", [&]() {
            HIR_Expand_Closures(*hir_crate);
            });
        // - Construct VTables for all traits and impls.
        CompilePhaseV("Expand HIR VTables", [&]() { HIR_Expand_VTables(*hir_crate); });
        // - And calls can be turned into UFCS
        CompilePhaseV("Expand HIR Calls", [&]() {
            HIR_Expand_UfcsEverything(*hir_crate);
            });
        CompilePhaseV("Expand HIR Reborrows", [&]() {
            HIR_Expand_Reborrows(*hir_crate);
            });
        CompilePhaseV("Expand HIR ErasedType", [&]() {
            HIR_Expand_ErasedType(*hir_crate);
            });
        CompilePhaseV("Dump HIR", [&]() {
            ::std::ofstream os (format(params.outfile, "_2_hir.rs"));
            HIR_Dump( os, *hir_crate );
            });
        // - Ensure that typeck worked (including Fn trait call insertion etc)
        CompilePhaseV("Typecheck Expressions (validate)", [&]() {
            Typecheck_Expressions_Validate(*hir_crate);
            });

        if( params.last_stage == ProgramParams::STAGE_TYPECK ) {
            return 0;
        }

        // Lower expressions into MIR
        CompilePhaseV("Lower MIR", [&]() {
            HIR_GenerateMIR(*hir_crate);
            });

        CompilePhaseV("Dump MIR", [&]() {
            ::std::ofstream os (format(params.outfile, "_3_mir.rs"));
            MIR_Dump( os, *hir_crate );
            });

        // Validate the MIR
        CompilePhaseV("MIR Validate", [&]() {
            MIR_CheckCrate(*hir_crate);
            });

        // Second shot of constant evaluation (with full type information)
        CompilePhaseV("Constant Evaluate Full", [&]() {
            ConvertHIR_ConstantEvaluateFull(*hir_crate);
            });
        CompilePhaseV("Dump HIR", [&]() {
            ::std::ofstream os (format(params.outfile, "_2_hir.rs"));
            HIR_Dump( os, *hir_crate );
            });

        // - Expand constants in HIR and virtualise calls
        CompilePhaseV("MIR Cleanup", [&]() {
            MIR_CleanupCrate(*hir_crate);
            });
        // Optimise the MIR
        CompilePhaseV("MIR Optimise", [&]() {
            MIR_OptimiseCrate(*hir_crate);
            });

        CompilePhaseV("Dump MIR", [&]() {
            ::std::ofstream os (format(params.outfile, "_3_mir.rs"));
            MIR_Dump( os, *hir_crate );
            });

        if( params.last_stage == ProgramParams::STAGE_MIR ) {
            return 0;
        }

        // TODO: Pass to mark items that are
        // - Signature Exportable (public)
        // - MIR Exportable (public generic, #[inline], or used by a either of those)
        // - Require codegen (public or used by an exported function)

        // Generate code for non-generic public items (if requested)
        switch( crate_type )
        {
        case ::AST::Crate::Type::Unknown:
            // ERROR?
            break;
        case ::AST::Crate::Type::RustLib:
            // Save a loadable HIR dump
            CompilePhaseV("HIR Serialise", [&]() {
                //HIR_Serialise(params.outfile + ".meta", *hir_crate);
                HIR_Serialise(params.outfile, *hir_crate);
                });
            //TransList   items;
            //CompilePhaseV("Trans Enumerate", [&]() { Trans_Enumerate_Public(*hir_crate); });
            //CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile + ".o", *hir_crate, items); });
            // Generate a .o
            //HIR_Codegen_Lib(params.outfile + ".o", *hir_crate);
            // Link metatdata and object into a .rlib
            break;
        case ::AST::Crate::Type::RustDylib:
            // Save a loadable HIR dump
            CompilePhaseV("HIR Serialise", [&]() { HIR_Serialise(params.outfile, *hir_crate); });
            // - Enumerate codegen for visible and non-generic items
            //TransList   items;
            //CompilePhaseV("Trans Enumerate", [&]() { Trans_Enumerate_Public(*hir_crate); });
            //CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile, *hir_crate, items); });
            // Generate a .so/.dll
            // TODO: Codegen and include the metadata in a non-loadable segment
            break;
        case ::AST::Crate::Type::CDylib:
            // Generate a .so/.dll
            break;
        case ::AST::Crate::Type::Executable:
            // Generate a binary
            // - Enumerate items for translation
            TransList items = CompilePhase<TransList>("Trans Enumerate", [&]() { return Trans_Enumerate_Main(*hir_crate); });
            // - Perform codegen
            CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile, *hir_crate, items); });
            // - Invoke linker?
            break;
        }
    }
    catch(unsigned int) {}
    //catch(const CompileError::Base& e)
    //{
    //    ::std::cerr << "Parser Error: " << e.what() << ::std::endl;
    //    return 2;
    //}
    //catch(const ::std::exception& e)
    //{
    //    ::std::cerr << "Misc Error: " << e.what() << ::std::endl;
    //    return 2;
    //}
    //catch(const char* e)
    //{
    //    ::std::cerr << "Internal Compiler Error: " << e << ::std::endl;
    //    return 2;
    //}
    return 0;
}

ProgramParams::ProgramParams(int argc, char *argv[])
{
    // Hacky command-line parsing
    for( int i = 1; i < argc; i ++ )
    {
        const char* arg = argv[i];

        if( arg[0] != '-' )
        {
            if( this->infile != "" )
                ;
            this->infile = arg;

            if( this->infile == "" )
                ;
        }
        else if( arg[1] != '-' )
        {
            arg ++; // eat '-'
            for( ; *arg; arg ++ )
            {
                switch(*arg)
                {
                // "-o <file>" : Set output file
                case 'o':
                    if( i == argc - 1 ) {
                        // TODO: BAIL!
                        exit(1);
                    }
                    this->outfile = argv[++i];
                    break;
                default:
                    exit(1);
                }
            }
        }
        else
        {
            if( strcmp(arg, "--crate-path") == 0 ) {
                if( i == argc - 1 ) {
                    ::std::cerr << "Flag --crate-path requires an argument" << ::std::endl;
                    exit(1);
                }
                this->crate_path = argv[++i];
            }
            else if( strcmp(arg, "--out-dir") == 0 ) {
                if( i == argc - 1 ) {
                    ::std::cerr << "Flag " << arg << " requires an argument" << ::std::endl;
                    exit(1);
                }
                this->output_dir = argv[++i];
                if( this->output_dir == "" )
                    ;
                if( this->output_dir.back() != '/' )
                    this->output_dir += '/';
            }
            else if( strcmp(arg, "--crate-type") == 0 ) {
                if( i == argc - 1 ) {
                    ::std::cerr << "Flag --crate-type requires an argument" << ::std::endl;
                    exit(1);
                }
                const char* type_str = argv[++i];

                if( strcmp(type_str, "rlib") == 0 ) {
                    this->crate_type = ::AST::Crate::Type::RustLib;
                }
                else {
                    ::std::cerr << "Unknown value for --crate-type" << ::std::endl;
                    exit(1);
                }
            }
            else if( strcmp(arg, "--cfg") == 0 ) {
                if( i == argc - 1 ) {
                    ::std::cerr << "Flag --cfg requires an argument" << ::std::endl;
                    exit(1);
                }
                char* opt_and_val = argv[++i];
                if( char* p = strchr(opt_and_val, '=') ) {
                    *p = '\0';
                    const char* opt = opt_and_val;
                    const char* val = p + 1;
                    if( ::std::strcmp(opt, "feature") == 0 ) {
                        this->features.insert( ::std::string(val) );
                    }
                    else {
                        Cfg_SetValue(opt, val);
                    }
                }
                else {
                    Cfg_SetFlag(opt_and_val);
                }
            }
            else if( strcmp(arg, "--stop-after") == 0 ) {
                if( i == argc - 1 ) {
                    ::std::cerr << "Flag --stop-after requires an argument" << ::std::endl;
                    exit(1);
                }

                arg = argv[++i];
                if( strcmp(arg, "parse") == 0 )
                    this->last_stage = STAGE_PARSE;
                else if( strcmp(arg, "expand") == 0 )
                    this->last_stage = STAGE_EXPAND;
                else if( strcmp(arg, "resolve") == 0 )
                    this->last_stage = STAGE_RESOLVE;
                else if( strcmp(arg, "mir") == 0 )
                    this->last_stage = STAGE_MIR;
                else if( strcmp(arg, "ALL") == 0 )
                    this->last_stage = STAGE_ALL;
                else {
                    ::std::cerr << "Unknown argument to --stop-after : '" << arg << "'" << ::std::endl;
                    exit(1);
                }
            }
            else {
                ::std::cerr << "Unknown option '" << arg << "'" << ::std::endl;
                exit(1);
            }
        }
    }
}

