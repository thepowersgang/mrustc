/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/mod.cpp
 * - Expand pass core code
 */
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>
#include <synext.hpp>
#include <map>
#include "../macro_rules/macro_rules.hpp"
#include "../parse/common.hpp"  // For reparse from macros
#include <ast/expr.hpp>
#include <hir/hir.hpp>  // For macro lookup
#include "cfg.hpp"
#include "common.hpp"
#include "../resolve/common.hpp"
#include "proc_macro.hpp"
#include "../parse/ttstream.hpp"

#define MAX_MACRO_RECURSION 200

DecoratorDef*   g_decorators_list = nullptr;
MacroDef*   g_macros_list = nullptr;
::std::map< RcString, ::std::unique_ptr<ExpandDecorator> >  g_decorators;
::std::map< RcString, ::std::unique_ptr<ExpandProcMacro> >  g_macros;
// HACK: Used for expanding proc macros, which need to re-parse without access to the current module
// - Parsing needs module for 1) anon modules, and 2) expanding `#[path]`
AST::Module*    g_current_mod = nullptr;

enum class ExpandMode {
    FirstPass,
    Iterate,
    Final,
};

struct ExpandState {
    ::AST::Crate& crate;
    LList<const AST::Module*> modstack;
    ExpandMode mode;
    mutable bool change;
    mutable bool has_missing;
    ExpandState(::AST::Crate& crate, LList<const AST::Module*> modstack, ExpandMode mode)
        : crate(crate)
        , modstack(modstack)
        , mode(mode)
        , change(false)
        , has_missing(false)
    {
        DEBUG("" << this);
    }
    explicit ExpandState(const ExpandState&) = default;
};

void Expand_Attrs(const ExpandState& es, const ::AST::AttributeList& attrs, AttrStage stage,  ::std::function<void(const ExpandDecorator& d,const ::AST::Attribute& a)> f);
void Expand_Mod(const ExpandState& es, ::AST::AbsolutePath modpath, ::AST::Module& mod, unsigned int first_item = 0);
void Expand_Expr(const ExpandState& es, ::AST::ExprNodeP& node);
void Expand_Expr(const ExpandState& es, AST::Expr& node);
void Expand_Expr(const ExpandState& es, ::std::shared_ptr<AST::ExprNode>& node);
void Expand_Path(const ExpandState& es, ::AST::Module& mod, ::AST::Path& p);

void Register_Synext_Decorator(::std::string name, ::std::unique_ptr<ExpandDecorator> handler) {
    g_decorators.insert(::std::make_pair( RcString::new_interned(name), mv$(handler) )); 
}
void Register_Synext_Macro(::std::string name, ::std::unique_ptr<ExpandProcMacro> handler) {
    g_macros.insert(::std::make_pair( RcString::new_interned(name), mv$(handler) ));
}
void Register_Synext_Decorator_Static(DecoratorDef* def) {
    def->prev = g_decorators_list;
    g_decorators_list = def;
}
void Register_Synext_Macro_Static(MacroDef* def) {
    def->prev = g_macros_list;
    g_macros_list = def;
}

void Expand_Init()
{
    // TODO: Initialise all macros here.
    void Expand_init_assert(); Expand_init_assert();
    void Expand_init_std_prelude(); Expand_init_std_prelude();
    void Expand_init_panic(); Expand_init_panic();

    // Fill macro/decorator map from init list
    while(g_decorators_list)
    {
        g_decorators.insert(::std::make_pair( RcString::new_interned(g_decorators_list->name), mv$(g_decorators_list->def) ));
        g_decorators_list = g_decorators_list->prev;
    }
    while(g_macros_list)
    {
        g_macros.insert(::std::make_pair(RcString::new_interned(g_macros_list->name), mv$(g_macros_list->def)));
        g_macros_list = g_macros_list->prev;
    }
}

void ExpandDecorator::unexpected(const Span& sp, const AST::Attribute& mi, const char* loc_str) const
{
    WARNING(sp, W0000, "Unexpected attribute " << mi.name() << " on " << loc_str);
}


ExpandProcMacro* Expand_FindProcMacro(const RcString& name)
{
    auto it = g_macros.find(name);
    if(it == g_macros.end())
        return nullptr;
    else
        return it->second.get();
}

void Expand_Attr(const ExpandState& es, const Span& sp, const ::AST::Attribute& a, AttrStage stage,  ::std::function<void(const Span& sp, const ExpandDecorator& d,const ::AST::Attribute& a)> f)
{
    bool found = false;
    if( a.name().elems.empty() ) {
        return ;
    }
    for( auto& d : g_decorators ) {
        if( a.name() == d.first ) {
            found = true;
            if( d.second->stage() != stage ) {
                DEBUG("#[" << d.first << "] s=" << (int)d.second->stage() << " != " << (int)stage);
            }
            else {
                if( !d.second->run_during_iter() ) {
                    switch(es.mode) {
                    case ExpandMode::FirstPass:
                        if( stage != AttrStage::Pre ) {
                            DEBUG("#[" << d.first << "] m=" << (int)es.mode);
                            continue ;
                        }
                        break;
                    case ExpandMode::Iterate:
                        continue ;
                    case ExpandMode::Final:
                        if( stage != AttrStage::Post ) {
                            DEBUG("#[" << d.first << "] m=" << (int)es.mode);
                            continue ;
                        }
                        break;
                    }
                }
                DEBUG("#[" << d.first << "]");
                f(sp, *d.second, a);
                // TODO: Annotate the attribute as having been handled
            }
        }
    }
    if( !found && stage == AttrStage::Pre ) {
        auto m = Expand_LookupMacro(sp, es.crate, es.modstack, a.name());
        if( !m.is_None() ) {
            if( const auto* proc_mac_p = m.opt_ExternalProcMacro() ) {
                const auto* proc_mac = *proc_mac_p;
                struct ProcMacroDecorator: public ExpandDecorator {
                    ::std::vector<RcString> mac_path;
                    AttrStage stage() const override { return AttrStage::Pre; }
                    bool run_during_iter() const override { return false; }

                    void handle(
                        const Span& sp,
                        const AST::Attribute& attr,
                        ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item& i
                        ) const override
                    {
                        if( !i.is_None() )
                        {
                            auto lex = ProcMacro_Invoke(sp, crate, this->mac_path, attr.data(), attrs, vis, path.nodes.back().c_str(), i);
                            if( lex ) {
                                i = AST::Item::make_None({});
                                lex->parse_state().module = &mod;
                                Parse_ModRoot_Items(*lex, mod);
                            }
                            else {
                                ERROR(sp, E0000, "proc_macro derive failed");
                            }
                        }
                    }
                    // Disabled due to lack of module to pass to parse.
                    // - Maybe modules shouldn't be linked in until after expand?
                    // - Because otherwise anon mods won't be #[cfg]'d out properly?
                    void handle(
                        const Span& sp,
                        const AST::Attribute& attr,
                        AST::Crate& crate, AST::Impl& impl, const RcString& name, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item& i
                        ) const override
                    {
                        if( !i.is_None() )
                        {
                            auto lex = ProcMacro_Invoke(sp, crate, this->mac_path, attr.data(), attrs, vis, name.c_str(), i);
                            if( lex ) {
                                i = AST::Item::make_None({});
                                assert(g_current_mod);
                                lex->parse_state().module = g_current_mod;
                                while( lex->lookahead(0) != TOK_EOF )
                                {
                                    Parse_Impl_Item(*lex, impl);
                                }
                            }
                            else {
                                ERROR(sp, E0000, "proc_macro derive failed");
                            }
                        }
                    }
                } d;
                d.mac_path.push_back(proc_mac->path.crate_name());
                d.mac_path.insert(d.mac_path.end(), proc_mac->path.components().begin(), proc_mac->path.components().end());
                // HACK: tracing's #[instrument] is very slow (with mrustc), so just ignore it (this is the rustc-1.74 version)
                if( d.mac_path.front() == "tracing_attributes-0_1_26" && d.mac_path.back() == "instrument" ) {
                }
                else {
                    f(sp, d, a);
                }
                found = true;
            }
        }
    }
    if( !found ) {
        // TODO: Create no-op handlers for a whole heap of attributes
        // - There's a LOT
        //WARNING(sp, W0000, "Unknown attribute #[" << a.name() << "]");
    }
}
void Expand_Attrs(const ExpandState& es, const ::AST::AttributeList& attrs, AttrStage stage,  ::std::function<void(const Span& sp, const ExpandDecorator& d,const ::AST::Attribute& a)> f)
{
    // TODO: Reduce load on derive etc by visiting `cfg` first?
    for( auto& a : attrs.m_items )
    {
        if( a.name() == "cfg" ) {
            Expand_Attr(es, a.span(), a, stage, f);
        }
    }
    for( auto& a : attrs.m_items )
    {
        Expand_Attr(es, a.span(), a, stage, f);
    }
}
void Expand_Attrs_CfgAttr(AST::AttributeList& attrs)
{
    for(auto it = attrs.m_items.begin(); it != attrs.m_items.end(); )
    {
        auto& a = *it;
        if( a.name() == "cfg_attr" ) {
            auto new_attrs = check_cfg_attr(a);
            it = attrs.m_items.erase(it);
            it = attrs.m_items.insert(it, std::make_move_iterator(new_attrs.begin()), std::make_move_iterator(new_attrs.end()));
        }
        else {
            ++ it;
        }
    }
}
namespace {
    slice<const AST::Attribute> get_attrs_after(const ::AST::AttributeList& attrs, const ::AST::Attribute& a) {
        const auto* start = &a + 1;
        const auto* end = &attrs.m_items.back()+1;
        return slice<const AST::Attribute>(start, end - start);
    }
}
void Expand_Attrs(const ExpandState& es, const ::AST::AttributeList& attrs, AttrStage stage,  const ::AST::AbsolutePath& path, ::AST::Module& mod, const AST::Visibility& vis, ::AST::Item& item)
{
    Expand_Attrs(es, attrs, stage,  [&](const Span& sp, const auto& d, const auto& a){
        if(!item.is_None()) {
            // Pass attributes _after_ this attribute
            d.handle(sp, a, es.crate, path, mod, get_attrs_after(attrs, a), vis, item);
        }
        });
}
void Expand_Attrs(const ExpandState& es, const ::AST::AttributeList& attrs, AttrStage stage,  const ::AST::AbsolutePath& path, ::AST::Module& mod, ::AST::Trait& trait, ::AST::Item& item)
{
    g_current_mod = &mod;
    Expand_Attrs(es, attrs, stage,  [&](const Span& sp, const auto& d, const auto& a){
        if(!item.is_None()) {
            d.handle(sp, a, es.crate, path, trait, get_attrs_after(attrs, a), item);
        }
        });
    g_current_mod = nullptr;
}
void Expand_Attrs(const ExpandState& es, const ::AST::AttributeList& attrs, AttrStage stage, ::AST::Module& mod, ::AST::Impl& impl, const AST::Visibility& vis, const RcString& name, ::AST::Item& item)
{
    g_current_mod = &mod;
    Expand_Attrs(es, attrs, stage,  [&](const Span& sp, const auto& d, const auto& a){
        if(!item.is_None()) {
            d.handle(sp, a, es.crate, impl, name, get_attrs_after(attrs, a), vis, item);
        }
        });
    g_current_mod = nullptr;
}
void Expand_Attrs(const ExpandState& es, const ::AST::AttributeList& attrs, AttrStage stage,  ::AST::Module& mod, ::AST::ImplDef& impl)
{
    Expand_Attrs(es, attrs, stage,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, mod, impl); });
}

bool Expand_Attrs_CfgOnly(const ExpandState& es, AST::AttributeList& attrs)
{
    bool remove = false;
    Expand_Attrs_CfgAttr(attrs);
    Expand_Attrs(es, attrs, AttrStage::Pre, [&](const Span& sp, const ExpandDecorator& d, const AST::Attribute& a) {
        if( a.name() == "cfg" ) {
            if( !check_cfg(sp, a) ) {
                remove = true;
            }
            return ;
        }
        else if( a.name() == "allow" ) {
            // Lazy allow allow
        }
        else {
            TODO(sp, "non-cfg attributes - " << a);
        }
        });
    return !remove;
}

MacroRef Expand_LookupMacro(const Span& mi_span, const ::AST::Crate& crate, LList<const AST::Module*> modstack, const AST::AttributeName& path)
{
    AST::Path   p = AST::Path::new_relative({}, {});
    for(const auto& ent : path.elems) {
        p += AST::PathNode(ent);
    }
    return Expand_LookupMacro(mi_span, crate, modstack, p);
}
MacroRef Expand_LookupMacro(const Span& mi_span, const ::AST::Crate& crate, LList<const AST::Module*> modstack, const AST::Path& path)
{
    ASSERT_BUG(mi_span, path.size() > 0, "Path should have nodes: " << path);

    if( path.is_trivial() )
    {
        const auto& name = path.as_trivial();
        // 1. Search compiler-provided proc macros
        if(auto* pm = Expand_FindProcMacro(name))
        {
            DEBUG("Found builtin");
            return MacroRef(pm);
        }

        // Iterate up the module tree, using the first located macro
        for(const auto* ll = &modstack; ll; ll = ll->m_prev)
        {
            const auto& mac_mod = *ll->m_item;
            DEBUG("Searching in " << mac_mod.path());
            for( const auto& mr : reverse(mac_mod.macros()) )
            {
                if( mr.name == name )
                {
                    DEBUG(mac_mod.path() << "::" << mr.name << " - Defined");
                    return MacroRef(&*mr.data);
                }
            }

            // Find the last macro of this name (allows later #[macro_use] definitions to override)
            MacroRef    rv;
            for( const auto& mri : mac_mod.m_macro_imports )
            {
                //DEBUG("- " << mri.name);
                if( mri.name == name )
                {
                    DEBUG("?::" << mri.name << " - Imported");
                    rv = mri.ref.clone();
                }
            }
            if( !rv.is_None() )
            {
                return rv;
            }
        }
        if( path.m_class.is_Local() )
        {
            DEBUG("Local path not resolved?");
            return MacroRef();
        }
    }

    // HACK: If the crate name is empty, look up builtins
    if( path.is_absolute() && path.m_class.as_Absolute().crate == "" && path.nodes().size() == 1 )
    {
        const auto& name = path.nodes()[0].name();
        if(auto* pm = Expand_FindProcMacro(name))
        {
            return MacroRef(pm);
        }
    }

    // Resolve the path, following use statements (if required)
    // - Only mr_ptr matters, as proc_mac is about builtins
    auto rv = Resolve_Lookup_Macro(mi_span, crate, modstack.m_item->path(), path, /*out_path=*/nullptr);
    TU_MATCH_HDRA( (rv), { )
    TU_ARMA(None, _e)
        return MacroRef();
    TU_ARMA(InternalMacro, pm)
        return pm;
    TU_ARMA(ProcMacro, pm)
        return pm;
    TU_ARMA(MacroRules, p)
        return p;
    }
    return MacroRef();
}

::std::unique_ptr<TokenStream> Expand_Macro_Inner(
    const ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod,
    Span mi_span, const AST::Path& path, const RcString& input_ident, TokenTree& input_tt
    )
{
    ASSERT_BUG(mi_span, path.is_valid(), "Macro invocation with invalid path");

    TRACE_FUNCTION_F("Searching for macro " << path);

    // Find the macro
    auto mac = Expand_LookupMacro(mi_span, crate, modstack, path);
    if( mac.is_MacroRules() )
    {
        // TODO: If `mr_ptr` is tagged with #[rustc_builtin_macro], look for a matching entry in `g_macros`
    }

    ::std::unique_ptr<TokenStream>  rv;
    TU_MATCH_HDRA( (mac), {)
    TU_ARMA(None, e) {
        DEBUG("Unknown macro " << path);
        return ::std::unique_ptr<TokenStream>();
        }
    TU_ARMA(ExternalProcMacro, proc_mac) {
        ::std::vector<RcString> mac_path;
        mac_path.push_back(proc_mac->path.crate_name());
        mac_path.insert(mac_path.end(), proc_mac->path.components().begin(), proc_mac->path.components().end());
        rv = ProcMacro_Invoke(mi_span, crate, mac_path, input_tt);
        }
    TU_ARMA(BuiltinProcMacro, proc_mac) {
        ASSERT_BUG(mi_span, proc_mac, "null BuiltinProcMacro? " << path);
        rv = input_ident == ""
            ? proc_mac->expand(mi_span, crate, input_tt, mod)
            : proc_mac->expand_ident(mi_span, crate, input_ident, input_tt, mod)
            ;
        }
    TU_ARMA(MacroRules, mr_ptr) {
        if( input_ident != "" )
            ERROR(mi_span, E0000, "macro_rules! macros can't take an ident");

        DEBUG("Invoking macro_rules " << path << " " << mr_ptr);
        rv = Macro_InvokeRules(path.is_trivial() ? path.as_trivial() : RcString::new_interned(FMT(path).c_str()), *mr_ptr, mi_span, mv$(input_tt), crate, mod);
        input_tt = TokenTree();
        }
    }
    ASSERT_BUG(mi_span, rv, "Macro invocation returned null tokentree");
    return rv;
}
::std::unique_ptr<TokenStream> Expand_Macro(
    const ExpandState& es, ::AST::Module& mod,
    Span mi_span, const AST::Path& path, const RcString& input_ident, TokenTree& input_tt
    )
{
    auto rv = Expand_Macro_Inner(es.crate, es.modstack, mod, mi_span, path, input_ident, input_tt);
    if( rv ) {
        es.change = true;
        DEBUG("Change flagged");
        rv->parse_state().crate = &es.crate;
        rv->parse_state().module = &mod;
    }
    else {
        // HACK: Allow the expansion to happen, even in final (e.g. if from derive)
        if( es.mode == ExpandMode::Final ) {
            ERROR(mi_span, E0000, "Unknown macro " << path);
        }
        DEBUG("Missing, waiting until another pass (set es.has_missing=true)");
        es.has_missing = true;
    }
    return rv;
}
::std::unique_ptr<TokenStream> Expand_Macro(const ExpandState& es, ::AST::Module& mod, ::AST::MacroInvocation& mi)
{
    return Expand_Macro(es, mod,  mi.span(), mi.path(), mi.input_ident(), mi.input_tt());
}

void Expand_Pattern(const ExpandState& es, ::AST::Module& mod, ::AST::Pattern& pat, bool is_refutable)
{
    TU_MATCH_HDRA( (pat.data()), {)
    TU_ARMA(MaybeBind, e) {
        }
    TU_ARMA(Macro, e) {
        const auto span = e.inv->span();

        auto tt = Expand_Macro(es, mod,  *e.inv);
        if( tt )
        {
            auto& lex = *tt;
            auto newpat = Parse_Pattern(lex);
            if( LOOK_AHEAD(lex) != TOK_EOF ) {
                ERROR(span, E0000, "Trailing tokens in macro expansion");
            }

            for(auto& b : pat.bindings()) {
                newpat.bindings().push_back( std::move(b) );
            }

            pat = mv$(newpat);
            Expand_Pattern(es, mod, pat, is_refutable);
        }
        }
    TU_ARMA(Any, e) {
        }
    TU_ARMA(Box, e) {
        Expand_Pattern(es, mod,  *e.sub, is_refutable);
        }
    TU_ARMA(Ref, e) {
        Expand_Pattern(es, mod,  *e.sub, is_refutable);
        }
    TU_ARMA(Value, e) {
        //Expand_Expr(crate, modstack, e.start);
        //Expand_Expr(crate, modstack, e.end);
        }
    TU_ARMA(ValueLeftInc, e) {
        //Expand_Expr(crate, modstack, e.start);
        //Expand_Expr(crate, modstack, e.end);
        }
    TU_ARMA(Tuple, e) {
        for(auto& sp : e.start)
            Expand_Pattern(es, mod, sp, is_refutable);
        for(auto& sp : e.end)
            Expand_Pattern(es, mod, sp, is_refutable);
        }
    TU_ARMA(StructTuple, e) {
        for(auto& sp : e.tup_pat.start)
            Expand_Pattern(es, mod, sp, is_refutable);
        for(auto& sp : e.tup_pat.end)
            Expand_Pattern(es, mod, sp, is_refutable);
        }
    TU_ARMA(Struct, e) {
        for(auto& subpat : e.sub_patterns) {
            if( !Expand_Attrs_CfgOnly(es, subpat.attrs) ) {
                subpat.name = RcString();
                continue ;
            }

            Expand_Pattern(es, mod, subpat.pat, is_refutable);
        }
        auto new_end = std::remove_if(e.sub_patterns.begin(), e.sub_patterns.end(), [&](const auto& e){ return e.name == ""; });
        e.sub_patterns.erase(new_end, e.sub_patterns.end());
        }
    TU_ARMA(Slice, e) {
        for(auto& sp : e.sub_pats)
            Expand_Pattern(es, mod, sp, is_refutable);
        }
    TU_ARMA(SplitSlice, e) {
        for(auto& sp : e.leading)
            Expand_Pattern(es, mod, sp, is_refutable);
        for(auto& sp : e.trailing)
            Expand_Pattern(es, mod, sp, is_refutable);
        }
    TU_ARMA(Or, e) {
        for(auto& sp : e)
            Expand_Pattern(es, mod, sp, is_refutable);
        }
    }
}

void Expand_Type(const ExpandState& es, ::AST::Module& mod, ::TypeRef& ty)
{
    TU_MATCH_HDRA( (ty.m_data), {)
    TU_ARMA(None, e) {
        }
    TU_ARMA(Any, e) {
        }
    TU_ARMA(Unit, e) {
        }
    TU_ARMA(Bang, e) {
        }
    TU_ARMA(Macro, e) {
        auto tt = Expand_Macro(es, mod,  *e.inv);
        if(tt)
        {
            auto new_ty = Parse_Type(*tt);
            if( tt->lookahead(0) != TOK_EOF )
                ERROR(e.inv->span(), E0000, "Extra tokens after parsed type");
            ty = mv$(new_ty);

            Expand_Type(es, mod,  ty);
        }
        }
    TU_ARMA(Primitive, e) {
        }
    TU_ARMA(Function, e) {
        Type_Function& tf = e.info;
        Expand_Type(es, mod,  *tf.m_rettype);
        for(auto& st : tf.m_arg_types)
            Expand_Type(es, mod,  st);
        }
    TU_ARMA(Tuple, e) {
        for(auto& st : e.inner_types)
            Expand_Type(es, mod,  st);
        }
    TU_ARMA(Borrow, e) {
        Expand_Type(es, mod,  *e.inner);
        }
    TU_ARMA(Pointer, e) {
        Expand_Type(es, mod,  *e.inner);
        }
    TU_ARMA(Array, e) {
        Expand_Type(es, mod,  *e.inner);
        if( e.size ) {
            Expand_Expr(es,  e.size);
        }
        }
    TU_ARMA(Slice, e) {
        Expand_Type(es, mod,  *e.inner);
        }
    TU_ARMA(Generic, e) {
        }
    TU_ARMA(Path, e) {
        Expand_Path(es, mod,  *e);
        }
    TU_ARMA(TraitObject, e) {
        for(auto& p : e.traits)
        {
            // TODO: p.hrbs? Not needed until types are in those
            Expand_Path(es, mod,  *p.path);
        }
        }
    TU_ARMA(ErasedType, e) {
        for(auto& p : e.traits)
        {
            // TODO: p.hrbs?
            Expand_Path(es, mod,  *p.path);
        }
        }
    }
}
void Expand_PathParams(const ExpandState& es, ::AST::Module& mod, ::AST::PathParams& params)
{
    for(auto& e : params.m_entries)
    {
        TU_MATCH_HDRA( (e), {)
        TU_ARMA(Null, _) {
            }
        TU_ARMA(Lifetime, _) {
            }
        TU_ARMA(Type, typ) {
            Expand_Type(es, mod, typ);
            }
        TU_ARMA(Value, node) {
            Expand_Expr(es, node);
            }
        TU_ARMA(AssociatedTyEqual, aty) {
            Expand_Type(es, mod, aty.second);
            }
        TU_ARMA(AssociatedTyBound, aty) {
            Expand_Path(es, mod, aty.second);
            }
        }
    }
}
void Expand_Path(const ExpandState& es, ::AST::Module& mod, ::AST::Path& p)
{
    auto expand_nodes = [&](::std::vector<::AST::PathNode>& nodes) {
        for(auto& node : nodes)
        {
            Expand_PathParams(es, mod, node.args());
        }
        };

    TU_MATCH_HDRA( (p.m_class), {)
    TU_ARMA(Invalid, pe) {
        }
    TU_ARMA(Local, pe) {
        }
    TU_ARMA(Relative, pe) {
        expand_nodes(pe.nodes);
        }
    TU_ARMA(Self, pe) {
        expand_nodes(pe.nodes);
        }
    TU_ARMA(Super, pe) {
        expand_nodes(pe.nodes);
        }
    TU_ARMA(Absolute, pe) {
        expand_nodes(pe.nodes);
        }
    TU_ARMA(UFCS, pe) {
        Expand_Type(es, mod, *pe.type);
        if( pe.trait ) {
            Expand_Path(es, mod, *pe.trait);
        }
        expand_nodes(pe.nodes);
        }
    }
}
namespace {
    static AST::Path get_path(const RcString& core_crate, const char* c1, const char* c2) {
        return AST::AbsolutePath(core_crate, { RcString::new_interned(c1), RcString::new_interned(c2) });
    }
    static AST::Path get_path(const RcString& core_crate, const char* c1, const char* c2, const char* c3) {
        return AST::AbsolutePath(core_crate, { RcString::new_interned(c1), RcString::new_interned(c2), RcString::new_interned(c3) });
    }
}

struct CExpandExpr:
    public ::AST::NodeVisitor
{
    ::AST::Crate&    crate;
    const ExpandState& parent_expand_state;
    ExpandState expand_state;
    ::AST::ExprNodeP replacement;

    // Stack of `try { ... }` blocks (the string is the loop label for the desugaring)
    ::std::vector<RcString>   m_try_stack;
    unsigned m_try_index = 0;

    AST::ExprNode_Block*    current_block = nullptr;
    bool in_assign_lhs = false;

    CExpandExpr(const ExpandState& es)
        : crate(es.crate)
        , parent_expand_state(es)
        , expand_state(es)
    {
    }
    ~CExpandExpr()
    {
        if( expand_state.change ) {
            if( ! parent_expand_state.change ) {
                DEBUG("Propagate change");
            }
            parent_expand_state.change = true;
        }
        if( expand_state.has_missing ) {
            if( ! parent_expand_state.has_missing ) {
                DEBUG("Propagate has missing (to " << &parent_expand_state << ")");
            }
            parent_expand_state.has_missing = true;
        }
    }

    ::AST::Module& cur_mod() {
        return *const_cast< ::AST::Module*>(expand_state.modstack.m_item);
    }

    void visit(::AST::ExprNodeP& cnode) {
        if(cnode.get())
        {
            auto attrs = mv$(cnode->attrs());
            Expand_Attrs_CfgAttr(attrs);
            Expand_Attrs(expand_state, attrs, AttrStage::Pre,  [&](const Span& sp, const ExpandDecorator& d, const auto& a){ d.handle(sp, a, this->crate, cnode); });
            if(cnode.get())
                cnode->attrs() = mv$(attrs);
        }
        if(cnode.get())
        {
            cnode->visit(*this);
            // If the node was a macro, and it was consumed, reset it
            if( auto* n_mac = dynamic_cast<AST::ExprNode_Macro*>(cnode.get()) )
            {
                if( !n_mac->m_path.is_valid() )
                    cnode.reset();
            }
            if( this->replacement ) {
                cnode = mv$(this->replacement);
            }
        }

        if(cnode.get())
        {
            auto attrs = mv$(cnode->attrs());
            Expand_Attrs(expand_state, attrs, AttrStage::Post,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, this->crate, cnode); });
            if(cnode.get())
                cnode->attrs() = mv$(attrs);
        }
        assert( ! this->replacement );
    }
    void visit_nodelete(const ::AST::ExprNode& parent, ::AST::ExprNodeP& cnode) {
        if( cnode.get() != nullptr )
        {
            this->visit(cnode);
            if(cnode.get() == nullptr)
                ERROR(parent.span(), E0000, "#[cfg] not allowed in this position");
        }
        assert( ! this->replacement );
    }
    void visit_vector(::std::vector< ::AST::ExprNodeP >& cnodes) {
        for( auto it = cnodes.begin(); it != cnodes.end(); ) {
            assert( it->get() );
            this->visit(*it);
            if( it->get() == nullptr ) {
                it = cnodes.erase( it );
            }
            else {
                ++ it;
            }
        }
    }

    ::AST::ExprNodeP visit_macro(::AST::ExprNode_Macro& node, ::std::vector< ::AST::ExprNodeP>* nodes_out)
    {
        TRACE_FUNCTION_F(node.m_path << "!");
        if( !node.m_path.is_valid() ) {
            return ::AST::ExprNodeP();
        }

        ::AST::ExprNodeP    rv;
        auto& mod = this->cur_mod();
        auto ttl = Expand_Macro( expand_state, mod,  node.span(),  node.m_path, node.m_ident, node.m_tokens );
        if( !ttl.get() )
        {
            // No expansion
            DEBUG("Deferred");
        }
        else
        {
            while( ttl->lookahead(0) != TOK_EOF )
            {
                SET_MODULE( (*ttl), mod );

                // Reparse as expression / item
                bool    add_silence_if_end = false;
                ::std::shared_ptr< AST::Module> tmp_local_mod;
                auto& local_mod_ptr = (this->current_block ? this->current_block->m_local_mod : tmp_local_mod);
                DEBUG("-- Parsing as expression line");
                auto newexpr = Parse_ExprBlockLine_WithItems(*ttl, local_mod_ptr, add_silence_if_end);

                if( tmp_local_mod )
                    TODO(node.span(), "Handle edge case where a macro expansion outside of a _Block creates an item");

                if( newexpr )
                {
                    if( nodes_out ) {
                        nodes_out->push_back( mv$(newexpr) );
                    }
                    else {
                        assert( !rv );
                        rv = mv$(newexpr);
                    }
                }
                else
                {
                    // Expansion line just added a new item
                }

                if( ttl->lookahead(0) != TOK_EOF )
                {
                    if( !nodes_out ) {
                        ERROR(node.span(), E0000, "Unused tokens at the end of macro expansion - " << ttl->getToken());
                    }
                }
            }
            node.m_path = AST::Path();

            if( !nodes_out && !rv )
            {
                ERROR(node.span(), E0000, "Macro didn't expand to anything");
            }
        }

        return mv$(rv);
    }

    void visit(::AST::ExprNode_Macro& node) override
    {
        TRACE_FUNCTION_F("ExprNode_Macro - name = " << node.m_path);
        if( !node.m_path.is_valid() ) {
            return ;
        }

        replacement = this->visit_macro(node, nullptr);

        if( this->replacement )
        {
            DEBUG("--- Visiting new node");
            auto n = mv$(this->replacement);
            this->visit(n);
            if( n )
            {
                assert( !this->replacement );
                this->replacement = mv$(n);
            }
        }
    }

    void visit(::AST::ExprNode_Block& node) override {
        unsigned int mod_item_count = 0;

        auto prev_modstack = this->expand_state.modstack;
        if( node.m_local_mod ) {
            this->expand_state.modstack = LList<const ::AST::Module*>(&prev_modstack, node.m_local_mod.get());
        }

        // TODO: macro_rules! invocations within the expression list influence this.
        // > Solution: Defer creation of the local module until during expand.
        if( node.m_local_mod ) {
            Expand_Mod(this->expand_state, node.m_local_mod->path(), *node.m_local_mod);
            mod_item_count = node.m_local_mod->m_items.size();
        }

        auto saved = this->current_block;
        this->current_block = &node;

        for( auto it = node.m_nodes.begin(); it != node.m_nodes.end(); )
        {
            assert( it->get() );

            if( auto* node_mac = dynamic_cast<::AST::ExprNode_Macro*>(it->get()) )
            {
                auto attrs = std::move( (*it)->attrs() );
                Expand_Attrs_CfgAttr( attrs );
                Expand_Attrs(expand_state, attrs, AttrStage::Pre,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, this->crate, *it); });
                if( !it->get() ) {
                    it = node.m_nodes.erase( it );
                    continue ;
                }
                (*it)->attrs() = std::move(attrs);

                assert(it->get() == node_mac);

                ::std::vector< ::AST::ExprNodeP>    new_nodes;
                this->visit_macro(*node_mac, &new_nodes);

                if(node_mac->m_path.is_valid()) {
                    DEBUG("Deferred macro");
                    ++ it;
                }
                else {
                    it = node.m_nodes.erase(it);
                    it = node.m_nodes.insert(it, ::std::make_move_iterator(new_nodes.begin()), ::std::make_move_iterator(new_nodes.end()));
                    // NOTE: Doesn't advance the iterator above, we want to re-visit the new node
                }
            }
            else
            {
                this->visit(*it);
                if( it->get() == nullptr ) {
                    it = node.m_nodes.erase( it );
                }
                else {
                    ++ it;
                }
            }
        }

        this->current_block = saved;

        // HACK! Run Expand_Mod twice on local modules.
        if( node.m_local_mod ) {
            Expand_Mod(this->expand_state, node.m_local_mod->path(), *node.m_local_mod, mod_item_count);
        }

        this->expand_state.modstack = mv$(prev_modstack);
    }
    void visit(::AST::ExprNode_Try& node) override {
        // Desugar into
        // ```
        // loop '#tryNNN {
        //   break '#tryNNN { ... }
        // }
        // ```
        // NOTE: MIR lowering and HIR typecheck need to know to skip these (OR resolve should handle naming all loop blocks)
        m_try_stack.push_back(RcString::new_interned(FMT("#try" << m_try_index++)));
        this->visit_nodelete(node, node.m_inner);
        auto loop_name = mv$(m_try_stack.back());
        m_try_stack.pop_back();

        auto core_crate = crate.m_ext_cratename_core;
        AST::ExprNodeP  ok_node;
        if(TARGETVER_MOST_1_39)
        {
            auto path_Ok  = get_path(core_crate, "result", "Result", "Ok");
            ok_node = ::AST::ExprNodeP(new ::AST::ExprNode_CallPath( mv$(path_Ok), ::make_vec1(mv$(node.m_inner)) ));
        }
        else
        {
            auto path_Try = get_path(core_crate, "ops", "Try");
            auto path_Try_from_output = ::AST::Path::new_ufcs_trait(::TypeRef(node.span()), path_Try, { ::AST::PathNode(RcString::new_interned("from_output")) });
            ok_node = ::AST::ExprNodeP(new ::AST::ExprNode_CallPath( mv$(path_Try_from_output), ::make_vec1(mv$(node.m_inner)) ));
        }
        auto break_node = AST::ExprNodeP(new AST::ExprNode_Flow(AST::ExprNode_Flow::BREAK, loop_name, mv$(ok_node)));
        this->replacement = AST::ExprNodeP(new AST::ExprNode_Loop(loop_name, mv$(break_node)));
    }
    void visit(::AST::ExprNode_Asm& node) override {
        for(auto& v : node.m_output)
            this->visit_nodelete(node, v.value);
        for(auto& v : node.m_input)
            this->visit_nodelete(node, v.value);
    }
    void visit(::AST::ExprNode_Asm2& node) override {
        for(auto& v : node.m_params)
        {
            TU_MATCH_HDRA((v), {)
            TU_ARMA(Const, e) {
                this->visit_nodelete(node, e);
                }
            TU_ARMA(Sym, e) {
                Expand_Path(this->expand_state, this->cur_mod(), e);
                }
            TU_ARMA(RegSingle, e) {
                this->visit_nodelete(node, e.val);
                }
            TU_ARMA(Reg, e) {
                this->visit_nodelete(node, e.val_in);
                this->visit_nodelete(node, e.val_out);
                }
            }
        }
    }
    void visit(::AST::ExprNode_Flow& node) override {
        this->visit_nodelete(node, node.m_value);

        if( node.m_type == AST::ExprNode_Flow::YEET )
        {
            auto core_crate = crate.m_ext_cratename_core;
            auto path_ops_Yeet = get_path(core_crate, "ops", "Yeet");
            auto path_FromResidual_from_residual = get_path(core_crate, "ops", "FromResidual", "from_residual");

            auto v = ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                ::AST::Path(path_ops_Yeet),
                ::make_vec1( std::move(node.m_value) )
                ));
            v->set_span(node.span());
            v = ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                ::AST::Path(path_FromResidual_from_residual),
                ::make_vec1( std::move(v) )
                ));
            v->set_span(node.span());
            replacement = ::AST::ExprNodeP(new ::AST::ExprNode_Flow(
                (m_try_stack.empty() ? ::AST::ExprNode_Flow::RETURN : ::AST::ExprNode_Flow::BREAK),   // NOTE: uses `break 'tryblock` instead of return if in a try block.
                (m_try_stack.empty() ? RcString("") : m_try_stack.back()),
                std::move(v)
                ));
            replacement->set_span(node.span());
        }
    }
    void visit(::AST::ExprNode_LetBinding& node) override {
        Expand_Type(this->expand_state, this->cur_mod(),  node.m_type);
        Expand_Pattern(this->expand_state, this->cur_mod(),  node.m_pat, false);
        this->visit_nodelete(node, node.m_value);
        this->visit_nodelete(node, node.m_else);
    }
    void visit(::AST::ExprNode_Assign& node) override {
        in_assign_lhs = true;
        this->visit_nodelete(node, node.m_slot);
        in_assign_lhs = false;
        this->visit_nodelete(node, node.m_value);

        // Desugar destructuring assignment
        // https://rust-lang.github.io/rfcs/2909-destructuring-assignment.html
        if( node.m_op == ::AST::ExprNode_Assign::NONE )
        {
            struct VisitorToPat:
                public ::AST::NodeVisitor
            {
                std::vector<std::pair<RcString,AST::ExprNodeP>> m_slots;
                ::AST::Pattern  m_rv;

                bool m_rv_set = false;
                bool m_is_slot = false;

                ::AST::Pattern lower(::AST::ExprNodeP& ep) {
                    assert(ep);
                    ep->visit(*this);
                    ASSERT_BUG(ep->span(), m_rv_set, ep.type_name() << " - Didn't yield a pattern");
                    if(m_is_slot) {
                        assert(!m_slots.empty());
                        assert(!m_slots.back().second);
                        m_slots.back().second = std::move(ep);
                        m_is_slot = false;
                    }
                    m_rv_set = false;
                    return std::move(m_rv);
                }

                // - This is a de-structuring pattern
                void pat(::AST::Pattern rv) {
                    assert(!m_rv_set);
                    assert(!m_is_slot);
                    m_rv_set = true;
                    assert(rv.bindings().empty());
                    m_rv = std::move(rv);
                }
                // - This is a slot (to be assigned)
                void slot(::AST::ExprNode& v) {
                    m_rv_set = true;
                    m_is_slot = true;

                    RcString   name(FMT("_#" << m_slots.size()).c_str());
                    m_slots.push_back(std::make_pair(name, AST::ExprNodeP()));
                    m_rv = AST::Pattern(AST::Pattern::TagBind(), v.span(), m_slots.back().first);
                }
                // - The given node isn't valid on the LHS of an assignment
                void invalid(const ::AST::ExprNode& v) {
                    ERROR(v.span(), E0000, typeid(v).name() << " isn't valid on the LHS of an assignemnt");
                }

                void visit(::AST::ExprNode_Block& v) override { invalid(v); }
                void visit(::AST::ExprNode_Try  & v) override { invalid(v); }
                void visit(::AST::ExprNode_Macro& v) override { BUG(v.span(), "Encountered macro"); }
                void visit(::AST::ExprNode_Asm & v) override { invalid(v); }
                void visit(::AST::ExprNode_Asm2& v) override { invalid(v); }
                void visit(::AST::ExprNode_Flow& v) override { invalid(v); }
                void visit(::AST::ExprNode_LetBinding& v) override { invalid(v); }
                void visit(::AST::ExprNode_Assign& v) override { invalid(v); }
                void visit(::AST::ExprNode_CallPath  & v) override { invalid(v); }
                void visit(::AST::ExprNode_CallMethod& v) override { invalid(v); }
                void visit(::AST::ExprNode_CallObject& v) override { invalid(v); }
                void visit(::AST::ExprNode_Loop& v) override { invalid(v); }
                void visit(::AST::ExprNode_WhileLet& v) override { invalid(v); }
                void visit(::AST::ExprNode_Match& v) override { invalid(v); }
                void visit(::AST::ExprNode_If& v) override { invalid(v); }
                void visit(::AST::ExprNode_IfLet& v) override { invalid(v); }

                void visit(::AST::ExprNode_WildcardPattern& v) override {
                    m_rv_set = true;
                    m_rv = AST::Pattern(v.span(), AST::Pattern::Data::make_Any({}));
                }
                void visit(::AST::ExprNode_Integer& v) override { invalid(v); }
                void visit(::AST::ExprNode_Float& v) override { invalid(v); }
                void visit(::AST::ExprNode_Bool& v) override { invalid(v); }
                void visit(::AST::ExprNode_String& v) override { invalid(v); }
                void visit(::AST::ExprNode_ByteString& v) override { invalid(v); }
                void visit(::AST::ExprNode_Closure& v) override { invalid(v); }

                void visit(::AST::ExprNode_StructLiteral& v) override {
                    if( v.m_base_value ) {
                        TODO(v.span(), "Struct literal with `..value` in destructured assignment");
                    }
                    std::vector<AST::StructPatternEntry>   subpats;
                    for(auto& m : v.m_values) {
                        subpats.push_back(AST::StructPatternEntry { std::move(m.attrs), m.name, lower(m.value) });
                    }
                    pat(AST::Pattern(AST::Pattern::TagStruct(), v.span(), v.m_path, std::move(subpats), true));
                }
                void visit(::AST::ExprNode_StructLiteralPattern& v) override {
                    std::vector<AST::StructPatternEntry>   subpats;
                    for(auto& m : v.m_values) {
                        subpats.push_back(AST::StructPatternEntry { std::move(m.attrs), m.name, lower(m.value) });
                    }
                    pat(AST::Pattern(AST::Pattern::TagStruct(), v.span(), v.m_path, std::move(subpats), false));
                }
                void visit(::AST::ExprNode_Array& v) override {
                    if( v.m_size ) {
                        TODO(v.span(), "Sized Array literal in destructured assignment");
                    }
                    else {
                        std::vector<AST::Pattern>   subpats;
                        for(auto& m : v.m_values) {
                            subpats.push_back(lower(m));
                        }
                        pat(AST::Pattern(v.span(), AST::Pattern::Data::make_Slice({ std::move(subpats) })));
                    }
                }
                void visit(::AST::ExprNode_Tuple& v) override {
                    bool is_split = false;
                    std::vector<AST::Pattern>   subpats_start;
                    std::vector<AST::Pattern>   subpats;
                    for(auto& m : v.m_values) {
                        if( const auto* e = dynamic_cast<AST::ExprNode_BinOp*>(m.get()) ) {
                            if( e->m_type == ::AST::ExprNode_BinOp::RANGE && !e->m_left && !e->m_right ) {
                                ASSERT_BUG(v.span(), !is_split, "Multiple `..` in tuple pattern?");
                                is_split = true;
                                subpats_start = std::move(subpats);
                                continue ;
                            }
                        }
                        subpats.push_back(lower(m));
                    }
                    if( is_split ) {
                        pat(AST::Pattern(AST::Pattern::TagTuple(), v.span(), AST::Pattern::TuplePat { std::move(subpats_start), true, std::move(subpats) }));
                    }
                    else {
                        pat(AST::Pattern(AST::Pattern::TagTuple(), v.span(), std::move(subpats)));
                    }
                }

                // Just emit as if it's a slot, `UnitStruct = Foo` isn't valid
                void visit(::AST::ExprNode_NamedValue& v) override { slot(v); }
                void visit(::AST::ExprNode_Field& v) override { slot(v); }
                void visit(::AST::ExprNode_Index& v) override { slot(v); }
                void visit(::AST::ExprNode_Deref& v) override { slot(v); }

                void visit(::AST::ExprNode_Cast& v) override { invalid(v); }
                void visit(::AST::ExprNode_TypeAnnotation& v) override { invalid(v); }
                void visit(::AST::ExprNode_BinOp& v) override { invalid(v); }
                void visit(::AST::ExprNode_UniOp& v) override { invalid(v); }
            } v;
            auto pat = v.lower(node.m_slot);
            if(pat.bindings().size() > 0) {
                assert(pat.bindings().size() == 1); // The above code shouldn't be making double bindings
                assert(!node.m_slot);
                assert(v.m_slots.front().second);
                node.m_slot = std::move(v.m_slots.front().second);
            }
            else {
                // Create a block with a `let` and individual assignments
                auto rv = new AST::ExprNode_Block();
                rv->m_yields_final_value = false;
                rv->m_nodes.push_back(AST::ExprNodeP(new AST::ExprNode_LetBinding(std::move(pat), TypeRef(node.span()), std::move(node.m_value))));
                for(auto& slots : v.m_slots) {
                    rv->m_nodes.push_back(AST::ExprNodeP(new AST::ExprNode_Assign(AST::ExprNode_Assign::NONE,
                        std::move(slots.second),
                        AST::ExprNodeP(new AST::ExprNode_NamedValue(AST::Path::new_local(std::move(slots.first))))
                        )));
                }
                this->replacement = AST::ExprNodeP(rv);
            }
        }
    }
    void visit(::AST::ExprNode_CallPath& node) override {
        Expand_Path(this->expand_state, this->cur_mod(),  node.m_path);
        this->visit_vector(node.m_args);
    }
    void visit(::AST::ExprNode_CallMethod& node) override {
        Expand_PathParams(this->expand_state, this->cur_mod(), node.m_method.args());
        this->visit_nodelete(node, node.m_val);
        this->visit_vector(node.m_args);
    }
    void visit(::AST::ExprNode_CallObject& node) override {
        this->visit_nodelete(node, node.m_val);
        this->visit_vector(node.m_args);
    }
    void visit(::AST::ExprNode_Loop& node) override {
        this->visit_nodelete(node, node.m_cond);
        this->visit_nodelete(node, node.m_code);
        Expand_Pattern(this->expand_state, this->cur_mod(),  node.m_pattern, false);
        if(node.m_type == ::AST::ExprNode_Loop::FOR)
        {
            static const RcString rcstring_into_iter = RcString::new_interned("into_iter");
            static const RcString rcstring_next = RcString::new_interned("next");
            static const RcString rcstring_it = RcString::new_interned("it");
            auto core_crate = crate.m_ext_cratename_core;
            auto path_Some = get_path(core_crate, "option", "Option", "Some");
            auto path_None = get_path(core_crate, "option", "Option", "None");
            auto path_IntoIterator = get_path(core_crate, "iter", "IntoIterator");
            auto path_Iterator     = get_path(core_crate, "iter", "Iterator"    );
            // Desugar into:
            // {
            //     match <_ as ::iter::IntoIterator>::into_iter(`m_cond`) {
            //     mut it => {
            //         `m_label`: loop {
            //             match ::iter::Iterator::next(&mut it) {
            //             Some(`m_pattern`) => `m_code`,
            //             None => break `m_label`,
            //             }
            //         }
            //     }
            // }
            ::std::vector< ::AST::ExprNode_Match_Arm>   arms;
            // - `Some(pattern ) => code`
            arms.push_back( ::AST::ExprNode_Match_Arm(
                ::make_vec1( ::AST::Pattern(::AST::Pattern::TagNamedTuple(), node.span(), path_Some, ::make_vec1( mv$(node.m_pattern) ) ) ),
                {},
                mv$(node.m_code)
                ) );
            // - `None => break label`
            arms.push_back( ::AST::ExprNode_Match_Arm(
                ::make_vec1( ::AST::Pattern(::AST::Pattern::TagValue(), node.span(), ::AST::Pattern::Value::make_Named(path_None)) ),
                {},
                ::AST::ExprNodeP(new ::AST::ExprNode_Flow(::AST::ExprNode_Flow::BREAK, node.m_label, nullptr))
                ) );

            replacement.reset(new ::AST::ExprNode_Match(
                ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                    ::AST::Path::new_ufcs_trait( ::TypeRef(node.span()), path_IntoIterator, { ::AST::PathNode(rcstring_into_iter) } ),
                    ::make_vec1( mv$(node.m_cond) )
                    )),
                ::make_vec1(::AST::ExprNode_Match_Arm(
                    ::make_vec1( ::AST::Pattern(::AST::Pattern::TagBind(), node.span(), rcstring_it) ),
                    {},
                    ::AST::ExprNodeP(new ::AST::ExprNode_Loop(
                        node.m_label,
                        ::AST::ExprNodeP(new ::AST::ExprNode_Match(
                            ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                                ::AST::Path::new_ufcs_trait( ::TypeRef(node.span()), path_Iterator, { ::AST::PathNode(rcstring_next) } ),
                                ::make_vec1( ::AST::ExprNodeP(new ::AST::ExprNode_UniOp(
                                    ::AST::ExprNode_UniOp::REFMUT,
                                    ::AST::ExprNodeP(new ::AST::ExprNode_NamedValue( ::AST::Path(rcstring_it) ))
                                    )) )
                                )),
                            mv$(arms)
                            ))
                        )) )
                    )
                ) );
        }
    }

    AST::ExprNodeP desugar_let_chain(const Span& sp, std::vector<AST::IfLet_Condition>& conditions, AST::ExprNodeP true_block, std::function<AST::ExprNodeP()> get_false_block)
    {
        // Iterate the conditions in reverse, because this is being built from the innermost block
        for(size_t i = conditions.size(); i--; )
        {
            auto& cond = conditions[i];
            auto node_nomatch = get_false_block();
            node_nomatch->set_span(sp);
            if( cond.opt_pat ) {
                ::std::vector< ::AST::ExprNode_Match_Arm>   arms;
                // - `pattern => body`
                arms.push_back( ::AST::ExprNode_Match_Arm(
                    ::make_vec1( std::move(*cond.opt_pat) ),
                    {},
                    std::move(true_block)
                ) );
                // - `_ => break,`
                arms.push_back( ::AST::ExprNode_Match_Arm(
                    ::make_vec1( ::AST::Pattern() ),
                    {},
                    std::move(node_nomatch)
                ) );
                true_block.reset(new ::AST::ExprNode_Match( std::move(cond.value), std::move(arms) ));
            }
            else {
                true_block.reset(new ::AST::ExprNode_If( std::move(cond.value), std::move(true_block), std::move(node_nomatch) ));
            }
            true_block->set_span(sp);
        }
        return true_block;
    }

    void visit(::AST::ExprNode_WhileLet& node) override {
        for(auto& cond : node.m_conditions) {
            if( cond.opt_pat ) {
                Expand_Pattern(this->expand_state, this->cur_mod(),  *cond.opt_pat, true);
            }
            this->visit_nodelete(node, cond.value);
        }
        this->visit_nodelete(node, node.m_code);

        // Desugar into:
        /*
         * loop {
         *   match val_a {
         *    pat_a => match val_b {
         *      pat_b => ...,
         *      _ => break,
         *      },
         *   _ => break,
         *   }
         * }
         */
        auto node_body = desugar_let_chain(node.span(), node.m_conditions,
            std::move(node.m_code),
            [&](){ return AST::ExprNodeP { new AST::ExprNode_Flow(AST::ExprNode_Flow::BREAK, node.m_label, AST::ExprNodeP()) }; }
        );
        replacement.reset(new ::AST::ExprNode_Loop(node.m_label, std::move(node_body) ));
        replacement->set_span(node.span());
    }
    void visit(::AST::ExprNode_Match& node) override {
        this->visit_nodelete(node, node.m_val);
        for(auto& arm : node.m_arms)
        {
            Expand_Attrs_CfgAttr( arm.m_attrs );
            Expand_Attrs(expand_state, arm.m_attrs, AttrStage::Pre ,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, crate,  arm); });
            if( arm.m_patterns.size() == 0 )
                continue ;
            for(auto& pat : arm.m_patterns) {
                Expand_Pattern(this->expand_state, this->cur_mod(),  pat, true);
            }
            for(auto& cond : arm.m_guard) {
                if( cond.opt_pat ) {
                    Expand_Pattern(this->expand_state, this->cur_mod(),  *cond.opt_pat, true);
                }
                this->visit_nodelete(node, cond.value);
            }

            this->visit_nodelete(node, arm.m_code);
            Expand_Attrs(expand_state, arm.m_attrs, AttrStage::Post,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, crate,  arm); });
        }
        // Prune deleted arms
        for(auto it = node.m_arms.begin(); it != node.m_arms.end(); ) {
            if( it->m_patterns.size() == 0 )
                it = node.m_arms.erase(it);
            else
                ++ it;
        }
    }
    void visit(::AST::ExprNode_If& node) override {
        this->visit_nodelete(node, node.m_cond);
        this->visit_nodelete(node, node.m_true);
        this->visit_nodelete(node, node.m_false);
    }
    void visit(::AST::ExprNode_IfLet& node) override {
        for(auto& cond : node.m_conditions) {
            if( cond.opt_pat ) {
                Expand_Pattern(this->expand_state, this->cur_mod(),  *cond.opt_pat, true);
            }
            this->visit_nodelete(node, cond.value);
        }
        this->visit_nodelete(node, node.m_true);
        this->visit_nodelete(node, node.m_false);

        // TODO: Desugar into:
        /*
        * '#iflet: {
        *   match val_a {
        *    pat_a => match val_b {
        *      pat_b => break '#iflet ({ true body }),
        *      _ => {},
        *      },
        *   _ => {},
        *   }
        *   false body
        * }
        */
#if 1
        Ident   label({}, RcString::new_interned("#iflet"));
        auto node_body = desugar_let_chain(node.span(), node.m_conditions,
            AST::ExprNodeP { new AST::ExprNode_Flow(AST::ExprNode_Flow::BREAK, label, std::move(node.m_true)) },
            [&](){ return AST::ExprNodeP { new AST::ExprNode_Block() }; }
        );
        // - Create the block
        auto* replacement = new ::AST::ExprNode_Block();
        replacement->m_label = label;
        replacement->m_nodes.push_back(std::move(node_body));
        if(node.m_false) {
            replacement->m_nodes.push_back(std::move(node.m_false));
        }
        else {
            replacement->m_nodes.push_back(new AST::ExprNode_Tuple({}));
        }
        replacement->m_nodes.back()->set_span(node.span());
        replacement->m_yields_final_value = true;
        replacement->set_span(node.span());
        this->replacement.reset(replacement);
#endif
    }
    void visit(::AST::ExprNode_WildcardPattern& node) override { }
    void visit(::AST::ExprNode_Integer& node) override { }
    void visit(::AST::ExprNode_Float& node) override { }
    void visit(::AST::ExprNode_Bool& node) override { }
    void visit(::AST::ExprNode_String& node) override { }
    void visit(::AST::ExprNode_ByteString& node) override { }
    void visit(::AST::ExprNode_Closure& node) override {
        auto try_stack = ::std::move(m_try_stack);
        for(auto& arg : node.m_args) {
            Expand_Pattern(this->expand_state, this->cur_mod(),  arg.first, false);
            Expand_Type(this->expand_state, this->cur_mod(),  arg.second);
        }
        Expand_Type(this->expand_state, this->cur_mod(),  node.m_return);
        this->visit_nodelete(node, node.m_code);
        m_try_stack = std::move(try_stack);
    }
    void visit(::AST::ExprNode_StructLiteral& node) override {
        this->visit_nodelete(node, node.m_base_value);
        for(auto& val : node.m_values)
        {
            Expand_Attrs_CfgAttr(val.attrs);
            Expand_Attrs(expand_state, val.attrs, AttrStage::Pre ,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, crate,  val); });
            if( !val.value )
                continue ;
            this->visit_nodelete(node, val.value);
            Expand_Attrs(expand_state, val.attrs, AttrStage::Post,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, crate,  val); });
        }
        for(auto it = node.m_values.begin(); it != node.m_values.end(); )
        {
            if( it->value )
                ++it;
            else
                it = node.m_values.erase(it);
        }
    }
    void visit(::AST::ExprNode_StructLiteralPattern& node) override {
        for(auto& val : node.m_values)
        {
            Expand_Attrs_CfgAttr(val.attrs);
            Expand_Attrs(expand_state, val.attrs, AttrStage::Pre ,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, crate,  val); });
            if( !val.value )
                continue ;
            this->visit_nodelete(node, val.value);
            Expand_Attrs(expand_state, val.attrs, AttrStage::Post,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, crate,  val); });
        }
        for(auto it = node.m_values.begin(); it != node.m_values.end(); )
        {
            if( it->value )
                ++it;
            else
                it = node.m_values.erase(it);
        }
    }
    void visit(::AST::ExprNode_Array& node) override {
        this->visit_nodelete(node, node.m_size);
        this->visit_vector(node.m_values);
    }
    void visit(::AST::ExprNode_Tuple& node) override {
        this->visit_vector(node.m_values);
    }
    void visit(::AST::ExprNode_NamedValue& node) override {
        Expand_Path(this->expand_state, this->cur_mod(),  node.m_path);
    }
    void visit(::AST::ExprNode_Field& node) override {
        this->visit_nodelete(node, node.m_obj);
    }
    void visit(::AST::ExprNode_Index& node) override {
        this->visit_nodelete(node, node.m_obj);
        this->visit_nodelete(node, node.m_idx);
    }
    void visit(::AST::ExprNode_Deref& node) override {
        this->visit_nodelete(node, node.m_value);
    }
    void visit(::AST::ExprNode_Cast& node) override {
        this->visit_nodelete(node, node.m_value);
        Expand_Type(this->expand_state, this->cur_mod(),  node.m_type);
    }
    void visit(::AST::ExprNode_TypeAnnotation& node) override {
        this->visit_nodelete(node, node.m_value);
        Expand_Type(this->expand_state, this->cur_mod(),  node.m_type);
    }
    void visit(::AST::ExprNode_BinOp& node) override {
        this->visit_nodelete(node, node.m_left);
        this->visit_nodelete(node, node.m_right);

        if( this->in_assign_lhs ) {
            return;
        }
        static const RcString rcstring_start = RcString::new_interned("start");
        static const RcString rcstring_end   = RcString::new_interned("end");
        switch(node.m_type)
        {
        case ::AST::ExprNode_BinOp::RANGE: {
            // NOTE: Not language items pre 1.39
            auto core_crate = crate.m_ext_cratename_core;
            auto path_Range     = get_path(core_crate, "ops", "Range"    );
            auto path_RangeFrom = get_path(core_crate, "ops", "RangeFrom");
            auto path_RangeTo   = get_path(core_crate, "ops", "RangeTo"  );
            auto path_RangeFull = get_path(core_crate, "ops", "RangeFull");

            ::AST::ExprNode_StructLiteral::t_values values;
            if( node.m_left && node.m_right )
            {
                values.push_back({ {}, rcstring_start, mv$(node.m_left ) });
                values.push_back({ {}, rcstring_end  , mv$(node.m_right) });
                replacement.reset( new ::AST::ExprNode_StructLiteral(mv$(path_Range), nullptr, mv$(values)) );
            }
            else if( node.m_left )
            {
                values.push_back({ {}, rcstring_start, mv$(node.m_left ) });
                replacement.reset( new ::AST::ExprNode_StructLiteral(mv$(path_RangeFrom), nullptr, mv$(values)) );
            }
            else if( node.m_right )
            {
                values.push_back({ {}, rcstring_end  , mv$(node.m_right) });
                replacement.reset( new ::AST::ExprNode_StructLiteral(mv$(path_RangeTo), nullptr, mv$(values)) );
            }
            else
            {
                replacement.reset( new ::AST::ExprNode_StructLiteral(mv$(path_RangeFull), nullptr, mv$(values)) );
            }
            replacement->set_span( node.span() );
            break; }
        case ::AST::ExprNode_BinOp::RANGE_INC: {
            // NOTE: Not language items pre 1.54
            auto core_crate = crate.m_ext_cratename_core;
            auto path_None = get_path(core_crate, "option", "Option", "None");
            auto path_RangeInclusive_NonEmpty = get_path(core_crate, "ops", "RangeInclusive"  );
            auto path_RangeToInclusive        = get_path(core_crate, "ops", "RangeToInclusive");

            if( node.m_left )
            {
                ::AST::ExprNode_StructLiteral::t_values values;
                values.push_back({ {}, rcstring_start, mv$(node.m_left)  });
                values.push_back({ {}, rcstring_end  , mv$(node.m_right) });
                switch(gTargetVersion)
                {
                case TargetVersion::Rustc1_19:
                    break;
                case TargetVersion::Rustc1_29:
                case TargetVersion::Rustc1_39:
                    values.push_back({ {}, RcString::new_interned("is_empty"), ::AST::ExprNodeP(new ::AST::ExprNode_NamedValue(mv$(path_None))) });
                    break;
                case TargetVersion::Rustc1_54:
                case TargetVersion::Rustc1_74:
                    values.push_back({ {}, RcString::new_interned("exhausted"), ::AST::ExprNodeP(new ::AST::ExprNode_Bool(false)) });
                    break;
                }
                replacement.reset( new ::AST::ExprNode_StructLiteral(mv$(path_RangeInclusive_NonEmpty), nullptr, mv$(values)) );
            }
            else
            {
                ::AST::ExprNode_StructLiteral::t_values values;
                values.push_back({ {}, rcstring_end,  mv$(node.m_right) });
                replacement.reset( new ::AST::ExprNode_StructLiteral(mv$(path_RangeToInclusive), nullptr, mv$(values)) );
            }
            replacement->set_span( node.span() );
            break; }
        default:
            break;
        }
    }
    void visit(::AST::ExprNode_UniOp& node) override {
        this->visit_nodelete(node, node.m_value);
        // - Desugar question mark operator before resolve so it can create names
        if( node.m_type == ::AST::ExprNode_UniOp::QMARK ) {
            auto core_crate = crate.m_ext_cratename_core;

            // TODO: Find a way of creating bindings during HIR lower instead (so lang items are available)

            //auto it = crate.m_lang_items.find("try");
            //ASSERT_BUG(node.span(), it != crate.m_lang_items.end(), "Can't find the `try` lang item");
            //auto path_Try = it->second;
            auto path_Try = get_path(core_crate, "ops", "Try");
            static const RcString rcstring_v = RcString::new_interned("v");
            static const RcString rcstring_r = RcString::new_interned("r");
            if(TARGETVER_MOST_1_39)
            {
                auto path_Ok   = get_path(core_crate, "result", "Result", "Ok" );
                auto path_Err  = get_path(core_crate, "result", "Result", "Err");
                auto path_From = get_path(core_crate, "convert", "From");
                path_From.nodes().back().args().m_entries.push_back( ::TypeRef(node.span()) );

                auto path_Try_into_result = ::AST::Path::new_ufcs_trait(::TypeRef(node.span()), path_Try, { ::AST::PathNode(RcString::new_interned("into_result")) });
                auto path_Try_from_error  = ::AST::Path::new_ufcs_trait(::TypeRef(node.span()), path_Try, { ::AST::PathNode(RcString::new_interned("from_error" )) });

                // Desugars into
                // ```
                // match `Try::into_result(m_value)` {
                // Ok(v) => v,
                // Err(e) => return Try::from_error(From::from(e)),
                // }
                // ```

                ::std::vector< ::AST::ExprNode_Match_Arm>   arms;
                // `Ok(v) => v,`
                arms.push_back(::AST::ExprNode_Match_Arm(
                    ::make_vec1( ::AST::Pattern(::AST::Pattern::TagNamedTuple(), node.span(), path_Ok, ::make_vec1( ::AST::Pattern(::AST::Pattern::TagBind(), node.span(), rcstring_v) )) ),
                    {},
                    ::AST::ExprNodeP( new ::AST::ExprNode_NamedValue( ::AST::Path(rcstring_v) ) )
                    ));
                // `Err(e) => return Try::from_error(From::from(e)),`
                arms.push_back(::AST::ExprNode_Match_Arm(
                    ::make_vec1( ::AST::Pattern(::AST::Pattern::TagNamedTuple(), node.span(), path_Err, ::make_vec1( ::AST::Pattern(::AST::Pattern::TagBind(), node.span(), rcstring_r) )) ),
                    {},
                    ::AST::ExprNodeP(new ::AST::ExprNode_Flow(
                        (m_try_stack.empty() ? ::AST::ExprNode_Flow::RETURN : ::AST::ExprNode_Flow::BREAK),   // NOTE: uses `break 'tryblock` instead of `return` if in a try block.
                        (m_try_stack.empty() ? RcString("") : m_try_stack.back()),
                        ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                            ::AST::Path(path_Try_from_error),
                            ::make_vec1(
                                ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                                    ::AST::Path::new_ufcs_trait(::TypeRef(node.span()), mv$(path_From), { ::AST::PathNode(RcString::new_interned("from")) }),
                                    ::make_vec1( ::AST::ExprNodeP( new ::AST::ExprNode_NamedValue( ::AST::Path(rcstring_r) ) ) )
                                    ))
                                )
                            ))
                        ))
                    ));

                replacement.reset(new ::AST::ExprNode_Match(
                    ::AST::ExprNodeP(new AST::ExprNode_CallPath(
                        mv$(path_Try_into_result),
                        ::make_vec1( mv$(node.m_value) )
                        )),
                    mv$(arms)
                    ));
            }
            else  // 1.54+ - TryV2
            {
                auto path_Try_branch = ::AST::Path::new_ufcs_trait(::TypeRef(node.span()), path_Try, { ::AST::PathNode(RcString::new_interned("branch")) });
                // Not a lang item
                auto path_ControlFlow_Continue = get_path(core_crate, "ops", "ControlFlow", "Continue");
                auto path_ControlFlow_Break    = get_path(core_crate, "ops", "ControlFlow", "Break"   );
                auto path_FromResidual_from_residual = get_path(core_crate, "ops", "FromResidual", "from_residual");

                ::std::vector< ::AST::ExprNode_Match_Arm>   arms;
                // `Continue(v) => v,`
                arms.push_back(::AST::ExprNode_Match_Arm(
                    ::make_vec1( ::AST::Pattern(::AST::Pattern::TagNamedTuple(), node.span(), path_ControlFlow_Continue, ::make_vec1( ::AST::Pattern(::AST::Pattern::TagBind(), node.span(), rcstring_v) )) ),
                    {},
                    ::AST::ExprNodeP( new ::AST::ExprNode_NamedValue( ::AST::Path(rcstring_v) ) )
                    ));
                // `Break(r) => return R::from_residual(r),`
                arms.push_back(::AST::ExprNode_Match_Arm(
                    ::make_vec1( ::AST::Pattern(::AST::Pattern::TagNamedTuple(), node.span(), path_ControlFlow_Break, ::make_vec1( ::AST::Pattern(::AST::Pattern::TagBind(), node.span(), rcstring_r) )) ),
                    {},
                    ::AST::ExprNodeP(new ::AST::ExprNode_Flow(
                        (m_try_stack.empty() ? ::AST::ExprNode_Flow::RETURN : ::AST::ExprNode_Flow::BREAK),   // NOTE: uses `break 'tryblock` instead of return if in a try block.
                        (m_try_stack.empty() ? RcString("") : m_try_stack.back()),
                        ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                            ::AST::Path(path_FromResidual_from_residual),
                            ::make_vec1(::AST::ExprNodeP( new ::AST::ExprNode_NamedValue( ::AST::Path(rcstring_r) ) ))
                            ))
                        ))
                    ));

                replacement.reset(new ::AST::ExprNode_Match(
                    ::AST::ExprNodeP(new AST::ExprNode_CallPath(
                        mv$(path_Try_branch),
                        ::make_vec1( mv$(node.m_value) )
                        )),
                    mv$(arms)
                    ));
            }
        }
    }
};

void Expand_Expr(const ExpandState& es, ::AST::ExprNodeP& node)
{
    TRACE_FUNCTION_F("unique_ptr");
    CExpandExpr visitor { es };
    visitor.visit(node);
}
void Expand_Expr(const ExpandState& es, ::std::shared_ptr<AST::ExprNode>& node)
{
    TRACE_FUNCTION_F("shared_ptr");
    CExpandExpr visitor { es };
    node->visit(visitor);
    if( visitor.replacement ) {
        node.reset( visitor.replacement.release() );
    }
}
void Expand_Expr(const ExpandState& es, AST::Expr& node)
{
    TRACE_FUNCTION_F("AST::Expr");
    CExpandExpr visitor { es };
    node.visit_nodes(visitor);
    if( visitor.replacement ) {
        node = AST::Expr( mv$(visitor.replacement) );
    }
}

void Expand_GenericParams(const ExpandState& es, ::AST::Module& mod,  ::AST::GenericParams& params)
{
    for(auto& param_def : params.m_params)
    {
        TU_MATCH_HDRA( (param_def), {)
        TU_ARMA(None, e) {
            // Ignore
            }
        TU_ARMA(Lifetime, e) {
            }
        TU_ARMA(Type, ty_def) {
            Expand_Type(es, mod,  ty_def.get_default());
            }
        TU_ARMA(Value, val_def) {
            Expand_Type(es, mod,  val_def.type());
            }
        }
    }
    for(auto& bound : params.m_bounds)
    {
        TU_MATCHA( (bound), (be),
        (None,
            ),
        (Lifetime,
            ),
        (TypeLifetime,
            Expand_Type(es, mod,  be.type);
            ),
        (IsTrait,
            Expand_Type(es, mod,  be.type);
            Expand_Path(es, mod,  be.trait);
            ),
        (MaybeTrait,
            Expand_Type(es, mod,  be.type);
            Expand_Path(es, mod,  be.trait);
            ),
        (NotTrait,
            Expand_Type(es, mod,  be.type);
            Expand_Path(es, mod,  be.trait);
            ),
        (Equality,
            Expand_Type(es, mod,  be.type);
            Expand_Type(es, mod,  be.replacement);
            )
        )
    }
}

void Expand_BareExpr(const ::AST::Crate& crate, const AST::Module& mod, ::AST::ExprNodeP& node)
{
    ExpandState es { const_cast<AST::Crate&>(crate), LList<const AST::Module*>(nullptr, &mod), ExpandMode::FirstPass };
    Expand_Expr(es, node);
    es.mode = ExpandMode::Final;
    //Expand_Expr(es, node);
}
::AST::ExprNodeP Expand_ParseAndExpand_ExprVal(const ::AST::Crate& crate, const AST::Module& mod, TokenStream& lex)
{
    auto sp = lex.point_span();
    auto n = Parse_ExprVal(lex);
    ASSERT_BUG(sp, n, "No expression returned");
    Expand_BareExpr(crate, mod, n);
    return n;
}

void Expand_Function(const ExpandState& es, AST::Module& mod, AST::Function& e)
{
    for(size_t i = 0; i < e.args().size(); i ++)
    {
        auto& arg = e.args()[i];
        if( !Expand_Attrs_CfgOnly(es, arg.attrs) ) {
            e.args().erase(e.args().begin() + i);
            i --;
            continue ;
        }
        Expand_Pattern(es, mod,  arg.pat, false);
        Expand_Type(es, mod,  arg.ty);
        Expand_Attrs(es, arg.attrs, AttrStage::Post, [&](const Span& sp, const ExpandDecorator& d, const AST::Attribute& a) {
            TODO(sp, "attributes on function arguments - " << a);
            });
    }
    Expand_Type(es, mod,  e.rettype());
    Expand_Expr(es, e.code());
}

void Expand_Impl(const ExpandState& es, ::AST::Path modpath, ::AST::Module& mod, ::AST::Impl& impl)
{
    TRACE_FUNCTION_F(impl.def());
    Expand_Attrs_CfgAttr(impl.def().attrs());
    Expand_Attrs(es, impl.def().attrs(), AttrStage::Pre,  mod, impl.def());
    if( impl.def().type().is_wildcard() ) {
        DEBUG("Deleted");
        return ;
    }
    Expand_GenericParams(es, mod,  impl.def().params());

    Expand_Type(es, mod,  impl.def().type());
    Expand_Path(es, mod,  impl.def().trait().ent);

    DEBUG("> Items");
    for( unsigned int idx = 0; idx < impl.items().size(); idx ++ )
    {
        auto i = std::move(impl.items()[idx]);
        DEBUG("  - " << i.name << " :: " << i.attrs);

        // TODO: Make a path from the impl definition? Requires having the impl def resolved to be correct
        // - Does it? the namespace is essentially the same. There may be issues with wherever the path is used though
        // TODO: UFCS path, or different method
        AST::AbsolutePath   path("", {"", i.name});

        auto attrs = mv$(i.attrs);
        Expand_Attrs_CfgAttr(attrs);
        Expand_Attrs(es, attrs, AttrStage::Pre,  mod, impl, i.vis, i.name, *i.data);

        TU_MATCH_HDRA( (*i.data), {)
        default:
            BUG(Span(), "Unknown item type in impl block - " << i.data->tag_str());
        TU_ARMA(None, e) {
            }
        TU_ARMA(MacroInv, e) {
            if( e.path().is_valid() )
            {
                TRACE_FUNCTION_F("Macro invoke " << e.path());
                // Move out of the module to avoid invalidation if a new macro invocation is added
                auto mi_owned = mv$(e);

                auto ttl = Expand_Macro(es, mod, mi_owned);
                ASSERT_BUG(mi_owned.span(), ttl, "TODO: Unexpanded macro?");

                if( ttl )
                {
                    // Re-parse tt
                    while( ttl->lookahead(0) != TOK_EOF )
                    {
                        Parse_Impl_Item(*ttl, impl);
                    }
                    // - Any new macro invocations ends up at the end of the list and handled
                    ASSERT_BUG(mi_owned.span(), impl.items().size() > idx, "");

                    *i.data = AST::Item::make_None({});
                }
                else {
                    // Move back in (using the index, as the old pointr may be invalid)
                    i.data->as_MacroInv() = mv$(mi_owned);
                }
            }
            }
        TU_ARMA(Function, e) {
            TRACE_FUNCTION_F("fn " << i.name);
            Expand_Function(es, mod, e);
            }
        TU_ARMA(Static, e) {
            TRACE_FUNCTION_F("static " << i.name);
            Expand_Expr(es, e.value());
            Expand_Type(es, mod,  e.type());
            }
        TU_ARMA(Type, e) {
            TRACE_FUNCTION_F("type " << i.name);
            Expand_Type(es, mod,  e.type());
            }
        }
        impl.items()[idx] = std::move(i);

        // Run post-expansion decorators and restore attributes
        {
            auto& i = impl.items()[idx];
            Expand_Attrs(es, attrs, AttrStage::Post,  mod, impl, i.vis, i.name, *i.data);
            // TODO: How would this be populated? It got moved out?
            if( i.attrs.m_items.size() == 0 )
                i.attrs = mv$(attrs);
        }
    }

    Expand_Attrs(es, impl.def().attrs(), AttrStage::Post, mod, impl.def());
}
void Expand_ImplDef(const ExpandState& es, ::AST::Path modpath, ::AST::Module& mod, ::AST::ImplDef& impl_def)
{
    Expand_Attrs_CfgAttr(impl_def.attrs());
    Expand_Attrs(es, impl_def.attrs(), AttrStage::Pre,  mod, impl_def);
    if( impl_def.type().is_wildcard() ) {
        DEBUG("Deleted");
        return ;
    }
    Expand_GenericParams(es, mod,  impl_def.params());

    Expand_Type(es, mod,  impl_def.type());
    //Expand_Type(es, mod,  impl_def.trait());

    Expand_Attrs(es, impl_def.attrs(), AttrStage::Post,  mod, impl_def);
}

//void Expand_Function(

void Expand_Mod(const ExpandState& es, ::AST::AbsolutePath modpath, ::AST::Module& mod, unsigned int first_item)
{
    TRACE_FUNCTION_F("modpath = " << modpath << ", first_item=" << first_item);

    // TODO: Pre-parse all macro_rules invocations into items?

    if( es.mode == ExpandMode::FirstPass )
    {
        // Import all macros from parent module.
        if( first_item == 0 )
        {
            if( es.modstack.m_prev )
            {
                for(const auto& mac : es.modstack.m_prev->m_item->m_macro_imports) {
                    mod.m_macro_imports.push_back(mac.clone());
                }
                for(const auto& mac : es.modstack.m_prev->m_item->macros()) {
                    mod.m_macro_imports.push_back(AST::Module::MacroImport { false, mac.name, es.modstack.m_prev->m_item->path() + mac.name, &*mac.data });
                }
            }
            for( const auto& mi: mod.m_macro_imports ) {
                DEBUG("- Imports '" << mi.path << "'");
            }
        }

        // Insert prelude if: Enabled for this module, present for the crate, and this module is not an anon
        if( es.crate.m_prelude_path != AST::Path() )
        {
            if( mod.m_insert_prelude && ! mod.is_anon() ) {
                DEBUG("> Adding custom prelude " << es.crate.m_prelude_path);
                mod.add_item(Span(), AST::Visibility::make_restricted(AST::Visibility::Ty::Private, mod.path()), "", ::AST::UseItem { Span(), ::make_vec1(::AST::UseItem::Ent { Span(), es.crate.m_prelude_path, "" }) }, {});
            }
            else {
                DEBUG("> Not inserting custom prelude (anon or disabled)");
            }
        }
    }

    // Stack to prevent macro recursion
    // - Items are popped if the item address matches
    std::vector<const AST::Named<AST::Item>*>   macro_recursion_stack;

    DEBUG("Items");
    for( unsigned int idx = first_item; idx < mod.m_items.size(); idx ++ )
    {
        auto& i = *mod.m_items[idx];

        // If this is the pop point for this entry, then pop
        // - Note, can be `nullptr`, but that indicates that the macro invocation was the end
        while( !macro_recursion_stack.empty() && macro_recursion_stack.back() == &i ) {
            macro_recursion_stack.pop_back();
            DEBUG("End macro recursion guard");
        }

        DEBUG("- " << modpath << "::" << i.name << " (" << ::AST::Item::tag_to_str(i.data.tag()) << ") :: " << i.attrs);
        auto path = modpath + i.name;

        if(const auto* mi = i.data.opt_MacroInv() )
        {
            if( mi->path().is_trivial() && mi->path().as_trivial() == "macro_rules" ) {
                i.vis = AST::Visibility::make_global();
                DEBUG("macro_rules made pub");
            }
        }

        // Pre-exapand inner `#[cfg]`
        {
            struct H {
                static void filter_cfg(::std::vector<AST::StructItem>& lst) {
                    auto new_end = ::std::remove_if(lst.begin(), lst.end(), [&](const AST::StructItem& v) { return !check_cfg_attrs(v.m_attrs); });
                    DEBUG(lst.size() << " -> " << new_end - lst.begin());
                    lst.erase(new_end, lst.end());
                }
                static void filter_cfg(::std::vector<AST::TupleItem>& lst) {
                    auto new_end = ::std::remove_if(lst.begin(), lst.end(), [&](const AST::TupleItem& v) { return !check_cfg_attrs(v.m_attrs); });
                    DEBUG(lst.size() << " -> " << new_end - lst.begin());
                    lst.erase(new_end, lst.end());
                }
            };
            DEBUG(i.data.tag_str() << " " << mod.path() << "::" << i.name);
            TU_MATCH_HDRA( (i.data), { )
            // Expand cfg within types, so derive macros don't need to care
            TU_ARMA(Struct, str) {
                TU_MATCH_HDRA((str.m_data), {)
                TU_ARMA(Unit, e) {
                    }
                TU_ARMA(Struct, e) {
                    H::filter_cfg(e.ents);
                    }
                TU_ARMA(Tuple, e) {
                    H::filter_cfg(e.ents);
                    }
                }
                }
            TU_ARMA(Union, unm) {
                H::filter_cfg(unm.m_variants);
                }
            TU_ARMA(Enum, enm) {
                for(auto it = enm.variants().begin(); it != enm.variants().end(); )
                {
                    if( !check_cfg_attrs(it->m_attrs) ) {
                        it = enm.variants().erase(it);
                    }
                    else {
                        TU_MATCH_HDRA( (it->m_data), { )
                        TU_ARMA(Value, e) {}
                        TU_ARMA(Tuple, e) {
                            H::filter_cfg(e.m_items);
                            }
                        TU_ARMA(Struct, e) {
                            H::filter_cfg(e.m_fields);
                            }
                        }

                        ++ it;
                    }
                }
                }
            default:
                break;
            }
        }

        auto attrs = mv$(i.attrs);
        auto vis = i.vis;
        if( es.mode == ExpandMode::FirstPass )
        {
            Expand_Attrs_CfgAttr(attrs);
            Expand_Attrs(es, attrs, AttrStage::Pre,  path, mod, vis, i.data);
        }

        // Do modules without moving the definition (so the module path is always valid)
        if( i.data.is_Module() )
        {
            auto& e = i.data.as_Module();
            LList<const AST::Module*>   sub_modstack(&es.modstack, &e);
            ExpandState es_inner(es.crate, sub_modstack, es.mode);
            Expand_Mod(es_inner, path, e, 0);
            Expand_Attrs(es, attrs, AttrStage::Post,  path, mod, vis, i.data);
            es.change |= es_inner.change;
            es.has_missing |= es_inner.has_missing;
            i.attrs = std::move(attrs);
            continue ;
        }

        auto dat = mv$(i.data);

        TU_MATCH_HDRA( (dat), {)
        TU_ARMA(None, e) {
            // Skip: nothing
            }
        TU_ARMA(GlobalAsm, e) {
            // Skip: Nothing to expand
            }
        TU_ARMA(MacroInv, e) {
            // Move out of the module to avoid invalidation if a new macro invocation is added

            if( macro_recursion_stack.size() > MAX_MACRO_RECURSION ) {
                ERROR(i.span, E0000, "Exceeded macro recusion limit of " << MAX_MACRO_RECURSION);
            }
            auto mi_owned = mv$(e);

            if( !mi_owned.is_expanded() )
            {
                assert(mi_owned.span());
                TRACE_FUNCTION_F("Macro invoke " << mi_owned.path());

                auto ttl = Expand_Macro(es, mod, mi_owned);
                if( ttl )
                {
                    Expand_Attrs(es, attrs, AttrStage::Post,  path, mod, vis, dat);

                    // Re-parse tt
                    // TODO: All new items should be placed just after this?
                    DEBUG("-- Parsing as mod items");
                    // Move the item list out
                    auto old_items = std::move(mod.m_items);
                    // Parse module items
                    Parse_ModRoot_Items(*ttl, mod);
                    auto new_item_count = mod.m_items.size();
                    // Then insert the newly created items
                    old_items.insert(old_items.begin() + idx + 1, std::make_move_iterator(mod.m_items.begin()), std::make_move_iterator(mod.m_items.end()));
                    // and move the (updated) item list back in
                    mod.m_items = std::move(old_items);

                    auto next_non_macro_item = idx + 1 + new_item_count;
                    macro_recursion_stack.push_back(next_non_macro_item == mod.m_items.size() ? nullptr : &*mod.m_items[next_non_macro_item]);

                    mi_owned.set_expanded();
                }
                else {
                    DEBUG("Deferred macro");
                }
            }
            dat.as_MacroInv() = mv$(mi_owned);
            }
        TU_ARMA(Macro, e) {
            ASSERT_BUG(i.span, e, "Null macro - " << i.name);
            mod.add_macro(i.vis.is_global(), i.name, mv$(e));
            dat = AST::Item::make_None({});
            }
        TU_ARMA(Use, e) {
            // Determine if the `use` refers to a macro, and import into the current scope
            for(const auto& ue : e.entries)
            {
                // Get module ref, if it's to a HIR module then grab the macro
                if(ue.name != "" && ue.path.nodes().size() >= 1)
                {
                    DEBUG("Use " << ue.path);

                    AST::AbsolutePath   ref_path;
                    auto m = Resolve_Lookup_Macro(ue.sp, es.crate, mod.path(), ue.path, /*out_path=*/&ref_path);
                    MacroRef   ref;
                    TU_MATCH_HDRA( (m), { )
                    TU_ARMA(None, e) {
                        // Not found? Ignore.
                        }
                    TU_ARMA(InternalMacro, e) {
                        // Ignore builtins, they're always available.
                        }
                    TU_ARMA(ProcMacro, pm) {
                        ref = pm;
                        }
                    TU_ARMA(MacroRules, mr) {
                        ref = mr;
                        }
                    }
                    if( ! ref.is_None() ) {
                        DEBUG("+ Macro Import: " << ref_path);
                        mod.m_macro_imports.push_back(AST::Module::MacroImport { false, ue.name, std::move(ref_path), std::move(ref) });
                    }
                }
            }
            }
        TU_ARMA(ExternBlock, e) {
            // TODO: Run expand on inner items?
            // HACK: Just convert inner items into outer items
            auto items = mv$( e.items() );
            for(auto& i2 : items)
            {
                mod.m_items.push_back( box$(i2) );
            }
            }
        TU_ARMA(Impl, e) {
            Expand_Impl(es, modpath, mod,  e);
            if( e.def().type().is_wildcard() ) {
                dat = AST::Item();
            }
            }
        TU_ARMA(NegImpl, e) {
            Expand_ImplDef(es, modpath, mod,  e);
            if( e.type().is_wildcard() ) {
                dat = AST::Item();
            }
            }
        TU_ARMA(Module, e) {
            throw "";
            }
        TU_ARMA(Crate, e) {
            if( e.name != "" )
            {
                // Can't recurse into an `extern crate`
                if(es.crate.m_extern_crates.count(e.name) == 0)
                {
                    e.name = es.crate.load_extern_crate( i.span, e.name );
                }
                // Crates imported in root are added to the implicit list
                if( modpath.nodes.empty() )
                {
                    AST::g_implicit_crates.insert( std::make_pair(i.name, e.name) );
                }
            }
            else {
                if( modpath.nodes.empty() )
                {
                    AST::g_implicit_crates.insert( std::make_pair(i.name, "") );
                }
            }
            }

        TU_ARMA(Struct, e) {
            Expand_GenericParams(es, mod,  e.params());
            TU_MATCH_HDRA( (e.m_data), {)
            TU_ARMA(Unit, sd) {
                }
            TU_ARMA(Struct, sd) {
                for(auto it = sd.ents.begin(); it != sd.ents.end(); ) {
                    auto& si = *it;
                    Expand_Attrs_CfgAttr(si.m_attrs);
                    Expand_Attrs(es, si.m_attrs, AttrStage::Pre, [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, si); });
                    Expand_Type(es, mod,  si.m_type);
                    Expand_Attrs(es, si.m_attrs, AttrStage::Post, [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, si); });

                    if( si.m_name == "" )
                        it = sd.ents.erase(it);
                    else
                        ++it;
                }
                }
            TU_ARMA(Tuple, sd) {
                for(auto it = sd.ents.begin(); it != sd.ents.end(); ) {
                    auto& si = *it;
                    Expand_Attrs_CfgAttr(si.m_attrs);
                    Expand_Attrs(es, si.m_attrs, AttrStage::Pre, [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, si); });
                    Expand_Type(es, mod,  si.m_type);
                    Expand_Attrs(es, si.m_attrs, AttrStage::Post, [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, si); });

                    if( ! si.m_type.is_valid() )
                        it = sd.ents.erase(it);
                    else
                        ++it;
                }
                }
            }
            }
        TU_ARMA(Enum, e) {
            Expand_GenericParams(es, mod,  e.params());
            for(auto& var : e.variants()) {
                Expand_Attrs_CfgAttr(var.m_attrs);
                Expand_Attrs(es, var.m_attrs, AttrStage::Pre,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, var); });
                TU_MATCH_HDRA( (var.m_data), {)
                TU_ARMA(Value, e) {
                    Expand_Expr(es,  e.m_value);
                    }
                TU_ARMA(Tuple, e) {
                    for(auto it = e.m_items.begin(); it != e.m_items.end(); ) {
                        auto& si = *it;
                        Expand_Attrs_CfgAttr(si.m_attrs);
                        Expand_Attrs(es, si.m_attrs, AttrStage::Pre, [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, si); });
                        Expand_Type(es, mod,  si.m_type);
                        Expand_Attrs(es, si.m_attrs, AttrStage::Pre, [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, si); });
                        if( ! si.m_type.is_valid() )
                            it = e.m_items.erase(it);
                        else
                            ++it;
                    }
                    }
                TU_ARMA(Struct, e) {
                    for(auto it = e.m_fields.begin(); it != e.m_fields.end(); ) {
                        auto& si = *it;
                        Expand_Attrs_CfgAttr(si.m_attrs);
                        Expand_Attrs(es, si.m_attrs, AttrStage::Pre, [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, si); });
                        Expand_Type(es, mod,  si.m_type);
                        Expand_Attrs(es, si.m_attrs, AttrStage::Post, [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, si); });

                        if( si.m_name == "" )
                            it = e.m_fields.erase(it);
                        else
                            ++it;
                    }
                    }
                }
                Expand_Attrs(es, var.m_attrs, AttrStage::Post,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, var); });
            }
            // Handle cfg on variants (kinda hacky)
            for(auto it = e.variants().begin(); it != e.variants().end(); ) {
                if( it->m_name == "" ) {
                    it = e.variants().erase(it);
                }
                else {
                    ++ it;
                }
            }
            }
        TU_ARMA(Union, e) {
            Expand_GenericParams(es, mod,  e.m_params);
            for(auto it = e.m_variants.begin(); it != e.m_variants.end(); )
            {
                auto& si = *it;
                Expand_Attrs_CfgAttr(si.m_attrs);
                Expand_Attrs(es, si.m_attrs, AttrStage::Pre, [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, si); });
                Expand_Type(es, mod,  si.m_type);
                Expand_Attrs(es, si.m_attrs, AttrStage::Post, [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, es.crate, si); });

                if( si.m_name == "" )
                    it = e.m_variants.erase(it);
                else
                    ++it;
            }
            }
        TU_ARMA(Trait, e) {
            Expand_GenericParams(es, mod,  e.params());
            for(auto& p : e.supertraits())
                Expand_Path(es, mod, *p.ent.path);
            auto& trait_items = e.items();
            for(size_t idx = 0; idx < trait_items.size(); idx ++)
            {
                auto& ti = trait_items[idx];
                DEBUG(" - " << ti.name << " " << ti.data.tag_str());
                auto attrs = mv$(ti.attrs);
                auto ti_path = path + ti.name;
                Expand_Attrs_CfgAttr(attrs);
                Expand_Attrs(es, attrs, AttrStage::Pre,  ti_path, mod, e, ti.data);

                TU_MATCH_HDRA( (ti.data), {)
                default:
                    BUG(Span(), "Unknown item type in trait block - " << ti.data.tag_str());
                TU_ARMA(None, e) {}
                TU_ARMA(MacroInv, e) {
                    if( e.path().is_valid() )
                    {
                        TRACE_FUNCTION_F("Macro invoke " << e.path());
                        // Move out of the module to avoid invalidation if a new macro invocation is added
                        auto mi_owned = mv$(e);

                        auto ttl = Expand_Macro(es, mod, mi_owned);
                        ASSERT_BUG(mi_owned.span(), ttl, "TODO: Unexpanded macro");

                        if( ttl.get() )
                        {
                            // Re-parse tt
                            size_t insert_pos = idx+1;
                            while( ttl->lookahead(0) != TOK_EOF )
                            {
                                auto i = Parse_Trait_Item(*ttl);
                                trait_items.insert( trait_items.begin() + insert_pos, mv$(i) );
                                insert_pos ++;
                            }
                            // - Any new macro invocations ends up at the end of the list and handled
                            trait_items[idx].data = AST::Item::make_None({});
                        }
                        else
                        {
                            // Move back in (using the index, as the old pointer may be invalid)
                            trait_items[idx].data.as_MacroInv() = mv$(mi_owned);
                        }
                    }
                    }
                TU_ARMA(Function, e) {
                    Expand_Function(es, mod, e);
                    }
                TU_ARMA(Static, e) {
                    Expand_Expr(es, e.value());
                    Expand_Type(es, mod,  e.type());
                    }
                TU_ARMA(Type, e) {
                    Expand_Type(es, mod,  e.type());
                    }
                }

                {
                    auto& ti = trait_items[idx];

                    Expand_Attrs(es, attrs, AttrStage::Post,  ti_path, mod, e, ti.data);
                    if( ti.attrs.m_items.size() == 0 )
                        ti.attrs = mv$(attrs);
                }
            }
            }
        TU_ARMA(Type, e) {
            Expand_Type(es, mod,  e.type());
            }

        TU_ARMA(Function, e) {
            Expand_Function(es, mod, e);
            }
        TU_ARMA(Static, e) {
            Expand_Expr(es, e.value());
            Expand_Type(es, mod,  e.type());
            }
        TU_ARMA(TraitAlias, e) {
            for(auto& p : e.traits)
                Expand_Path(es, mod, *p.ent.path);
            }
        }
        Expand_Attrs(es, attrs, AttrStage::Post,  path, mod, vis, dat);

        {

            auto& i = *mod.m_items[idx];
            if( i.data.tag() == ::AST::Item::TAGDEAD ) {
                i.data = mv$(dat);
            }
            // TODO: When would this _not_ be empty?
            if( i.attrs.m_items.size() == 0 )
                i.attrs = mv$(attrs);
        }
    }

    // IGNORE m_anon_modules, handled as part of expressions

    //for( const auto& mi: mod.macro_imports_res() )
    //    DEBUG("- Imports '" << mi.name << "'");
}
void Expand_Mod_IndexAnon(::AST::Crate& crate, ::AST::Module& mod)
{
    TRACE_FUNCTION_F("mod=" << mod.path());

    for(auto& i : mod.m_items)
    {
        DEBUG("- " << i->data.tag_str() << " '" << i->name << "'");
        if(auto* e = i->data.opt_Module())
        {
            Expand_Mod_IndexAnon(crate, *e);

            // TODO: Also ensure that all #[macro_export] macros end up in parent
        }
    }

    for( auto& mp : mod.anon_mods() )
    {
        if( mp.unique() ) {
            DEBUG("- " << mp->path() << " dropped due to node destruction");
            mp.reset();
        }
        else {
            Expand_Mod_IndexAnon(crate, *mp);
        }
    }
}


//
// Expand all `cfg` attributes... mostly to find #[macro_export]
//
void Expand_Mod_Early(::AST::Crate& crate, ::AST::Module& mod, std::vector<std::unique_ptr<AST::Named<AST::Item>>>& new_root_items)
{
    TRACE_FUNCTION_F(mod.path());
    for(auto& i : mod.m_items)
    {
        if(const auto* mi = i->data.opt_MacroInv() )
        {
            if( mi->path().is_trivial() && mi->path().as_trivial() == "macro_rules" ) {
                i->vis = AST::Visibility::make_global();
                DEBUG("macro_rules made pub");
            }
        }

        Expand_Attrs_CfgAttr(i->attrs);
        bool is_macro_export = false;
        bool cfg_failed = false;
        for(auto& a : i->attrs.m_items)
        {
            if( a.name() == "cfg" ) {
                if( !check_cfg(i->span, a) ) {
                    cfg_failed = true;
                }
            }
            else if( a.name() == "macro_export" ) {
                is_macro_export = true;
            }
            else {
            }
        }
        if( cfg_failed ) {
            i->data = ::AST::Item::make_None({});
        }
        else if( is_macro_export ) {
            if( i->data.is_MacroInv() && i->data.as_MacroInv().path().is_trivial() && i->data.as_MacroInv().path().as_trivial() == "macro_rules" )
            {
                const auto& mac_inv = i->data.as_MacroInv();
                DEBUG("macro_rules marked with #[macro_export] moved to the crate root - " << mac_inv.input_ident());
                new_root_items.push_back(box$(*i));
                i->data = AST::Item();

#if 0
                TTStream    lex(i->span, ParseState(crate.m_edition), mac_inv.input_tt());
                auto mac = Parse_MacroRules(lex);
                const auto* mac_ptr = &*mac;
                crate.m_root_module.add_macro(true, mac_inv.input_ident(), std::move(mac));
                crate.m_exported_macros[mac_inv.input_ident()] = mac_ptr;
#else
#endif
            }
            else if( i->data.is_Macro() )
            {
                // TODO: `#[macro_export] macro foo { ... }` DOESN'T move the item to the root
                // - Instead, it should add an alias? Or just tag for export
                DEBUG("macro item export: " << i->name);
                i->data.as_Macro()->m_exported = true;
            }
            else
            {
                ERROR(i->span, E0000, "#[macro_export] on non-macro_rules - " << i->data.tag_str());
            }
        }
        else if(auto* e = i->data.opt_Module() ) {
            Expand_Mod_Early(crate, *e, new_root_items);
        }
        else {
        }
    }

    DEBUG("Items");
    for( unsigned int idx = 0; idx < mod.m_items.size(); idx ++ )
    {
        auto& i = *mod.m_items[idx];
        if(auto* mi = i.data.opt_MacroInv() )
        {
            // 1.74 HACK - Parse `macro_rules` during the first pass, so they're present for `use` to refer to
            if( mi->path().is_trivial() && mi->path().as_trivial() == "macro_rules" ) {
                auto mi_owned = mv$(*mi);

                TRACE_FUNCTION_F("Macro invoke " << mi_owned.path());

                ExpandState es { crate, {}, ExpandMode::Iterate };
                auto ttl = Expand_Macro(es, mod, mi_owned);
                ASSERT_BUG(mi_owned.span(), ttl, "BUG: macro_rules not expanded");
                assert( mi_owned.path().is_valid() );

                if( ttl.get() )
                {
                    // Re-parse tt
                    assert(ttl.get());
                    DEBUG("-- Parsing as mod items");
                    // Move the item list out
                    auto old_items = std::move(mod.m_items);
                    // Parse module items
                    Parse_ModRoot_Items(*ttl, mod);
                    //auto new_item_count = mod.m_items.size();
                    // Then insert the newly created items
                    old_items.insert(old_items.begin() + idx + 1, std::make_move_iterator(mod.m_items.begin()), std::make_move_iterator(mod.m_items.end()));
                    // and move the (updated) item list back in
                    mod.m_items = std::move(old_items);

                    //auto next_non_macro_item = idx + 1 + new_item_count;
                    //macro_recursion_stack.push_back(next_non_macro_item == mod.m_items.size() ? nullptr : &*mod.m_items[next_non_macro_item]);
                    //mod.m_items[idx]->data = AST::Item::make_None({});
                }
                else
                {
                }
                mod.m_items[idx]->data.as_MacroInv() = mv$(mi_owned);
            }
        }
    }
}

void Expand(::AST::Crate& crate)
{
    for(const auto& e : g_decorators)
    {
        DEBUG("Decorator: " << e.first);
    }
    for(const auto& e : g_macros)
    {
        DEBUG("Macro: " << e.first);
    }

    ExpandState es { crate, LList<const ::AST::Module*>(nullptr, &crate.m_root_module), ExpandMode::FirstPass };


    // 1. Crate attributes
    Expand_Attrs_CfgAttr(crate.m_attrs);
    Expand_Attrs(es, crate.m_attrs, AttrStage::Pre,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, crate); });

    // TODO: Crate name and type

    std::vector<std::unique_ptr<AST::Named<AST::Item>>> new_root_items;
    Expand_Mod_Early(crate, crate.m_root_module, new_root_items);
    crate.m_root_module.m_items.insert( crate.m_root_module.m_items.begin(),
        std::make_move_iterator(new_root_items.begin()), std::make_move_iterator(new_root_items.end()) );

    // Insert magic for libstd/libcore
    // NOTE: The actual crates are loaded in "LoadCrates" using magic in AST::Crate::load_externs
    RcString std_crate_shortname;
    RcString std_crate_name;
    switch( crate.m_load_std )
    {
    case ::AST::Crate::LOAD_STD:
        std_crate_shortname = RcString::new_interned("std");
        std_crate_name = crate.m_ext_cratename_std;
        break;
    case ::AST::Crate::LOAD_CORE:
        std_crate_shortname = RcString::new_interned("core");
        std_crate_name = crate.m_ext_cratename_core;
        break;
    case ::AST::Crate::LOAD_NONE:
        break;
    }
    if(std_crate_shortname != "")
    {
        ASSERT_BUG(Span(), std_crate_name != "", "`" << std_crate_shortname << "` not loaded?");
        if( crate.m_prelude_path == AST::Path() )
        {
            switch(crate.m_edition)
            {
            case ::AST::Edition::Rust2015:
            case ::AST::Edition::Rust2018:
                crate.m_prelude_path = AST::Path(std_crate_name, {AST::PathNode("prelude"), AST::PathNode("v1")});
                break;
            case ::AST::Edition::Rust2021:
                crate.m_prelude_path = AST::Path(std_crate_name, {AST::PathNode("prelude"), AST::PathNode("rust_2021")});
                break;
            }
        }
        AST::AttributeList  attrs;
        AST::AttributeName  name;
        name.elems.push_back("macro_use");
        attrs.push_back( AST::Attribute(Span(), mv$(name), {}) );
        // NOTE: For `macro_use` we want to import this first, but for item lookup, we want it to be last.
        // - Solution, add to the end - but pre-visit the attributes
        crate.m_root_module.m_items.push_back(
            box$( AST::Named<AST::Item>(Span(), mv$(attrs), AST::Visibility::make_restricted(AST::Visibility::Ty::Private, AST::AbsolutePath()), std_crate_shortname, AST::Item::make_Crate({std_crate_name}) ) )
            );
        auto& i = *crate.m_root_module.m_items.back();
        Expand_Attrs(es, i.attrs, AttrStage::Post, ::AST::AbsolutePath(), crate.m_root_module, i.vis, i.data);
    }

    // 2. Module attributes
    for( auto& a : crate.m_attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( a.name() == d.first && d.second->stage() == AttrStage::Pre ) {
                //d.second->handle(a, crate, ::AST::Path(), crate.m_root_module, crate.m_root_module);
            }
        }
    }

    // 3. Module tree
    // Loop until no more expansions happen
    // - Combine this with allowing macros to fail to expand, to be caught with a final pass
    Expand_Mod(es, ::AST::AbsolutePath(), crate.m_root_module);
    DEBUG("(first) es.change = " << es.change << ", es.has_missing=" << es.has_missing << " (" << &es << ")");
    if( es.has_missing )
    {
        for(size_t n_iters = 0; n_iters < 5 && es.change && es.has_missing; n_iters ++)
        {
            es.mode = ExpandMode::Iterate;
            es.change = false;
            es.has_missing = false;
            Expand_Mod(es, ::AST::AbsolutePath(), crate.m_root_module);
            DEBUG("?(Iter) es.change = " << es.change << ", es.has_missing=" << es.has_missing);
        }
        //ASSERT_BUG(Span(), !es.has_missing, "Expand too too many attempts");
        es.has_missing = false;
    }
    es.mode = ExpandMode::Final;
    Expand_Mod(es, ::AST::AbsolutePath(), crate.m_root_module);
    ASSERT_BUG(Span(), !es.has_missing, "Expand too too many attempts");

    //Expand_Attrs(es, crate.m_attrs, AttrStage::Post,  [&](const Span& sp, const auto& d, const auto& a){ d.handle(sp, a, crate); });

    // Post-process
    Expand_Mod_IndexAnon(crate, crate.m_root_module);

    // Extract exported macros

    {
        auto& exported_macros = crate.m_exported_macros;

        ::std::vector< ::AST::Module*>    mods;
        mods.push_back( &crate.m_root_module );
        do
        {
            auto& mod = *mods.back();
            mods.pop_back();

            for( /*const*/ auto& mac : mod.macros() ) {
                if( mac.data->m_exported ) {
                    auto res = exported_macros.insert( ::std::make_pair(mac.name,  &*mac.data) );
                    if( res.second )
                        DEBUG("- Define " << mac.name << "!");
                }
                else {
                    DEBUG("- Non-exported " << mac.name << "!");
                }
            }

            for(auto& i : mod.m_items) {
                if( i->data.is_Module() )
                    mods.push_back( &i->data.as_Module() );
            }
        } while( mods.size() > 0 );

        // - Exported macros imported by the root (is this needed?)
        // - Re-exported macros (ignore proc macros for now?)
        for( const auto& mac : crate.m_root_module.m_macro_imports )
        {
            if( mac.is_pub )
            {
                if( !mac.ref.is_MacroRules() ) {
                    continue ;
                }
                auto v = ::std::make_pair( mac.name, mac.ref.as_MacroRules() );

                auto it = exported_macros.find(mac.name);
                if( it == exported_macros.end() )
                {
                    auto res = exported_macros.insert( mv$(v) );
                    DEBUG("- Import " << mac.name << "! (from \"" << res.first->second->m_source_crate << "\")");
                }
                else if( v.second->m_rules.empty() ) {
                    // Skip
                }
                else {
                    DEBUG("- Replace " << mac.name << "! (from \"" << it->second->m_source_crate << "\") with one from \"" << v.second->m_source_crate << "\"");
                    it->second = mv$( v.second );
                }
            }
        }
    }
}


