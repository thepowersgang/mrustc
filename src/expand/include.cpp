/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/include.cpp
 * - include!/include_str!/include_bytes! support
 */
#include <synext_macro.hpp>
#include <parse/common.hpp>
#include <parse/parseerror.hpp> // for GET_CHECK_TOK
#include <parse/tokentree.hpp>  // TTStream
#include <parse/lex.hpp>
#include <ast/expr.hpp>

class CIncludeExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        if( ident != "" )
            ERROR(sp, E0000, "include! doesn't take an ident");
        
        Token   tok;
        auto lex = TTStream(tt);
        
        GET_CHECK_TOK(tok, lex, TOK_STRING);
        auto path = mv$(tok.str());
        GET_CHECK_TOK(tok, lex, TOK_EOF);
        
        auto base_path = mod.m_file_info.path;
        
        ::std::string file_path;
        if( base_path[base_path.size()-1] == '/' ) {
            file_path = base_path + path;
        }
        else {
            TODO(sp, "Open '" << path << "' relative to '" << base_path << "'");
        }
        
        return box$( Lexer(file_path) );
    }
};

STATIC_MACRO("include", CIncludeExpander);


