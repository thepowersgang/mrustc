
#include <synext.hpp>
#include <parse/tokentree.hpp>
#include <parse/lex.hpp>

class CCfgExpander:
    public ExpandProcMacro
{
    bool    expand_early() const override { return true; }
    
    ::std::unique_ptr<TokenStream> expand(Span sp, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        if( ident != "" ) {
            ERROR(sp, E0000, "cfg! doesn't take an identifier");
        }
        
        DEBUG("cfg!()");
        
        // TODO: Handle cfg!()
        
        return box$( TTStreamO(TokenTree(TOK_RWORD_FALSE)) );
    }
};

STATIC_MACRO("cfg", CCfgExpander);
//STATIC_DECORATOR("cfg", CCfgHandler);
