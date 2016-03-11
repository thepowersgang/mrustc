/*
 */
#include <synext.hpp>
#include "../parse/common.hpp"

class CExpanderFile:
    public ExpandProcMacro
{
    bool    expand_early() const override { return true; }
    
    ::std::unique_ptr<TokenStream> expand(Span sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(TokenTree(Token(TOK_STRING, sp.filename))) );
    }
};

class CExpanderLine:
    public ExpandProcMacro
{
    bool    expand_early() const override { return true; }
    
    ::std::unique_ptr<TokenStream> expand(Span sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(TokenTree(Token((uint64_t)sp.start_line, CORETYPE_I32))) );
    }
};

STATIC_MACRO("file", CExpanderFile);
STATIC_MACRO("line", CExpanderLine);

