/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/stringify.cpp
 * - stringify! macro
 */
#include <synext.hpp>
#include "../parse/common.hpp"
#include "../parse/ttstream.hpp"

class CExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;
        ::std::string rv;

        auto lex = TTStream(sp, tt);
        while( GET_TOK(tok, lex) != TOK_EOF )
        {
            if(!rv.empty())
                rv += " ";
            rv += tok.to_str();
        }

        return box$( TTStreamO(sp, TokenTree(Token(TOK_STRING, mv$(rv)))) );
    }
};

STATIC_MACRO("stringify", CExpander);

