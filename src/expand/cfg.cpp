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
#include <ast/attrs.hpp>
#include <parse/parseerror.hpp>

#include <map>
#include <set>

::std::multimap< ::std::string, ::std::string>   g_cfg_values;
::std::map< ::std::string, ::std::function<bool(const ::std::string&)> >   g_cfg_value_fcns;
::std::set< ::std::string >   g_cfg_flags;

static const RcString rcstring_cfg = RcString::new_interned("cfg");

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

namespace {
    bool check_cfg_inner1(const RcString& name, TokenStream& lex);
    bool check_cfg_inner(TokenStream& lex)
    {
        TRACE_FUNCTION;
        if( lex.lookahead(0) == TOK_INTERPOLATED_META )
        {
            auto meta = std::move(lex.getTokenCheck(TOK_INTERPOLATED_META).frag_meta());
            auto ilex = TTStream(meta.span(), ParseState(), meta.data());
            return check_cfg_inner1(meta.name().as_trivial(), ilex);
        }
        else
        {
            auto name = lex.getTokenCheck(TOK_IDENT).ident().name;
            return check_cfg_inner1(name, lex);
        }
    }
    bool check_cfg_inner1(const RcString& name, TokenStream& lex)
    {
        Token   tok;
        switch(lex.lookahead(0))
        {
        case TOK_EQUAL: {
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);
            std::string val;
            if(lex.lookahead(0) == TOK_INTERPOLATED_EXPR) {
                auto n = lex.getTokenCheck(TOK_INTERPOLATED_EXPR).take_frag_node();
                const auto* np = dynamic_cast<AST::ExprNode_String*>(n.get());
                ASSERT_BUG(n->span(), np, "");
                val = np->m_value;
            }
            else {
                GET_CHECK_TOK(tok, lex, TOK_STRING);
                val = tok.str();
            }
            // Equality
            auto its = g_cfg_values.equal_range(name.c_str());
            for(auto it = its.first; it != its.second; ++it)
            {
                DEBUG(name << ": '" << it->second << "' == '" << val << "'");
                if( it->second == val )
                    return true;
            }
            if( its.first != its.second )
                return false;

            auto it2 = g_cfg_value_fcns.find(name.c_str());
            if(it2 != g_cfg_value_fcns.end() )
            {
                DEBUG(name << ": ('" << val << "')?");
                return it2->second( val );
            }

            //WARNING(lex.point_span(), W0000, "Unknown cfg() param '" << name << "'");
            return false;
            }
        case TOK_PAREN_OPEN:
            GET_TOK(tok, lex);

            static const RcString rcstring_any = RcString::new_interned("any");
            static const RcString rcstring_not = RcString::new_interned("not");
            static const RcString rcstring_all = RcString::new_interned("all");
            if( name == rcstring_any || name == rcstring_cfg ) {
                bool rv = false;
                while(lex.lookahead(0) != TOK_PAREN_CLOSE) {
                    rv |= check_cfg_inner(lex);
                    if(lex.lookahead(0) != TOK_COMMA)
                        break;
                    GET_CHECK_TOK(tok, lex, TOK_COMMA);
                }
                GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
                return rv;
            }
            else if( name == rcstring_not ) {
                bool rv = check_cfg_inner(lex);
                // Allow a trailing comma
                lex.getTokenIf(TOK_COMMA);
                GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
                return !rv;
            }
            else if( name == rcstring_all ) {
                bool rv = true;
                while(lex.lookahead(0) != TOK_PAREN_CLOSE) {
                    rv &= check_cfg_inner(lex);
                    if(lex.lookahead(0) != TOK_COMMA)
                        break;
                    GET_CHECK_TOK(tok, lex, TOK_COMMA);
                }
                GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
                return rv;
            }
            else {
                // oops
                ERROR(lex.point_span(), E0000, "Unknown cfg() function - " << name);
            }

            break;
        default:
            auto its = g_cfg_values.equal_range(name.c_str());
            for(auto it = its.first; it != its.second; ++it) {
                return true;
            }
            // Flag
            auto it = g_cfg_flags.find(name.c_str());
            return (it != g_cfg_flags.end());
        }
    }
}
bool check_cfg_stream(TokenStream& lex)
{
    Token   tok;
    bool rv = false;
    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
    while(lex.lookahead(0) != TOK_PAREN_CLOSE) {
        rv |= check_cfg_inner(lex);
        if(lex.lookahead(0) != TOK_COMMA)
            break;
        GET_CHECK_TOK(tok, lex, TOK_COMMA);
    }
    GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
    return rv;
}
bool check_cfg(const Span& sp, const ::AST::Attribute& mi)
{
    TTStream    lex(sp, ParseState(), mi.data());
    return check_cfg_stream(lex);
}
bool check_cfg_attrs(const ::AST::AttributeList& attrs)
{
    for( auto& a : attrs.m_items )
    {
        if( a.name() == rcstring_cfg ) {
            if( !check_cfg(a.span(), a) ) {
                return false;
            }
        }
    }
    return true;
}
std::vector<AST::Attribute> check_cfg_attr(const ::AST::Attribute& mi)
{
    TTStream    lex(mi.span(), ParseState(), mi.data());

    Token   tok;
    std::vector<AST::Attribute> rv;
    lex.getTokenCheck(TOK_PAREN_OPEN);
    auto cfg_res = check_cfg_inner(lex);
    while( lex.lookahead(0) == TOK_COMMA )
    {
        lex.getTokenCheck(TOK_COMMA);
        rv.push_back( Parse_MetaItem(lex) );
    }
    lex.getTokenCheck(TOK_PAREN_CLOSE);
    lex.getTokenCheck(TOK_EOF);
    if(cfg_res) {
        return rv;
    }
    else {
        return std::vector<AST::Attribute>();
    }
}

class CCfgExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        DEBUG("cfg!() - " << tt);
        auto lex = TTStream(sp, ParseState(), tt);
        bool rv = check_cfg_inner(lex);
        lex.getTokenCheck(TOK_EOF);

        return box$( TTStreamO(sp, ParseState(), TokenTree(AST::Edition::Rust2015,{}, rv ? TOK_RWORD_TRUE : TOK_RWORD_FALSE )) );
    }
};

class CCfgSelectExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        DEBUG("cfg_select!() - " << tt);
        auto lex = TTStream(sp, ParseState(), tt);
        for(;;)
        {
            bool rv = lex.getTokenIf(TOK_UNDERSCORE) || check_cfg_inner(lex);
            lex.getTokenCheck(TOK_FATARROW);
            auto t = Parse_TT(lex, true);
            if(rv) {
                return box$( TTStreamO(sp, ParseState(), std::move(t)) );
            }
        }
        lex.getTokenCheck(TOK_EOF);

        ERROR(sp, E0000, "cfg_select - Nothing matched");
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
    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item&i) const override {
        TRACE_FUNCTION_FR("#[cfg] item - " << mi, (i.is_None() ? "Deleted" : ""));
        if( check_cfg(sp, mi) ) {
            // Leave
        }
        else {
            i = AST::Item::make_None({});
        }
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, AST::Impl& impl, const RcString& name, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item&i) const override {
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
            si.m_name = RcString();
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
            i.m_name = RcString();
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
STATIC_MACRO("cfg_select", CCfgSelectExpander);
STATIC_DECORATOR("cfg", CCfgHandler);
