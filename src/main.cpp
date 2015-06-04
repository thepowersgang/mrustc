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
#include <serialiser_texttree.hpp>
#include <cstring>
#include <main_bindings.hpp>


int g_debug_indent_level = 0;
::std::string g_cur_phase;

bool debug_enabled()
{
        
        return true;
}
::std::ostream& debug_output(int indent, const char* function)
{
        return ::std::cout << g_cur_phase << "- " << RepeatLitStr { " ", indent } << function << ": ";
}

struct ProgramParams
{
	static const unsigned int EMIT_C = 0x1;
	static const unsigned int EMIT_AST = 0x2;

	const char *infile = NULL;
	::std::string	outfile;
	const char *crate_path = ".";
	unsigned emit_flags = EMIT_C;
    
    ProgramParams(int argc, char *argv[]);
};

/// main!
int main(int argc, char *argv[])
{
    AST_InitProvidedModule();
    
	
    ProgramParams   params(argc, argv);
    
    
    try
    {
        g_cur_phase = "Parse";
        AST::Crate crate = Parse_Crate(params.infile);
    
        // Iterate all items in the AST, applying syntax extensions
        g_cur_phase = "Syn Exts";
        Process_Decorators(crate);
        // TODO:
        
        g_cur_phase = "PostParse";
        crate.post_parse();

        //s << crate;
        g_cur_phase = "Temp output";
        Dump_Rust( FMT(params.outfile << ".rs").c_str(), crate );
    
        // Resolve names to be absolute names (include references to the relevant struct/global/function)
        g_cur_phase = "Resolve";
        ResolvePaths(crate);
        
        g_cur_phase = "Temp output"; Dump_Rust( FMT(params.outfile << ".rs").c_str(), crate );

        // Typecheck / type propagate module (type annotations of all values)
        // - Check all generic conditions (ensure referenced trait is valid)
        //  > Also mark parameter with applicable traits
        #if 0
        g_cur_phase = "TypecheckBounds";
        Typecheck_GenericBounds(crate);
        // - Check all generic parameters match required conditions
        g_cur_phase = "TypecheckParams";
        Typecheck_GenericParams(crate);
        // - Typecheck statics and consts
        // - Typecheck + propagate functions
        //  > Forward pass first
        //g_cur_phase = "TypecheckExpr";
        //Typecheck_Expr(crate);
        #endif

        g_cur_phase = "Output";
        Dump_Rust( FMT(params.outfile << ".rs").c_str(), crate );
    
        if( params.emit_flags == ProgramParams::EMIT_AST )
        {
            ::std::ofstream os(params.outfile);
            Serialiser_TextTree os_tt(os);
            ((Serialiser&)os_tt) << crate;
            return 0;
        }
        // Flatten modules into "mangled" set
        g_cur_phase = "Flatten";
        AST::Flat flat_crate = Convert_Flatten(crate);

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

