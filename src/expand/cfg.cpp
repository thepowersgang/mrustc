/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/cfg.cpp
 * - cfg! and #[cfg] handling
 */
#include <synext.hpp>
#include <parse/common.hpp>
#include <parse/tokentree.hpp>
#include <parse/ttstream.hpp>
#include "cfg.hpp"
#include <ast/expr.hpp> // Needed to clear a ExprNodeP
#include <ast/crate.hpp>

#include <map>
#include <set>

::std::multimap< ::std::string, ::std::string>   g_cfg_values;
::std::map< ::std::string, ::std::function<bool(const ::std::string&)> >   g_cfg_value_fcns;
::std::set< ::std::string >   g_cfg_flags;

void Cfg_Dump(::std::ostream& os) {
    for(const auto& v : g_cfg_values) {
        os << ">" << v.first << "=" << v.second << std::endl;
    }
    for(const auto& f : g_cfg_flags) {
        os << ">" << f << std::endl;
    }
    // NOTE: `g_cfg_value_fcns` is only used for feature flags, which minicargo doesn't need
}
void Cfg_SetFlag(::std::string name) {
    g_cfg_flags.insert( mv$(name) );
}
void Cfg_SetValue(::std::string name, ::std::string val) {
    g_cfg_values.insert( ::std::make_pair(mv$(name), mv$(val)) );
}
void Cfg_SetValueCb(::std::string name, ::std::function<bool(const ::std::string&)> cb) {
    g_cfg_value_fcns.insert( ::std::make_pair(mv$(name), mv$(cb)) );
}

bool check_cfg(const Span& sp, const ::AST::Attribute& mi)
{
    if( !mi.name().is_trivial() )   ERROR(sp, E0000, "Non-trivial attribute name in cfg - " << mi.name());
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
        auto its = g_cfg_values.equal_range(mi.name().as_trivial().c_str());
        for(auto it = its.first; it != its.second; ++it)
        {
            DEBUG(""<<mi.name()<<": '"<<it->second<<"' == '"<<mi.string()<<"'");
            if( it->second == mi.string() )
                return true;
        }
        if( its.first != its.second )
            return false;

        auto it2 = g_cfg_value_fcns.find(mi.name().as_trivial().c_str());
        if(it2 != g_cfg_value_fcns.end() )
        {
            DEBUG(""<<mi.name()<<": ('"<<mi.string()<<"')?");
            return it2->second( mi.string() );
        }

        WARNING(sp, W0000, "Unknown cfg() param '" << mi.name() << "'");
        return false;
    }
    else {
        // Flag
        auto it = g_cfg_flags.find(mi.name().as_trivial().c_str());
        return (it != g_cfg_flags.end());
    }
    BUG(sp, "Fell off the end of check_cfg");
}

class CCfgExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        auto lex = TTStream(sp, ParseState(), tt);
        lex.parse_state().crate = &crate;
        lex.parse_state().module = &mod;
        auto attrs = Parse_MetaItem(lex);
        DEBUG("cfg!() - " << attrs);

        if( check_cfg(sp, attrs) ) {
            return box$( TTStreamO(sp, ParseState(), TokenTree(AST::Edition::Rust2015,{},TOK_RWORD_TRUE )) );
        }
        else {
            return box$( TTStreamO(sp, ParseState(), TokenTree(AST::Edition::Rust2015,{},TOK_RWORD_FALSE)) );
        }
    }
};


class CCfgHandler:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }


    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate) const override {
        DEBUG("#[cfg] crate - " << mi);
        // Ignore, as #[cfg] on a crate is handled in expand/mod.cpp
        if( check_cfg(sp, mi) ) {
        }
        else {
            // Remove all items (can't remove the module)
            crate.m_root_module.m_items.clear();
        }
    }
    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        TRACE_FUNCTION_FR("#[cfg] item - " << mi, (i.is_None() ? "Deleted" : ""));
        if( check_cfg(sp, mi) ) {
            // Leave
        }
        else {
            i = AST::Item::make_None({});
        }
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, AST::Impl& impl, const RcString& name, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        TRACE_FUNCTION_FR("#[cfg] item - " << mi, (i.is_None() ? "Deleted" : ""));
        if( check_cfg(sp, mi) ) {
            // Leave
        }
        else {
            i = AST::Item::make_None({});
        }
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::AbsolutePath& path, AST::Trait& trait, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        TRACE_FUNCTION_FR("#[cfg] item - " << mi, (i.is_None() ? "Deleted" : ""));
        if( check_cfg(sp, mi) ) {
            // Leave
        }
        else {
            i = AST::Item::make_None({});
        }
    }
    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, ::AST::ExprNodeP& expr) const override {
        DEBUG("#[cfg] expr - " << mi);
        if( check_cfg(sp, mi) ) {
            // Leave
        }
        else {
            expr.reset();
        }
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::Module& mod, AST::ImplDef& impl) const override {
        DEBUG("#[cfg] impl - " << mi);
        if( check_cfg(sp, mi) ) {
            // Leave
        }
        else {
            impl.type() = ::TypeRef(sp);
        }
    }

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::StructItem& si) const override {
        DEBUG("#[cfg] struct item - " << mi);
        if( !check_cfg(sp, mi) ) {
            si.m_name = "";
        }
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::TupleItem& i) const override {
        DEBUG("#[cfg] tuple item - " << mi);
        if( !check_cfg(sp, mi) ) {
            i.m_type = ::TypeRef(sp);
        }
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::EnumVariant& i) const override {
        DEBUG("#[cfg] enum variant - " << mi);
        if( !check_cfg(sp, mi) ) {
            i.m_name = "";
        }
    }

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::ExprNode_Match_Arm& i) const override {
        DEBUG("#[cfg] match arm - " << mi);
        if( !check_cfg(sp, mi) ) {
            i.m_patterns.clear();
        }
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::ExprNode_StructLiteral::Ent& i) const override {
        DEBUG("#[cfg] struct lit - " << mi);
        if( !check_cfg(sp, mi) ) {
            i.value.reset();
        }
    }
};

STATIC_MACRO("cfg", CCfgExpander);
STATIC_DECORATOR("cfg", CCfgHandler);
