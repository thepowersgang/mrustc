/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * main.cpp
 * - Compiler Entrypoint
 */
#include <iostream>
#include <string>
#include "parse/lex.hpp"
#include "parse/parseerror.hpp"
#include "ast/ast.hpp"
#include "ast/crate.hpp"
#include <serialiser_texttree.hpp>
#include <cstring>
#include <main_bindings.hpp>

#include "expand/cfg.hpp"

int g_debug_indent_level = 0;
::std::string g_cur_phase;

bool debug_enabled()
{
    return g_cur_phase != "Parse";
}
::std::ostream& debug_output(int indent, const char* function)
{
    return ::std::cout << g_cur_phase << "- " << RepeatLitStr { " ", indent } << function << ": ";
}

struct ProgramParams
{
    static const unsigned int EMIT_C = 0x1;
    static const unsigned int EMIT_AST = 0x2;
    enum eLastStage {
        STAGE_PARSE,
        STAGE_EXPAND,
        STAGE_RESOLVE,
        STAGE_TYPECK,
        STAGE_BORROWCK,
        STAGE_ALL,
    } last_stage = STAGE_ALL;

    const char *infile = NULL;
    ::std::string   outfile;
    const char *crate_path = ".";
    unsigned emit_flags = EMIT_C;
    
    ProgramParams(int argc, char *argv[]);
};

template <typename Rv, typename Fcn>
Rv CompilePhase(const char *name, Fcn f) {
    ::std::cout << name << ": V V V" << ::std::endl;
    g_cur_phase = name;
    auto rv = f();
    g_cur_phase = "";
    ::std::cout << name << ": DONE" << ::std::endl;
    return rv;
}
template <typename Fcn>
void CompilePhaseV(const char *name, Fcn f) {
    CompilePhase<int>(name, [&]() { f(); return 0; });
}

/// main!
int main(int argc, char *argv[])
{
    AST_InitProvidedModule();
    
    
    ProgramParams   params(argc, argv);
    
    // Set up cfg values
    Cfg_SetFlag("linux");
    Cfg_SetValue("target_pointer_width", "64");
    
    
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
            
        // Run a quick post-parse pass
        CompilePhaseV("PostParse", [&]() {
            crate.index_impls();
            });

        // XXX: Dump crate before resolve
        CompilePhaseV("Temp output - Parsed", [&]() {
            Dump_Rust( FMT(params.outfile << "_0_pp.rs").c_str(), crate );
            });

        if( params.last_stage == ProgramParams::STAGE_EXPAND ) {
            return 0;
        }
        
        // Resolve names to be absolute names (include references to the relevant struct/global/function)
        // - This does name checking on types and free functions.
        // - Resolves all identifiers/paths to references
        CompilePhaseV("Resolve", [&]() {
            Resolve_Use(crate);
            //Resolve_Absolutise(crate);
            //Resolve_UfcsPaths(crate);
            ResolvePaths(crate);
            });
        
        // XXX: Dump crate before typecheck
        CompilePhaseV("Temp output - Resolved", [&]() {
            Dump_Rust( FMT(params.outfile << "_1_res.rs").c_str(), crate );
            });

        // Replace type aliases (`type`) into the actual type
        CompilePhaseV("Resolve Type Aliases", [&]() {
            //
            });
        
        // Perform type checking on items
        
        // 
        // Typecheck / type propagate module (type annotations of all values)
        // - Check all generic conditions (ensure referenced trait is valid)
        //  > Also mark parameter with applicable traits
        CompilePhaseV("TypecheckBounds", [&]() {
            //Typecheck_GenericBounds(crate);
            });
        
        // - Check all generic parameters match required conditions
        CompilePhaseV("TypecheckParams", [&]() {
            //Typecheck_GenericParams(crate);
            });
        // - Typecheck statics and consts
        // - Typecheck + propagate functions
        //  > Forward pass first
        CompilePhaseV("TypecheckExpr", [&]() {
            //Typecheck_Expr(crate);
            });

        CompilePhaseV("Output", [&]() {
            Dump_Rust( FMT(params.outfile << ".rs").c_str(), crate );
            });
    
        if( params.emit_flags == ProgramParams::EMIT_AST )
        {
            ::std::ofstream os(params.outfile);
            Serialiser_TextTree os_tt(os);
            ((Serialiser&)os_tt) << crate;
            return 0;
        }
        // Flatten modules into "mangled" set
        //g_cur_phase = "Flatten";
        //AST::Flat flat_crate = Convert_Flatten(crate);

        // Convert structures to C structures / tagged enums
        //Convert_Render(flat_crate, stdout);
    }
    catch(const CompileError::Base& e)
    {
        ::std::cerr << "Parser Error: " << e.what() << ::std::endl;
        return 2;
    }
    catch(const ::std::exception& e)
    {
        ::std::cerr << "Misc Error: " << e.what() << ::std::endl;
        return 2;
    }
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
            this->infile = arg;
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
                    // TODO: BAIL!
                    exit(1);
                }
                this->crate_path = argv[++i];
            }
            else if( strcmp(arg, "--emit") == 0 ) {
                if( i == argc - 1 ) {
                    // TODO: BAIL!
                    exit(1);
                }
                
                arg = argv[++i];
                if( strcmp(arg, "ast") == 0 )
                    this->emit_flags = EMIT_AST;
                else if( strcmp(arg, "c") == 0 )
                    this->emit_flags = EMIT_C;
                else {
                    ::std::cerr << "Unknown argument to --emit : '" << arg << "'" << ::std::endl;
                    exit(1);
                }
            }
            else if( strcmp(arg, "--stop-after") == 0 ) {
                if( i == argc - 1 ) {
                    // TODO: BAIL!
                    exit(1);
                }
                
                arg = argv[++i];
                if( strcmp(arg, "parse") == 0 )
                    this->last_stage = STAGE_PARSE;
                else {
                    ::std::cerr << "Unknown argument to --stop-after : '" << arg << "'" << ::std::endl;
                    exit(1);
                }
            }
            else {
                exit(1);
            }
        }
    }
    
    if( this->outfile == "" )
    {
        this->outfile = (::std::string)this->infile + ".o";
    }
}

