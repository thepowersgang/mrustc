
#include <synext.hpp>
#include <parse/tokentree.hpp>
#include <parse/lex.hpp>
#include <parse/common.hpp>
#include "cfg.hpp"

#include <map>
#include <set>

::std::map< ::std::string, ::std::string>   g_cfg_values;
::std::set< ::std::string >   g_cfg_flags;

void Cfg_SetFlag(::std::string name) {
    g_cfg_flags.insert( mv$(name) );
}
void Cfg_SetValue(::std::string name, ::std::string val) {
    g_cfg_values.insert( ::std::make_pair(mv$(name), mv$(val)) );
}

bool check_cfg(Span sp, const ::AST::MetaItem& mi) {
    
    if( mi.has_sub_items() ) {
        // Must be `any`/`not`/`all`
        if( mi.name() == "any" || mi.name() == "cfg" ) {
            for(const auto& si : mi.items()) {
                if( check_cfg(sp, si) )
                    return true;
            }
            return false;
        }
        else if( mi.name() == "not" ) {
            if( mi.items().size() != 1 )
                ERROR(sp, E0000, "cfg(not()) with != 1 argument");
            return !check_cfg(sp, mi.items()[0]);
        }
        else if( mi.name() == "all" ) {
            for(const auto& si : mi.items()) {
                if( ! check_cfg(sp, si) )
                    return false;
            }
            return true;
        }
        else {
            // oops
            ERROR(sp, E0000, "Unknown cfg() function - " << mi.name());
        }
    }
    else if( mi.has_string() ) {
        // Equaliy
        auto it = g_cfg_values.find(mi.name());
        if( it != g_cfg_values.end() )
        {
            DEBUG(""<<mi.name()<<": '"<<it->second<<"' == '"<<mi.string()<<"'");
            return it->second == mi.string();
        }
        else
        {
            WARNING(sp, W0000, "Unknown cfg() param '" << mi.name() << "'");
            return false;
        }
    }
    else {
        // Flag
        auto it = g_cfg_flags.find(mi.name());
        return (it != g_cfg_flags.end());
    }
    BUG(sp, "Fell off the end of check_cfg");
}

class CCfgExpander:
    public ExpandProcMacro
{
    bool    expand_early() const override { return true; }
    
    ::std::unique_ptr<TokenStream> expand(Span sp, const ::AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        if( ident != "" ) {
            ERROR(sp, E0000, "cfg! doesn't take an identifier");
        }
        
        auto lex = TTStreamO(tt);
        auto attrs = Parse_MetaItem(lex);
        DEBUG("cfg!() - " << attrs);
        
        if( check_cfg(sp, attrs) ) {
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
    
    
    void handle(const AST::MetaItem& mi, ::AST::Crate& crate, AST::MacroInvocation& mac) const override {
        DEBUG("#[cfg] mac! - " << mi);
        if( check_cfg(mac.span(), mi) ) {
            // Leave as is
        }
        else {
            mac.clear();
        }
    }
    void handle(const AST::MetaItem& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item&i) const override {
        DEBUG("#[cfg] item - " << mi);
        if( check_cfg(Span(), mi) ) {
            // Leave
        }
        else {
            i = AST::Item::make_None({});
        }
    }
    void handle(const AST::MetaItem& mi, ::AST::Crate& crate, ::std::unique_ptr<AST::ExprNode>& expr) const override {
        DEBUG("#[cfg] expr - " << mi);
        if( check_cfg(Span(expr->get_pos()), mi) ) {
            // Leave
        }
        else {
            expr.reset();
        }
    }
};

STATIC_MACRO("cfg", CCfgExpander);
STATIC_DECORATOR("cfg", CCfgHandler);
