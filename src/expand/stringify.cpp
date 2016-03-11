/*
 */
#include <synext.hpp>
#include "../parse/common.hpp"

class CExpander:
    public ExpandProcMacro
{
    bool    expand_early() const override { return true; }
    
    ::std::unique_ptr<TokenStream> expand(Span sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;
        ::std::string rv;
        
        auto lex = TTStream(tt);
        while( GET_TOK(tok, lex) != TOK_EOF )
        {
            rv += tok.to_str();
            rv += " ";
        }
        
        return box$( TTStreamO(TokenTree(Token(TOK_STRING, mv$(rv)))) );
    }
};

STATIC_MACRO("stringify", CExpander);

