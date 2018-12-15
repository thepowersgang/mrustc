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
#include <version.hpp>
#include <string_view.hpp>
#include "parse/lex.hpp"
#include "parse/parseerror.hpp"
#include "ast/ast.hpp"
#include "ast/crate.hpp"
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
#include <target_detect.h>	// tools/common/target_detect.h

int g_debug_indent_level = 0;
bool g_debug_enabled = true;
::std::string g_cur_phase;
::std::set< ::std::string>    g_debug_disable_map;

void init_debug_list()
{
    g_debug_disable_map.insert( "Target Load" );
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
    g_debug_disable_map.insert( "Trans Monomorph" );
    g_debug_disable_map.insert( "MIR Optimise Inline" );
    g_debug_disable_map.insert( "Trans Codegen" );

    // Mutate this map using an environment variable
    const char* debug_string = ::std::getenv("MRUSTC_DEBUG");
    if( debug_string )
    {
        while( debug_string[0] )
        {
            const char* end = strchr(debug_string, ':');

            ::std::string   s;
            if( end )
            {
                s = ::std::string { debug_string, end };
                debug_string = end + 1;
                g_debug_disable_map.erase( s );
            }
            else
            {
                s = debug_string;
            }
            if( g_debug_disable_map.erase(s) == 0 )
            {
                ::std::cerr << "WARN: Unknown compiler phase '" << s << "' in $MRUSTC_DEBUG" << ::std::endl;
            }
            if( !end ) {
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

    ::std::string   emit_depfile;

    ::AST::Crate::Type  crate_type = ::AST::Crate::Type::Unknown;
    ::std::string   crate_name;
    ::std::string   crate_name_suffix;

    unsigned opt_level = 0;
    bool emit_debug_info = false;

    bool test_harness = false;

    // NOTE: If populated, nothing happens except for loading the target
    ::std::string   target_saveback;

    ::std::vector<const char*> lib_search_dirs;
    ::std::vector<const char*> libraries;
    ::std::map<::std::string, ::std::string>    crate_overrides;    // --extern name=path

    ::std::set< ::std::string> features;

    struct {
        bool disable_mir_optimisations = false;
        bool full_validate = false;
        bool full_validate_early = false;

        bool dump_ast = false;
        bool dump_hir = false;
        bool dump_mir = false;
    } debug;
    struct {
        ::std::string   codegen_type;
        ::std::string   emit_build_command;
    } codegen;

    ProgramParams(int argc, char *argv[]);

    void show_help() const;
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

    // TODO: Show wall time too?
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
    CompilePhaseV("Target Load", [&]() {
        Target_SetCfg(params.target);
        });

    if( params.target_saveback != "")
    {
        Target_ExportCurSpec(params.target_saveback);
        return 0;
    }

    if( params.infile == "" )
    {
        ::std::cerr << "No input file passed" << ::std::endl;
        return 1;
    }

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

        if( params.debug.dump_ast )
        {
            CompilePhaseV("Dump Expanded", [&]() {
                Dump_Rust( FMT(params.outfile << "_1_ast.rs").c_str(), crate );
                });
        }

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
                ::std::string   alloc_crate_name;
                bool panic_runtime_loaded = false;
                ::std::string   panic_crate_name;
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
                            ERROR(Span(), E0000, "Multiple allocator crates loaded - " << alloc_crate_name << " and " << ec.first);
                        }
                        alloc_crate_name = ec.first;
                        allocator_crate_loaded = true;
                    }
                    if(ec.second.m_hir->m_lang_items.count("mrustc-panic_runtime"))
                    {
                        if( panic_runtime_loaded ) {
                            ERROR(Span(), E0000, "Multiple panic_runtime crates loaded - " << panic_crate_name << " and " << ec.first);
                        }
                        panic_crate_name = ec.first;
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

        if( params.emit_depfile != "" )
        {
            ::std::ofstream of { params.emit_depfile };
            of << params.outfile << ":";
            // - Iterate all loaded files for modules
            struct H {
                ::std::ofstream& of;
                H(::std::ofstream& of): of(of) {}
                void visit_module(::AST::Module& mod) {
                    if( mod.m_file_info.path != "!" && mod.m_file_info.path.back() != '/' ) {
                        of << " " << mod.m_file_info.path;
                    }
                    // TODO: Should we check anon modules?
                    //for(auto& amod : mod.anon_mods()) {
                    //    this->visit_module(*amod);
                    //}
                    for(auto& i : mod.items()) {
                        if(i.data.is_Module()) {
                            this->visit_module(i.data.as_Module());
                        }
                    }
                }
            };
            H(of).visit_module(crate.m_root_module);
            // - Iterate all loaded crates files
            for(const auto& ec : crate.m_extern_crates)
            {
                of << " " << ec.second.m_filename;
            }
            // - Iterate all extra files (include! and friends)
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

        if( params.debug.dump_ast )
        {
            CompilePhaseV("Temp output - Resolved", [&]() {
                Dump_Rust( FMT(params.outfile << "_1_ast.rs").c_str(), crate );
                });
        }

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

        if( params.debug.dump_hir )
        {
            // DUMP after initial consteval
            CompilePhaseV("Dump HIR", [&]() {
                ::std::ofstream os (FMT(params.outfile << "_2_hir.rs"));
                HIR_Dump( os, *hir_crate );
                });
        }

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
        //  TODO: How early can this be done?
        CompilePhaseV("Expand HIR VTables", [&]() {
            HIR_Expand_VTables(*hir_crate);
            });
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
        if( params.debug.dump_hir )
        {
            // DUMP after typecheck (before validation)
            CompilePhaseV("Dump HIR", [&]() {
                ::std::ofstream os (FMT(params.outfile << "_2_hir.rs"));
                HIR_Dump( os, *hir_crate );
                });
        }
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

        if( params.debug.dump_mir )
        {
            // DUMP after generation
            CompilePhaseV("Dump MIR", [&]() {
                ::std::ofstream os (FMT(params.outfile << "_3_mir.rs"));
                MIR_Dump( os, *hir_crate );
                });
        }

        // Validate the MIR
        CompilePhaseV("MIR Validate", [&]() {
            MIR_CheckCrate(*hir_crate);
            });

        if( params.debug.dump_hir )
        {
            // DUMP after consteval (full HIR again)
            CompilePhaseV("Dump HIR", [&]() {
                ::std::ofstream os (FMT(params.outfile << "_2_hir.rs"));
                HIR_Dump( os, *hir_crate );
                });
        }

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

        if( params.debug.dump_mir )
        {
            // DUMP: After optimisation
            CompilePhaseV("Dump MIR", [&]() {
                ::std::ofstream os (FMT(params.outfile << "_3_mir.rs"));
                MIR_Dump( os, *hir_crate );
                });
        }
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

        // TODO: Pass to mark items that are..
        // - Signature Exportable (public)
        // - MIR Exportable (public generic, #[inline], or used by a either of those)
        // - Require codegen (public or used by an exported function)
        TransOptions    trans_opt;
        trans_opt.mode = params.codegen.codegen_type == "" ? "c" : params.codegen.codegen_type;
        trans_opt.build_command_file = params.codegen.emit_build_command;
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

        // Enumerate items to be passed to codegen
        TransList items = CompilePhase<TransList>("Trans Enumerate", [&]() {
            switch( crate_type )
            {
            case ::AST::Crate::Type::Unknown:
                ::std::cerr << "BUG? Unknown crate type" << ::std::endl;
                exit(1);
                break;
            case ::AST::Crate::Type::RustLib:
            case ::AST::Crate::Type::RustDylib:
            case ::AST::Crate::Type::CDylib:
                return Trans_Enumerate_Public(*hir_crate);
            case ::AST::Crate::Type::ProcMacro:
                // TODO: proc macros enumerate twice, once as a library (why?) and again as an executable
                return Trans_Enumerate_Public(*hir_crate);
            case ::AST::Crate::Type::Executable:
                return Trans_Enumerate_Main(*hir_crate);
            }
            throw ::std::runtime_error("Invalid crate_type value");
            });
        // - Generate monomorphised versions of all functions
        CompilePhaseV("Trans Monomorph", [&]() { Trans_Monomorphise_List(*hir_crate, items); });
        // - Do post-monomorph inlining
        CompilePhaseV("MIR Optimise Inline", [&]() { MIR_OptimiseCrate_Inlining(*hir_crate, items); });
        // - Clean up no-unused functions
        //CompilePhaseV("Trans Enumerate Cleanup", [&]() { Trans_Enumerate_Cleanup(*hir_crate, items); });

        switch(crate_type)
        {
        case ::AST::Crate::Type::Unknown:
            throw "";
        case ::AST::Crate::Type::RustLib:
            // Generate a loadable .o
            CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile + ".o", trans_opt, *hir_crate, items, /*is_executable=*/false); });
            // Save a loadable HIR dump
            CompilePhaseV("HIR Serialise", [&]() { HIR_Serialise(params.outfile, *hir_crate); });
            // TODO: Link metatdata and object into a .rlib
            //Trans_Link(params.outfile, params.outfile + ".hir", params.outfile + ".o", CodegenOutput::StaticLibrary);
            break;
        case ::AST::Crate::Type::RustDylib:
            // Generate a .so
            //CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile + ".so", trans_opt, *hir_crate, items, CodegenOutput::DynamicLibrary); });
            CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile + ".o", trans_opt, *hir_crate, items, /*is_executable=*/false); });
            // Save a loadable HIR dump
            CompilePhaseV("HIR Serialise", [&]() { HIR_Serialise(params.outfile, *hir_crate); });
            // TODO: Add the metadata to the .so as a non-loadable segment
            //Trans_Link(params.outfile, params.outfile + ".hir", params.outfile + ".o", CodegenOutput::DynamicLibrary);
            break;
        case ::AST::Crate::Type::CDylib:
            // Generate a .so/.dll
            CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile, trans_opt, *hir_crate, items, /*is_executable=*/false); });
            // - No metadata file
            //Trans_Link(params.outfile, "", params.outfile + ".o", CodegenOutput::DynamicLibrary);
            break;
        case ::AST::Crate::Type::ProcMacro: {
            // Needs: An executable (the actual macro handler), metadata (for `extern crate foo;`)

            // 1. Generate code for the .o file
            // TODO: Is the .o actually needed for proc macros?
            CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile + ".o", trans_opt, *hir_crate, items, /*is_executable=*/false); });

            // 2. Generate code for the plugin itself
            TransList items2 = CompilePhase<TransList>("Trans Enumerate", [&]() { return Trans_Enumerate_Main(*hir_crate); });
            CompilePhaseV("Trans Monomorph", [&]() { Trans_Monomorphise_List(*hir_crate, items2); });
            CompilePhaseV("MIR Optimise Inline", [&]() { MIR_OptimiseCrate_Inlining(*hir_crate, items2); });
            CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile + "-plugin", trans_opt, *hir_crate, items2, /*is_executable=*/true); });

            // - Save a very basic HIR dump, making sure that there's no lang items in it (e.g. `mrustc-main`)
            hir_crate->m_lang_items.clear();
            CompilePhaseV("HIR Serialise", [&]() { HIR_Serialise(params.outfile, *hir_crate); });
            //Trans_Link(params.outfile, params.outfile + ".hir", params.outfile + ".o", CodegenOutput::StaticLibrary);
            //Trans_Link(params.outfile+"-plugin", "", params.outfile + "-plugin.o", CodegenOutput::StaticLibrary);
            break; }
        case ::AST::Crate::Type::Executable:
            CompilePhaseV("Trans Codegen", [&]() { Trans_Codegen(params.outfile, trans_opt, *hir_crate, items, /*is_executable=*/true); });
            //Trans_Link(params.outfile, "", params.outfile + ".o", CodegenOutput::Executable);
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
            case 'C': {
                ::std::string optname;
                ::std::string optval;
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
                auto eq_pos = optname.find('=');
                if( eq_pos != ::std::string::npos ) {
                    optval = optname.substr(eq_pos+1);
                    optname.resize(eq_pos);
                }
                auto get_optval = [&]() {
                    if( eq_pos == ::std::string::npos ) {
                        ::std::cerr << "Flag -Z " << optname << " requires an argument" << ::std::endl;
                        exit(1);
                    }
                    };
                //auto no_optval = [&]() {
                //    if(eq_pos != ::std::string::npos) {
                //        ::std::cerr << "Flag -Z " << optname << " doesn't take an argument" << ::std::endl;
                //        exit(1);
                //    }
                //    };

                if( optname == "emit-build-command" ) {
                    get_optval();
                    this->codegen.emit_build_command = optval;
                }
                else if( optname == "codegen-type" ) {
                    get_optval();
                    this->codegen.codegen_type = optval;
                }
                else if( optname == "emit-depfile" ) {
                    get_optval();
                    this->emit_depfile = optval;
                }
                else {
                    ::std::cerr << "Unknown codegen option: '" << optname << "'" << ::std::endl;
                    exit(1);
                }
                } continue;
            case 'Z': {
                ::std::string optname;
                ::std::string optval;
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
                auto eq_pos = optname.find('=');
                if( eq_pos != ::std::string::npos ) {
                    optval = optname.substr(eq_pos+1);
                    optname.resize(eq_pos);
                }
                auto get_optval = [&]() {
                    if( eq_pos == ::std::string::npos ) {
                        ::std::cerr << "Flag -Z " << optname << " requires an argument" << ::std::endl;
                        exit(1);
                    }
                    };
                auto no_optval = [&]() {
                    if(eq_pos != ::std::string::npos) {
                        ::std::cerr << "Flag -Z " << optname << " doesn't take an argument" << ::std::endl;
                        exit(1);
                    }
                    };

                if( optname == "disable-mir-opt" ) {
                    no_optval();
                    this->debug.disable_mir_optimisations = true;
                }
                else if( optname == "full-validate" ) {
                    no_optval();
                    this->debug.full_validate = true;
                }
                else if( optname == "full-validate-early" ) {
                    no_optval();
                    this->debug.full_validate_early = true;
                }
                else if( optname == "dump-ast" ) {
                    no_optval();
                    this->debug.dump_ast = true;
                }
                else if( optname == "dump-hir" ) {
                    no_optval();
                    this->debug.dump_hir = true;
                }
                else if( optname == "dump-mir" ) {
                    no_optval();
                    this->debug.dump_mir = true;
                }
                else if( optname == "stop-after" ) {
                    get_optval();
                    if( optval == "parse" )
                        this->last_stage = STAGE_PARSE;
                    else if( optval == "expand" )
                        this->last_stage = STAGE_EXPAND;
                    else if( optval == "resolve" )
                        this->last_stage = STAGE_RESOLVE;
                    else if( optval == "typeck" )
                        this->last_stage = STAGE_TYPECK;
                    else if( optval == "mir" )
                        this->last_stage = STAGE_MIR;
                    else {
                        ::std::cerr << "Unknown argument to -Z stop-after - '" << optval << "'" << ::std::endl;
                        exit(1);
                    }
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
                this->show_help();
                exit(0);
            }
            else if( strcmp(arg, "--version" ) == 0 ) {
                ::std::cout << "MRustC " << Version_GetString() << ::std::endl;
                ::std::cout << "- Build time: " << gsVersion_BuildTime << ::std::endl;
                ::std::cout << "- Commit: " << gsVersion_GitHash << (gbVersion_GitDirty ? " (dirty tree)" : "") << ::std::endl;
                exit(0);
            }
            // --out-dir <dir>  >> Set the output directory for automatically-named files
            else if( strcmp(arg, "--out-dir") == 0) {
                if (i == argc - 1) {
                    ::std::cerr << "Flag " << arg << " requires an argument" << ::std::endl;
                    exit(1);
                }
                this->output_dir = argv[++i];
                if( this->output_dir != "" && this->output_dir.back() != '/' )
                {
                    this->output_dir += '/';
                }
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
            else if( strcmp(arg, "--dump-target-spec") == 0 ) {
                if (i == argc - 1) {
                    ::std::cerr << "Flag " << arg << " requires an argument" << ::std::endl;
                    exit(1);
                }
                this->target_saveback = argv[++i];
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

    if( const auto* a = getenv("MRUSTC_DUMP") )
    {
        while( a[0] )
        {
            const char* end = strchr(a, ':');

            ::stdx::string_view  s;
            if( end ) {
                s = ::stdx::string_view { a, end };
                a = end + 1;
            }
            else {
                end = a + strlen(a);
                s = ::stdx::string_view { a, end };
                a = end;
            }

            if( s == "" ) {
                // Ignore
            }
            else if( s == "ast" ) {
                this->debug.dump_ast = true;
            }
            else if( s == "hir" ) {
                this->debug.dump_hir = true;
            }
            else if( s == "mir" ) {
                this->debug.dump_mir = true;
            }
            else {
                ::std::cerr << "Unknown option in $MRUSTC_DUMP '" << s << "'" << ::std::endl;
                // - No terminate, just warn
            }
        }
    }
}
void ProgramParams::show_help() const
{
    ::std::cout <<
        "USAGE: mrustc <sourcefile>\n"
        "\n"
        "OPTIONS:\n"
        "-L <dir>           : Search for crate files (.hir) in this directory\n"
        "-o <filename>      : Write compiler output (library or executable) to this file\n"
        "-O                 : Enable optimistion\n"
        "-g                 : Emit debugging information\n"
        "--out-dir <dir>    : Specify the output directory (alternative to `-o`)\n"
        "--extern <crate>=<path>\n"
        "                   : Specify the path for a given crate (instead of searching for it)\n"
        "--crate-tag <str>  : Specify a suffix for symbols and output files\n"
        "--crate-name <str> : Override/set the crate name\n"
        "--crate-type <ty>  : Override/set the crate type (rlib, bin, proc-macro)\n"
        "--cfg flag         : Set a boolean #[cfg]/cfg! flag\n"
        "--cfg flag=\"val\"   : Set a string #[cfg]/cfg! flag\n"
        "--target <name>    : Compile code for the given target\n"
        "--test             : Generate a unit test executable\n"
        "-C <option>        : Code-generation options\n"
        "-Z <option>        : Debugging/experiemental options\n"
        ;
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
