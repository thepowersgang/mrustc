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
#include "trans/target.hpp"

#include "expand/cfg.hpp"

// Hacky default target
#ifdef _MSC_VER
# if defined(_X64)
#  define DEFAULT_TARGET_NAME "x86_64-windows-msvc"
# else
#  define DEFAULT_TARGET_NAME "x86-windows-msvc"
# endif
#elif defined(__linux__)
# if defined(__amd64__)
#  define DEFAULT_TARGET_NAME "x86_64-linux-gnu"
# elif defined(__aarch64__)
#  define DEFAULT_TARGET_NAME "arm64-linux-gnu"
# elif defined(__arm__)
#  define DEFAULT_TARGET_NAME "arm-linux-gnu"
# elif defined(__i386__)
#  define DEFAULT_TARGET_NAME "i586-linux-gnu"
# else
#  error "Unable to detect a suitable default target (linux-gnu)"
# endif
#elif defined(__MINGW32__)
# if defined(_WIN64)
#  define DEFAULT_TARGET_NAME "x86_64-windows-gnu"
# else
#  define DEFAULT_TARGET_NAME "i586-windows-gnu"
# endif
#else
# error "Unable to detect a suitable default target"
#endif

int g_debug_indent_level = 0;
bool g_debug_enabled = true;
::std::string g_cur_phase;
::std::set< ::std::string>    g_debug_disable_map;

void init_debug_list()
{
    g_debug_disable_map.insert( "Parse" );
    g_debug_disable_map.insert( "LoadCrates" );
    g_debug_disable_map.insert( "Expand" );
    g_debug_disable_map.insert( "Dump Expanded" );
    g_debug_disable_map.insert( "Implicit Crates" );

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
    g_debug_disable_map.insert( "MIR Validate Full Early" );
    g_debug_disable_map.insert( "Dump MIR" );
    g_debug_disable_map.insert( "Constant Evaluate Full" );
    g_debug_disable_map.insert( "MIR Cleanup" );
    g_debug_disable_map.insert( "MIR Optimise" );
    g_debug_disable_map.insert( "MIR Validate PO" );
    g_debug_disable_map.insert( "MIR Validate Full" );

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
    ::std::string   target = DEFAULT_TARGET_NAME;

    ::AST::Crate::Type  crate_type = ::AST::Crate::Type::Unknown;
    ::std::string   crate_name;
    ::std::string   crate_name_suffix;

    unsigned opt_level = 0;
    bool emit_debug_info = false;

    bool test_harness = false;

    ::std::vector<const char*> lib_search_dirs;
    ::std::vector<const char*> libraries;
    ::std::map<::std::string, ::std::string>    crate_overrides;    // --extern name=path

    ::std::set< ::std::string> features;


    struct {
        bool disable_mir_optimisations = false;
        bool full_validate = false;
        bool full_validate_early = false;
    } debug;

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
    Cfg_SetValue("rust_compiler", "mrustc");
    Cfg_SetValueCb("feature", [&params](const ::std::string& s) {
        return params.features.count(s) != 0;
        });
    Target_SetCfg(params.target);


    if( params.test_harness )
    {
        Cfg_SetFlag("test");
    }

    try
    {
        // Parse the crate into AST
        AST::Crate crate = CompilePhase<AST::Crate>("Parse", [&]() {
            return Parse_Crate(params.infile);
            });
        crate.m_test_harness = params.test_harness;
        crate.m_crate_name_suffix = params.crate_name_suffix;

        if( params.last_stage == ProgramParams::STAGE_PARSE ) {
            return 0;
        }

        // Load external crates.
        CompilePhaseV("LoadCrates", [&]() {
            // Hacky!
            AST::g_crate_overrides = params.crate_overrides;
            for(const auto& ld : params.lib_search_dirs)
            {
                AST::g_crate_load_dirs.push_back(ld);
            }
            crate.load_externs();
            });

        // Iterate all items in the AST, applying syntax extensions
        CompilePhaseV("Expand", [&]() {
            Expand(crate);
            });

        if( params.test_harness )
        {
            Expand_TestHarness(crate);
        }

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

        if( crate.m_crate_type == ::AST::Crate::Type::ProcMacro )
        {
            Expand_ProcMacro(crate);
        }

        auto crate_name = params.crate_name;
        if( crate_name == "" )
        {
            crate_name = crate.m_crate_name;
        }
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
                if ('0' <= b && b <= '9') {
                }
                else if ('A' <= b && b <= 'Z') {
                }
                else if (b == '_') {
                }
                else if (b == '-') {
                    b = '_';
                }
                else {
                    // TODO: Error?
                }
            }
        }
        crate.m_crate_name = crate_name;
        if( params.test_harness )
        {
            crate.m_crate_name += "$test";
        }

        if( params.outfile == "" ) {
            switch( crate.m_crate_type )
            {
            case ::AST::Crate::Type::RustLib:
                params.outfile = FMT(params.output_dir << "lib" << crate.m_crate_name << ".hir");
                break;
            case ::AST::Crate::Type::Executable:
                params.outfile = FMT(params.output_dir << crate.m_crate_name);
                break;
            default:
                params.outfile = FMT(params.output_dir << crate.m_crate_name << ".o");
                break;
            }
            DEBUG("params.outfile = " << params.outfile);
        }

        // XXX: Dump crate before resolve
        CompilePhaseV("Dump Expanded", [&]() {
            Dump_Rust( FMT(params.outfile << "_0a_exp.rs").c_str(), crate );
            });

        if( params.last_stage == ProgramParams::STAGE_EXPAND ) {
            return 0;
        }

        // Allocator and panic strategies
        CompilePhaseV("Implicit Crates", [&]() {
            if( params.test_harness )
            {
                crate.load_extern_crate(Span(), "test");
            }
            if( crate.m_crate_type == ::AST::Crate::Type::Executable || params.test_harness || crate.m_crate_type == ::AST::Crate::Type::ProcMacro )
            {
                bool allocator_crate_loaded = false;
                bool panic_runtime_loaded = false;
                bool panic_runtime_needed = false;
                for(const auto& ec : crate.m_extern_crates)
                {
                    ::std::ostringstream    ss;
                    for(const auto& e : ec.second.m_hir->m_lang_items)
                        ss << e << ",";
                    DEBUG("Looking at lang items from " << ec.first << " : " << ss.str());
                    if(ec.second.m_hir->m_lang_items.count("mrustc-allocator"))
                    {
                        if( allocator_crate_loaded ) {
                            // TODO: Emit an error because there's multiple allocators loaded
                        }
                        allocator_crate_loaded = true;
                    }
                    if(ec.second.m_hir->m_lang_items.count("mrustc-panic_runtime"))
                    {
                        if( panic_runtime_loaded ) {
                            // TODO: Emit an error because there's multiple allocators loaded
                        }
                        panic_runtime_loaded = true;
                    }
                    if(ec.second.m_hir->m_lang_items.count("mrustc-needs_panic_runtime"))
                    {
                        panic_runtime_needed = true;
                    }
                }
                if( !allocator_crate_loaded )
                {
                    crate.load_extern_crate(Span(), "alloc_system");
                }

                if( panic_runtime_needed && !panic_runtime_loaded )
                {
                    // TODO: Get a panic method from the command line
                    // - Fall back to abort by default, because mrustc doesn't do unwinding yet.
                    crate.load_extern_crate(Span(), "panic_abort");
                }

                // - `mrustc-main` lang item default
                crate.m_lang_items.insert(::std::make_pair( ::std::string("mrustc-main"), ::AST::Path("", {AST::PathNode("main")}) ));
            }
            });

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
            Dump_Rust( FMT(params.outfile << "_1_res.rs").c_str(), crate );
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
        // - Also inserts defaults in trait impls
        CompilePhaseV("Resolve Type Aliases", [&]() {
            ConvertHIR_ExpandAliases(*hir_crate);
            });
        // Set up bindings and other useful information.
        CompilePhaseV("Resolve Bind", [&]() {
            ConvertHIR_Bind(*hir_crate);
            });
        // Enumerate marker impls on types and other useful metadata
        CompilePhaseV("Resolve HIR Markings", [&]() {
            ConvertHIR_Markings(*hir_crate);
            });
        // Determine what trait to use for <T>::Foo (and does some associated type expansion)
        CompilePhaseV("Resolve UFCS paths", [&]() {
            ConvertHIR_ResolveUFCS(*hir_crate);
            });
        // Basic constant evalulation (intergers/floats only)
        CompilePhaseV("Constant Evaluate", [&]() {
            ConvertHIR_ConstantEvaluate(*hir_crate);
            });

        CompilePhaseV("Dump HIR", [&]() {
            ::std::ofstream os (FMT(params.outfile << "_2_hir.rs"));
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
            ::std::ofstream os (FMT(params.outfile << "_2_hir.rs"));
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
            ::std::ofstream os (FMT(params.outfile << "_3_mir.rs"));
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
            ::std::ofstream os (FMT(params.outfile << "_2_hir.rs"));
            HIR_Dump( os, *hir_crate );
            });

        // - Expand constants in HIR and virtualise calls
        CompilePhaseV("MIR Cleanup", [&]() {
            MIR_CleanupCrate(*hir_crate);
            });
        if( params.debug.full_validate_early || getenv("MRUSTC_FULL_VALIDATE_PREOPT") )
        {
            CompilePhaseV("MIR Validate Full Early", [&]() {
                MIR_CheckCrate_Full(*hir_crate);
                });
        }

        // Optimise the MIR
        CompilePhaseV("MIR Optimise", [&]() {
            MIR_OptimiseCrate(*hir_crate, params.debug.disable_mir_optimisations);
            });

        CompilePhaseV("Dump MIR", [&]() {
            ::std::ofstream os (FMT(params.outfile << "_3_mir.rs"));
            MIR_Dump( os, *hir_crate );
            });
        CompilePhaseV("MIR Validate PO", [&]() {
            MIR_CheckCrate(*hir_crate);
            });
        // - Exhaustive MIR validation (follows every code path and checks variable validity)
        // > DEBUGGING ONLY
        CompilePhaseV("MIR Validate Full", [&]() {
            if( params.debug.full_validate || getenv("MRUSTC_FULL_VALIDATE") )
                MIR_CheckCrate_Full(*hir_crate);
            });

        if( params.last_stage == ProgramParams::STAGE_MIR ) {
            return 0;
        }

        // TODO: Pass to mark items that are
        // - Signature Exportable (public)
        // - MIR Exportable (public generic, #[inline], or used by a either of those)
        // - Require codegen (public or used by an exported function)
        TransOptions    trans_opt;
        trans_opt.opt_level = params.opt_level;
        for(const char* libdir : params.lib_search_dirs ) {
            // Store these paths for use in final linking.
            hir_crate->m_link_paths.push_back( libdir );
        }
        for(const char* libname : params.libraries ) {
            hir_crate->m_ext_libs.push_back(::HIR::ExternLibrary { libname });
        }
        trans_opt.emit_debug_info = params.emit_debug_info;

        // Generate code for non-generic public items (if requested)
        if( params.test_harness )
        {
            // If the test harness is enabled, override crate type to "Executable"
            crate_type = ::AST::Crate::Type::Executable;
        }
        switch( crate_type )
        {
        case ::AST::Crate::Type::Unknown:
            // ERROR?
            break;
        case ::AST::Crate::Type::RustLib: {
            #if 1
            // Generate a .o
            TransList   items = CompilePhase<TransList>("Trans Enumerate", [&]() { return Trans_Enumerate_Public(*hir_crate); });
            CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile + ".o", trans_opt, *hir_crate, items, false); });
            #endif

            // Save a loadable HIR dump
            CompilePhaseV("HIR Serialise", [&]() {
                //HIR_Serialise(params.outfile + ".meta", *hir_crate);
                HIR_Serialise(params.outfile, *hir_crate);
                });

            // Link metatdata and object into a .rlib
            break; }
        case ::AST::Crate::Type::RustDylib: {
            #if 1
            // Generate a .o
            TransList   items = CompilePhase<TransList>("Trans Enumerate", [&]() { return Trans_Enumerate_Public(*hir_crate); });
            CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile + ".o", trans_opt, *hir_crate, items, false); });
            #endif
            // Save a loadable HIR dump
            CompilePhaseV("HIR Serialise", [&]() { HIR_Serialise(params.outfile, *hir_crate); });

            // Generate a .so/.dll
            // TODO: Codegen and include the metadata in a non-loadable segment
            break; }
        case ::AST::Crate::Type::CDylib:
            // Generate a .so/.dll
            break;
        case ::AST::Crate::Type::ProcMacro: {
            // Needs: An executable (the actual macro handler), metadata (for `extern crate foo;`)
            // Can just emit the metadata and do miri?
            // - Requires MIR for EVERYTHING, not feasable.
            TransList items = CompilePhase<TransList>("Trans Enumerate", [&]() { return Trans_Enumerate_Public(*hir_crate); });
            CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile + ".o", trans_opt, *hir_crate, items, false); });

            TransList items2 = CompilePhase<TransList>("Trans Enumerate", [&]() { return Trans_Enumerate_Main(*hir_crate); });
            CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile + "-plugin", trans_opt, *hir_crate, items2, true); });

            hir_crate->m_lang_items.clear();    // Make sure that we're not exporting any lang items
            CompilePhaseV("HIR Serialise", [&]() { HIR_Serialise(params.outfile, *hir_crate); });
            break; }
        case ::AST::Crate::Type::Executable:
            // Generate a binary
            // - Enumerate items for translation
            TransList items = CompilePhase<TransList>("Trans Enumerate", [&]() { return Trans_Enumerate_Main(*hir_crate); });
            // - Perform codegen
            CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile, trans_opt, *hir_crate, items, true); });
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
            if (this->infile == "")
            {
                this->infile = arg;
            }
            else
            {
                // TODO: Error
                ::std::cerr << "Unexpected free argument" << ::std::endl;
                exit(1);
            }
        }
        else if( arg[1] != '-' )
        {
            arg ++; // eat '-'

            switch( *arg )
            {
            case 'L':
                if( arg[1] == '\0' ) {
                    if( i == argc - 1 ) {
                        ::std::cerr << "Option " << arg << " requires an argument" << ::std::endl;
                        exit(1);
                    }
                    this->lib_search_dirs.push_back( argv[++i] );
                }
                else {
                    this->lib_search_dirs.push_back( arg+1 );
                }
                continue ;
            case 'l':
                if( arg[1] == '\0' ) {
                    if( i == argc - 1 ) {
                        ::std::cerr << "Option " << arg << " requires an argument" << ::std::endl;
                        exit(1);
                    }
                    this->libraries.push_back( argv[++i] );
                }
                else {
                    this->libraries.push_back( arg+1 );
                }
                continue ;
            case 'Z': {
                ::std::string optname;
                if( arg[1] == '\0' ) {
                    if( i == argc - 1) {
                        ::std::cerr << "Option " << arg << " requires an argument" << ::std::endl;
                        exit(1);
                    }
                    optname = argv[++i];
                }
                else {
                    optname = arg+1;
                }

                if( optname == "disable-mir-opt" ) {
                    this->debug.disable_mir_optimisations = true;
                }
                else if( optname == "full-validate" ) {
                    this->debug.full_validate = true;
                }
                else if( optname == "full-validate-early" ) {
                    this->debug.full_validate_early = true;
                }
                else {
                    ::std::cerr << "Unknown debug option: '" << optname << "'" << ::std::endl;
                    exit(1);
                }
                } continue;

            default:
                // Fall through to the for loop below
                break;
            }

            for( ; *arg; arg ++ )
            {
                switch(*arg)
                {
                // "-o <file>" : Set output file
                case 'o':
                    if( i == argc - 1 ) {
                        ::std::cerr << "Option -" << *arg << " requires an argument" << ::std::endl;
                        exit(1);
                    }
                    this->outfile = argv[++i];
                    break;
                case 'O':
                    this->opt_level = 2;
                    break;
                case 'g':
                    this->emit_debug_info = true;
                    break;
                default:
                    ::std::cerr << "Unknown option: '-" << *arg << "'" << ::std::endl;
                    exit(1);
                }
            }
        }
        else
        {
            if( strcmp(arg, "--help") == 0 ) {
                // TODO: Help
            }
            // --out-dir <dir>  >> Set the output directory for automatically-named files
            else if (strcmp(arg, "--out-dir") == 0) {
                if (i == argc - 1) {
                    ::std::cerr << "Flag " << arg << " requires an argument" << ::std::endl;
                    exit(1);
                }
                this->output_dir = argv[++i];
                if (this->output_dir == "") {
                    // TODO: Error?
                }
                if( this->output_dir.back() != '/' )
                    this->output_dir += '/';
            }
            // --extern <name>=<path>   >> Override the file to load for `extern crate <name>;`
            else if( strcmp(arg, "--extern") == 0 ) {
                if( i == argc - 1 ) {
                    ::std::cerr << "Option " << arg << " requires an argument" << ::std::endl;
                    exit(1);
                }
                const char* desc = argv[++i];
                const char* pos = ::std::strchr(desc, '=');
                if( pos == nullptr ) {
                    ::std::cerr << "--extern takes an argument of the format name=path" << ::std::endl;
                    exit(1);
                }

                auto name = ::std::string(desc, pos);
                auto path = ::std::string(pos+1);
                this->crate_overrides.insert(::std::make_pair( mv$(name), mv$(path) ));
            }
            // --crate-tag <name>  >> Specify a version/identifier suffix for the crate
            else if( strcmp(arg, "--crate-tag") == 0 ) {
                if( i == argc - 1 ) {
                    ::std::cerr << "Flag " << arg << " requires an argument" << ::std::endl;
                    exit(1);
                }
                const char* name_str = argv[++i];
                this->crate_name_suffix = name_str;
            }
            // --crate-name <name>  >> Specify the crate name (overrides `#![crate_name="<name>"]`)
            else if( strcmp(arg, "--crate-name") == 0 ) {
                if( i == argc - 1 ) {
                    ::std::cerr << "Flag --crate-name requires an argument" << ::std::endl;
                    exit(1);
                }
                const char* name_str = argv[++i];
                this->crate_name = name_str;
            }
            // `--crate-type <name>`    - Specify the crate type (overrides `#![crate_type="<name>"]`)
            else if( strcmp(arg, "--crate-type") == 0 ) {
                if( i == argc - 1 ) {
                    ::std::cerr << "Flag --crate-type requires an argument" << ::std::endl;
                    exit(1);
                }
                const char* type_str = argv[++i];

                if( strcmp(type_str, "rlib") == 0 ) {
                    this->crate_type = ::AST::Crate::Type::RustLib;
                }
                else if( strcmp(type_str, "bin") == 0 ) {
                    this->crate_type = ::AST::Crate::Type::Executable;
                }
                else if( strcmp(type_str, "proc-macro") == 0 ) {
                    this->crate_type = ::AST::Crate::Type::ProcMacro;
                }
                else {
                    ::std::cerr << "Unknown value for --crate-type" << ::std::endl;
                    exit(1);
                }
            }
            // `--cfg <flag>`
            // `--cfg <var>=<value>`
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
                    // TODO: Correctly parse the values.
                    // - Value should be a double-quoted string.
                    if( ::std::strcmp(opt, "feature") == 0 ) {
                        this->features.insert( ::std::string(val) );
                    }
                    else {
                        if( val[0] == '"' ) {
                            // TODO: Something cleaner than this.
                            ::std::string   s = val+1;
                            assert(s.back() == '"');
                            s.pop_back();
                            Cfg_SetValue(opt, s);
                        }
                        else {
                            Cfg_SetValue(opt, val);
                        }
                    }
                }
                else {
                    Cfg_SetFlag(opt_and_val);
                }
            }
            // `--target <triple>`  - Override the default compiler target
            else if( strcmp(arg, "--target") == 0 ) {
                if (i == argc - 1) {
                    ::std::cerr << "Flag " << arg << " requires an argument" << ::std::endl;
                    exit(1);
                }
                this->target = argv[++i];
            }
            // `--stop-after <stage>`   - Stops the compiler after the specified stage
            // TODO: Convert this to a `-Z` option
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
            else if( strcmp(arg, "--test") == 0 ) {
                this->test_harness = true;
            }
            else {
                ::std::cerr << "Unknown option '" << arg << "'" << ::std::endl;
                exit(1);
            }
        }
    }

    if (this->infile == "")
    {
        ::std::cerr << "No input file passed" << ::std::endl;
        exit(1);
    }
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
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)v << 6;
                os << "\\u{" << ::std::hex << val << "}";
            }
            else if( v < 0xF0 )
            {
                uint32_t    val = (uint32_t)(v & 0x0F) << 12;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)v << 12;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)v << 6;
                os << "\\u{" << ::std::hex << val << "}";
            }
            else if( v < 0xF8 )
            {
                uint32_t    val = (uint32_t)(v & 0x07) << 18;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)v << 18;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)v << 12;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)v << 6;
                os << "\\u{" << ::std::hex << val << "}";
            }
            break;
        }
    }
    os << ::std::dec;
    return os;
}
