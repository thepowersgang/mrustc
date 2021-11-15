/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/stringify.cpp
 * - stringify! macro
 */
#include <synext.hpp>
#include <ast/crate.hpp>
#include "../parse/common.hpp"
#include "../parse/ttstream.hpp"

class CExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;
        ::std::string rv;

        auto lex = TTStream(sp, ParseState(), tt);
        while( GET_TOK(tok, lex) != TOK_EOF )
        {
            if(!rv.empty())
                rv += " ";
            DEBUG(" += " << tok);
            rv += tok.to_str();
        }

        // TODO: Strip out any `{...}` sequences that aren't from nested
        // strings.

        return box$( TTStreamO(sp, ParseState(), TokenTree(Token(TOK_STRING, mv$(rv)))) );
    }
};

STATIC_MACRO("stringify", CExpander);

