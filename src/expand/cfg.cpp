
#include <synext.hpp>
#include <parse/tokentree.hpp>
#include <parse/lex.hpp>
#include <parse/common.hpp>

bool check_cfg(const ::AST::MetaItem& mi) {
    // TODO: Handle cfg conditions
    throw ::std::runtime_error("TODO: Handle #[cfg] or cfg! conditions");
    return true;
}

class CCfgExpander:
    public ExpandProcMacro
{
    bool    expand_early() const override { return true; }
    
    ::std::unique_ptr<TokenStream> expand(Span sp, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        if( ident != "" ) {
            ERROR(sp, E0000, "cfg! doesn't take an identifier");
        }
        
        auto lex = TTStreamO(tt);
        auto attrs = Parse_MetaItem(lex);
        DEBUG("cfg!() - " << attrs);
        
        if( check_cfg(attrs) ) {
            return box$( TTStreamO(TokenTree(TOK_RWORD_TRUE )) );
        }
        else {
            return box$( TTStreamO(TokenTree(TOK_RWORD_FALSE)) );
        }
    }
};


class CCfgHandler:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::EarlyPre; }
    
    
    void handle(const AST::MetaItem& mi, AST::Crate& crate, AST::MacroInvocation& mac) const override {
        if( check_cfg(mi) ) {
            // Leave as is
        }
        else {
            mac.clear();
        }
    }
    void handle(const AST::MetaItem& mi, AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item&i) const override {
        if( check_cfg(mi) ) {
            // Leave
        }
        else {
            i = AST::Item::make_None({});
        }
    }
    void handle(const AST::MetaItem& mi, ::std::unique_ptr<AST::ExprNode>& expr) const override {
        if( check_cfg(mi) ) {
            // Leave
        }
        else {
            expr.reset();
        }
    }
};

STATIC_MACRO("cfg", CCfgExpander);
STATIC_DECORATOR("cfg", CCfgHandler);
