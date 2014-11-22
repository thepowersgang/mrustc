/*
 */
#include "preproc.hpp"
#include "../ast/ast.hpp"
#include "parseerror.hpp"
#include <cassert>

AST::Path Parse_Path(TokenStream& lex)
{
    AST::Path   path;
    for(;;)
    {
        Token tok = lex.getToken();
        if(tok.type() != TOK_IDENT)
            throw ParseError::Unexpected(tok);
        path.append( tok.str() );
        tok = lex.getToken();
        if(tok.type() != TOK_DOUBLE_COLON) {
            lex.putback(tok);
            break;
        }
    }
    return path;
}

AST::Module Parse_ModRoot(bool is_own_file, Preproc& lex)
{
    AST::Module mod;
    for(;;)
    {
        bool    is_public = false;
        Token tok = lex.getToken();
        switch(tok.type())
        {
        case TOK_BRACE_CLOSE:
            if( is_own_file )
                throw ParseError::Unexpected(tok);
            return mod;
        case TOK_EOF:
            if( !is_own_file )
                throw ParseError::Unexpected(tok);
            return mod;

        case TOK_RWORD_PUB:
            assert(!is_public);
            is_public = false;
            break;

        case TOK_RWORD_USE:
            mod.add_alias( Parse_Path(lex) );
            tok = lex.getToken();
            if( tok.type() != TOK_SEMICOLON )
                throw ParseError::Unexpected(tok, Token(TOK_SEMICOLON));
            break;

        case TOK_RWORD_CONST:
            //mod.add_constant(is_public, name, type, value);
            throw ParseError::Todo("modroot const");
        case TOK_RWORD_STATIC:
            //mod.add_global(is_public, is_mut, name, type, value);
            throw ParseError::Todo("modroot static");
        case TOK_RWORD_FN:
            throw ParseError::Todo("modroot fn");
        case TOK_RWORD_STRUCT:
            throw ParseError::Todo("modroot struct");

        default:
            throw ParseError::Unexpected(tok);
        }
    }
}

void Parse_Crate(::std::string mainfile)
{
    Preproc lex(mainfile);
    AST::Module rootmodule = Parse_ModRoot(true, lex);
}
