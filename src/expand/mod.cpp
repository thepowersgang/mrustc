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

DecoratorDef*   g_decorators_list = nullptr;
MacroDef*   g_macros_list = nullptr;
::std::map< RcString, ::std::unique_ptr<ExpandDecorator> >  g_decorators;
::std::map< RcString, ::std::unique_ptr<ExpandProcMacro> >  g_macros;

void Expand_Attrs(const ::AST::AttributeList& attrs, AttrStage stage,  ::std::function<void(const ExpandDecorator& d,const ::AST::Attribute& a)> f);
void Expand_Mod(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Path modpath, ::AST::Module& mod, unsigned int first_item = 0);
void Expand_Expr(::AST::Crate& crate, LList<const AST::Module*> modstack, AST::Expr& node);
void Expand_Expr(::AST::Crate& crate, LList<const AST::Module*> modstack, ::std::shared_ptr<AST::ExprNode>& node);
void Expand_Path(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod, ::AST::Path& p);

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
}


void ExpandDecorator::unexpected(const Span& sp, const AST::Attribute& mi, const char* loc_str) const
{
    WARNING(sp, W0000, "Unexpected attribute " << mi.name() << " on " << loc_str);
}

void Expand_Attr(const Span& sp, const ::AST::Attribute& a, AttrStage stage,  ::std::function<void(const Span& sp, const ExpandDecorator& d,const ::AST::Attribute& a)> f)
{
    bool found = false;
    for( auto& d : g_decorators ) {
        if( a.name() == d.first ) {
            DEBUG("#[" << d.first << "] " << (int)d.second->stage() << "-" << (int)stage);
            if( d.second->stage() == stage ) {
                f(sp, *d.second, a);
                // TODO: Early return?
                // TODO: Annotate the attribute as having been handled
            }
            found = true;
        }
    }
    if( !found ) {
        // TODO: Create no-op handlers for a whole heap of attributes
        //WARNING(sp, W0000, "Unknown attribute " << a.name());
    }
}
void Expand_Attrs(const ::AST::AttributeList& attrs, AttrStage stage,  ::std::function<void(const Span& sp, const ExpandDecorator& d,const ::AST::Attribute& a)> f)
{
    for( auto& a : attrs.m_items )
    {
        Expand_Attr(a.span(), a, stage, f);
    }
}
void Expand_Attrs_CfgAttr(AST::AttributeList& attrs)
{
    for(auto it = attrs.m_items.begin(); it != attrs.m_items.end(); )
    {
        auto& a = *it;
        if( a.name() == "cfg_attr" ) {
            if( check_cfg(a.span(), a.items().at(0)) ) {
                auto inner_attr = mv$(a.items().at(1));
                a = mv$(inner_attr);
                ++ it;
            }
            else {
                it = attrs.m_items.erase(it);
            }
        }
        else {
            ++ it;
        }
    }
}
void Expand_Attrs(const ::AST::AttributeList& attrs, AttrStage stage,  ::AST::Crate& crate, const ::AST::Path& path, ::AST::Module& mod, ::AST::Item& item)
{
    Expand_Attrs(attrs, stage,  [&](const auto& sp, const auto& d, const auto& a){
        if(!item.is_None()) {
            // TODO: Pass attributes _after_ this attribute
            d.handle(sp, a, crate, path, mod, slice<const AST::Attribute>(&a, &attrs.m_items.back() - &a + 1), item);
        }
        });
}
void Expand_Attrs(const ::AST::AttributeList& attrs, AttrStage stage,  ::AST::Crate& crate, ::AST::Module& mod, ::AST::ImplDef& impl)
{
    Expand_Attrs(attrs, stage,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, mod, impl); });
}

::std::unique_ptr<TokenStream> Expand_Macro_Inner(
    const ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod,
    Span mi_span, const AST::Path& path, const RcString& input_ident, TokenTree& input_tt
    )
{
    if( !path.is_valid() ) {
        return ::std::unique_ptr<TokenStream>();
    }

    // Find the macro
    /*const*/ ExpandProcMacro*  proc_mac = nullptr;
    const MacroRules*   mr_ptr = nullptr;

    if( path.is_trivial() )
    {
        const auto& name = path.as_trivial();
        // 1. Search compiler-provided proc macros
        for( const auto& m : g_macros )
        {
            if( name == m.first )
            {
                proc_mac = &*m.second;
                break;
            }
        }
        // Iterate up the module tree, using the first located macro
        for(const auto* ll = &modstack; ll; ll = ll->m_prev)
        {
            const auto& mac_mod = *ll->m_item;
            for( const auto& mr : reverse(mac_mod.macros()) )
            {
                if( mr.name == name )
                {
                    DEBUG(mac_mod.path() << "::" << mr.name << " - Defined");
                    mr_ptr = &*mr.data;
                    break;
                }
            }
            if( mr_ptr )
                break;

            // Find the last macro of this name (allows later #[macro_use] definitions to override)
            const MacroRules* last_mac = nullptr;
            for( const auto& mri : mac_mod.macro_imports_res() )
            {
                //DEBUG("- " << mri.name);
                if( mri.name == name )
                {
                    DEBUG("?::" << mri.name << " - Imported");

                    last_mac = mri.data;
                }
            }
            if( last_mac )
            {
                mr_ptr = last_mac;
                break;
            }
        }
    }
    else
    {
        // HACK: If the crate name is empty, look up builtins
        if( path.is_absolute() && path.nodes().size() == 1 && path.m_class.as_Absolute().crate == "" )
        {
            const auto& name = path.nodes()[0].name();
            for( const auto& m : g_macros )
            {
                if( name == m.first )
                {
                    proc_mac = &*m.second;
                    break;
                }
            }
        }

        if( !proc_mac && !mr_ptr )
        {
            // Resolve the path, following use statements (if required)
            // - Only mr_ptr matters, as proc_mac is about builtins

            TAGGED_UNION(ModuleRef, None,
                (None, struct {}),
                (Ast, const AST::Module*),
                (Hir, const HIR::Module*)
                );
            struct H {
                static bool find_in_ast(const AST::Module& mod, const RcString& name, std::function<bool(const AST::Item& i)> cb)
                {
                    for(const auto& i : mod.items())
                    {
                        if(i.name == name)
                        {
                            if( cb(i.data) )
                                return true;
                        }
                        if(const auto* use_stmt = i.data.opt_Use())
                        {
                            for(const auto& e : use_stmt->entries)
                            {
                                if( e.name == name )
                                {
                                    TODO(Span(), "Look through use statements - " << e.path);
                                }
                            }
                        }
                    }
                    for(const auto& i : mod.items())
                    {
                        if(const auto* use_stmt = i.data.opt_Use())
                        {
                            for(const auto& e : use_stmt->entries)
                            {
                                if( e.name == "" )
                                {
                                    TODO(Span(), "Look through glob use statements (" << e.path << ")");
                                }
                            }
                        }
                    }
                    return false;
                }
                static ModuleRef get_root(const AST::Crate& crate, const RcString& crate_name)
                {
                    if(crate_name == "")
                    {
                        return ModuleRef::make_Ast(&crate.m_root_module);
                    }
                    else
                    {
                        return ModuleRef::make_Hir(&crate.m_extern_crates.at(crate_name).m_hir->m_root_module);
                    }
                }
                static ModuleRef get_submod_in_ast(const Span& sp, const AST::Crate& crate, const AST::Module& mod, const RcString& name)
                {
                    ModuleRef   rv;
                    H::find_in_ast(mod, name, [&](const AST::Item& i_data)->bool {
                        switch(i_data.tag())
                        {
                        case AST::Item::TAG_Crate:
                            rv = ModuleRef::make_Hir(&crate.m_extern_crates.at(i_data.as_Crate().name).m_hir->m_root_module);
                            return true;
                        case AST::Item::TAG_Module:
                            rv = ModuleRef::make_Ast(&i_data.as_Module());
                            return true;
                        case AST::Item::TAG_Struct:
                        case AST::Item::TAG_Union:
                        case AST::Item::TAG_Enum:
                        case AST::Item::TAG_Type:
                            ERROR(sp, E0000, "Macro path component points at a type");
                            break;
                        default:
                            break;
                        }
                        return false;
                        });
                    return rv;
                }
                static ModuleRef get_submod_in_hir(const Span& sp, const AST::Crate& crate, const HIR::Module& mod, const RcString& name)
                {
                    ModuleRef   rv;
                    TODO(sp, "Search HIR");
                }
                static ModuleRef get_submod(const Span& sp, const AST::Crate& crate, const ModuleRef& mod_ref, const RcString& name)
                {
                    TU_MATCH_HDRA( (mod_ref), {)
                    TU_ARMA(None, e)
                        BUG(sp, "");
                    TU_ARMA(Ast, ast_ptr)
                        return get_submod_in_ast(sp, crate, *ast_ptr, name);
                    TU_ARMA(Hir, hir_ptr)
                        return get_submod_in_hir(sp, crate, *hir_ptr, name);
                    }
                    BUG(sp, "");
                }
            };

            // 1. Convert to absolute (but not canonical)
            AST::Path   real_path;
            TU_MATCH_HDRA( (path.m_class), {)
            TU_ARMA(Invalid,  e) {
                throw ::std::runtime_error("Path::nodes() on Invalid");
                }
            TU_ARMA(Local, e)
                BUG(mi_span, "Local path should have been handled (trivial)");
            TU_ARMA(Relative, e) {
                ASSERT_BUG(mi_span, e.nodes.size() > 1, "Too few nodes (should have been trivial?) - " << path);
                // 1. Search current scope (current module), seeking up anon modules
                const LList<const AST::Module*>* cur_mod = &modstack;
                do {
                    // Search for a matching module/crate
                    if( H::find_in_ast(*cur_mod->m_item, e.nodes[0].name(), [&](const AST::Item& i_data)->bool {
                        switch(i_data.tag())
                        {
                        case AST::Item::TAG_Crate:
                            real_path = AST::Path(i_data.as_Crate().name, {});
                            return true;
                        case AST::Item::TAG_Module:
                            real_path = AST::Path(i_data.as_Module().path());
                            return true;
                        case AST::Item::TAG_Struct:
                        case AST::Item::TAG_Union:
                        case AST::Item::TAG_Enum:
                        case AST::Item::TAG_Type:
                            ERROR(mi_span, E0000, "Macro path component points at a type - " << path);
                            break;
                        default:
                            break;
                        }
                        return false;
                        }) )
                    {
                        break;
                    }
                } while(cur_mod->m_item->is_anon());
                // 2. If not found, look for a matching crate name
                if( !real_path.is_valid() )
                {
                    TODO(mi_span, "Seach for extern crates");
                }
                // 3. Error if not found
                if( !real_path.is_valid() )
                {
                    ERROR(mi_span, E0000, "Cannot find module for " << path);
                }
                for(size_t i = 1; i < e.nodes.size(); i ++)
                    real_path.nodes().push_back(e.nodes[i]);
                }
            TU_ARMA(Self, e) {
                auto new_path = mod.path();
                if(new_path.nodes().back().name().c_str()[0] == '#')
                    TODO(mi_span, "Handle self paths in anon");
                for(size_t i = 0; i < e.nodes.size(); i ++)
                    new_path.nodes().push_back(e.nodes[i]);
                real_path = mv$(new_path);
                }
            TU_ARMA(Super, e) {
                auto new_path = mod.path();
                if(new_path.nodes().back().name().c_str()[0] == '#')
                    TODO(mi_span, "Handle super paths in anon");
                for(auto i = e.count; i--; )
                {
                    if(new_path.nodes().empty())
                        ERROR(mi_span, E0000, "Invalid path (too many `super`) - " << path);
                    new_path.nodes().pop_back();
                }
                for(size_t i = 0; i < e.nodes.size(); i ++)
                    new_path.nodes().push_back(e.nodes[i]);
                real_path = mv$(new_path);
                }
            TU_ARMA(Absolute, e) {
                // No change
                real_path = AST::Path(path);
                }
            TU_ARMA(UFCS, e) {
                BUG(mi_span, "UFCS path to macro - " << path);
                }
            }
            // 2. Walk path to find item
            auto& real_path_abs = real_path.m_class.as_Absolute();
            auto cur_mod = H::get_root(crate, real_path_abs.crate);
            for(size_t i = 0; i < real_path_abs.nodes.size() - 1; i ++)
            {
                cur_mod = H::get_submod(mi_span, crate, cur_mod, real_path_abs.nodes[i].name());
                if(cur_mod.is_None())
                    ERROR(mi_span, E0000, "Unable to locate component " << i << " of " << real_path);
            }
            const auto& final_name = real_path_abs.nodes.back().name();
            TU_MATCH_HDRA( (cur_mod), {)
            TU_ARMA(None, e)
                BUG(mi_span, "Should have errored earlier");
            TU_ARMA(Ast, e) {
                // Look in the pre-calculated macro list (TODO: Should really be using the main resolve machinery)
                for(const auto& mac : e->macro_imports_res())
                {
                    if(mac.name == final_name) {
                        mr_ptr = mac.data;
                        break;
                    }
                }
                //if( !mr_ptr )
                //{
                //    H::find_in_ast(*e, real_path_abs.nodes.back().name(), [&](const AST::Item& i)->bool {
                //        if(const auto* e = i.opt_Macro())
                //        {
                //            mr_ptr = &**e;
                //            return true;
                //        }
                //        return false;
                //        });
                //}
                }
            TU_ARMA(Hir, e) {
                TODO(mi_span, "Look up macros in HIR modules");
                }
            }
            if(!mr_ptr)
                ERROR(mi_span, E0000, "");
        }
    }

    if( mr_ptr )
    {
        // TODO: If `mr_ptr` is tagged with #[rustc_builtin_macro], look for a matching entry in `g_macros`
    }

    if( proc_mac )
    {
        auto e = input_ident == ""
            ? proc_mac->expand(mi_span, crate, input_tt, mod)
            : proc_mac->expand_ident(mi_span, crate, input_ident, input_tt, mod)
            ;
        return e;
    }
    else if( mr_ptr )
    {
        if( input_ident != "" )
            ERROR(mi_span, E0000, "macro_rules! macros can't take an ident");

        DEBUG("Invoking macro_rules " << path << " " << mr_ptr);
        auto e = Macro_InvokeRules(path.is_trivial() ? path.as_trivial().c_str() : "", *mr_ptr, mi_span, mv$(input_tt), crate, mod);
        e->parse_state().crate = &crate;
        return e;
    }
    else
    {
    }
    // Error - Unknown macro name
    ERROR(mi_span, E0000, "Unknown macro " << path);
}
::std::unique_ptr<TokenStream> Expand_Macro(
    const ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod,
    Span mi_span, const AST::Path& name, const RcString& input_ident, TokenTree& input_tt
    )
{
    auto rv = Expand_Macro_Inner(crate, modstack, mod, mi_span, name, input_ident, input_tt);
    assert(rv);
    rv->parse_state().module = &mod;
    return rv;
}
::std::unique_ptr<TokenStream> Expand_Macro(const ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod, ::AST::MacroInvocation& mi)
{
    return Expand_Macro(crate, modstack, mod,  mi.span(), mi.path(), mi.input_ident(), mi.input_tt());
}

void Expand_Pattern(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod, ::AST::Pattern& pat, bool is_refutable)
{
    TU_MATCH(::AST::Pattern::Data, (pat.data()), (e),
    (MaybeBind,
        ),
    (Macro,
        const auto span = e.inv->span();

        auto tt = Expand_Macro(crate, modstack, mod,  *e.inv);
        if( ! tt ) {
            ERROR(span, E0000, "Macro in pattern didn't expand to anything");
        }
        auto& lex = *tt;
        auto newpat = Parse_Pattern(lex, is_refutable);
        if( LOOK_AHEAD(lex) != TOK_EOF ) {
            ERROR(span, E0000, "Trailing tokens in macro expansion");
        }

        if( pat.binding().is_valid() ) {
            if( newpat.binding().is_valid() )
                ERROR(span, E0000, "Macro expansion provided a binding, but one already present");
            newpat.binding() = mv$(pat.binding());
        }

        pat = mv$(newpat);
        Expand_Pattern(crate, modstack, mod, pat, is_refutable);
        ),
    (Any,
        ),
    (Box,
        Expand_Pattern(crate, modstack, mod,  *e.sub, is_refutable);
        ),
    (Ref,
        Expand_Pattern(crate, modstack, mod,  *e.sub, is_refutable);
        ),
    (Value,
        //Expand_Expr(crate, modstack, e.start);
        //Expand_Expr(crate, modstack, e.end);
        ),
    (Tuple,
        for(auto& sp : e.start)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        for(auto& sp : e.end)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        ),
    (StructTuple,
        for(auto& sp : e.tup_pat.start)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        for(auto& sp : e.tup_pat.end)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        ),
    (Struct,
        for(auto& sp : e.sub_patterns)
            Expand_Pattern(crate, modstack, mod, sp.second, is_refutable);
        ),
    (Slice,
        for(auto& sp : e.sub_pats)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        ),
    (SplitSlice,
        for(auto& sp : e.leading)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        for(auto& sp : e.trailing)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        )
    )
}

void Expand_Type(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod, ::TypeRef& ty)
{
    TU_MATCH(::TypeData, (ty.m_data), (e),
    (None,
        ),
    (Any,
        ),
    (Unit,
        ),
    (Bang,
        ),
    (Macro,
        auto tt = Expand_Macro(crate, modstack, mod,  e.inv);
        if(!tt)
            ERROR(e.inv.span(), E0000, "Macro invocation didn't yeild any data");
        auto new_ty = Parse_Type(*tt);
        if( tt->lookahead(0) != TOK_EOF )
            ERROR(e.inv.span(), E0000, "Extra tokens after parsed type");
        ty = mv$(new_ty);

        Expand_Type(crate, modstack, mod,  ty);
        ),
    (Primitive,
        ),
    (Function,
        Type_Function& tf = e.info;
        Expand_Type(crate, modstack, mod,  *tf.m_rettype);
        for(auto& st : tf.m_arg_types)
            Expand_Type(crate, modstack, mod,  st);
        ),
    (Tuple,
        for(auto& st : e.inner_types)
            Expand_Type(crate, modstack, mod,  st);
        ),
    (Borrow,
        Expand_Type(crate, modstack, mod,  *e.inner);
        ),
    (Pointer,
        Expand_Type(crate, modstack, mod,  *e.inner);
        ),
    (Array,
        Expand_Type(crate, modstack, mod,  *e.inner);
        if( e.size ) {
            Expand_Expr(crate, modstack,  e.size);
        }
        ),
    (Generic,
        ),
    (Path,
        Expand_Path(crate, modstack, mod,  e.path);
        ),
    (TraitObject,
        for(auto& p : e.traits)
        {
            // TODO: p.hrbs? Not needed until types are in those
            Expand_Path(crate, modstack, mod,  p.path);
        }
        ),
    (ErasedType,
        for(auto& p : e.traits)
        {
            // TODO: p.hrbs?
            Expand_Path(crate, modstack, mod,  p.path);
        }
        )
    )
}
void Expand_Path(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod, ::AST::Path& p)
{
    auto expand_nodes = [&](::std::vector<::AST::PathNode>& nodes) {
        for(auto& node : nodes)
        {
            for(auto& typ : node.args().m_types)
                Expand_Type(crate, modstack, mod, typ);
            for(auto& aty : node.args().m_assoc_equal)
                Expand_Type(crate, modstack, mod, aty.second);
            for(auto& aty : node.args().m_assoc_bound)
                Expand_Path(crate, modstack, mod, aty.second);
        }
        };

    TU_MATCHA( (p.m_class), (pe),
    (Invalid,
        ),
    (Local,
        ),
    (Relative,
        expand_nodes(pe.nodes);
        ),
    (Self,
        expand_nodes(pe.nodes);
        ),
    (Super,
        expand_nodes(pe.nodes);
        ),
    (Absolute,
        expand_nodes(pe.nodes);
        ),
    (UFCS,
        Expand_Type(crate, modstack, mod, *pe.type);
        if( pe.trait ) {
            Expand_Path(crate, modstack, mod, *pe.trait);
        }
        expand_nodes(pe.nodes);
        )
    )
}

struct CExpandExpr:
    public ::AST::NodeVisitor
{
    ::AST::Crate&    crate;
    LList<const AST::Module*>   modstack;
    ::std::unique_ptr<::AST::ExprNode> replacement;

    // Stack of `try { ... }` blocks (the string is the loop label for the desugaring)
    ::std::vector<RcString>   m_try_stack;
    unsigned m_try_index = 0;

    AST::ExprNode_Block*    current_block = nullptr;

    CExpandExpr(::AST::Crate& crate, LList<const AST::Module*> ms):
        crate(crate),
        modstack(ms)
    {
    }

    ::AST::Module& cur_mod() {
        return *const_cast< ::AST::Module*>(modstack.m_item);
    }

    void visit(::std::unique_ptr<AST::ExprNode>& cnode) {
        if(cnode.get())
        {
            auto attrs = mv$(cnode->attrs());
            Expand_Attrs_CfgAttr(attrs);
            Expand_Attrs(attrs, AttrStage::Pre,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, this->crate, cnode); });
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
            Expand_Attrs(cnode->attrs(), AttrStage::Post,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, this->crate, cnode); });
        assert( ! this->replacement );
    }
    void visit_nodelete(const ::AST::ExprNode& parent, ::std::unique_ptr<AST::ExprNode>& cnode) {
        if( cnode.get() != nullptr )
        {
            this->visit(cnode);
            if(cnode.get() == nullptr)
                ERROR(parent.span(), E0000, "#[cfg] not allowed in this position");
        }
        assert( ! this->replacement );
    }
    void visit_vector(::std::vector< ::std::unique_ptr<AST::ExprNode> >& cnodes) {
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
        auto ttl = Expand_Macro( crate, modstack, mod,  node.span(),  node.m_path, node.m_ident, node.m_tokens );
        if( !ttl.get() )
        {
            // No expansion
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
        }

        node.m_path = AST::Path();
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

        auto prev_modstack = this->modstack;
        if( node.m_local_mod ) {
            this->modstack = LList<const ::AST::Module*>(&prev_modstack, node.m_local_mod.get());
        }

        // TODO: macro_rules! invocations within the expression list influence this.
        // > Solution: Defer creation of the local module until during expand.
        if( node.m_local_mod ) {
            Expand_Mod(crate, modstack, node.m_local_mod->path(), *node.m_local_mod);
            mod_item_count = node.m_local_mod->items().size();
        }

        auto saved = this->current_block;
        this->current_block = &node;

        for( auto it = node.m_nodes.begin(); it != node.m_nodes.end(); )
        {
            assert( it->get() );

            if( auto* node_mac = dynamic_cast<::AST::ExprNode_Macro*>(it->get()) )
            {
                Expand_Attrs_CfgAttr( (*it)->attrs() );
                Expand_Attrs((*it)->attrs(), AttrStage::Pre,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, this->crate, *it); });
                if( !it->get() ) {
                    it = node.m_nodes.erase( it );
                    continue ;
                }

                assert(it->get() == node_mac);

                ::std::vector< ::AST::ExprNodeP>    new_nodes;
                this->visit_macro(*node_mac, &new_nodes);

                it = node.m_nodes.erase(it);
                it = node.m_nodes.insert(it, ::std::make_move_iterator(new_nodes.begin()), ::std::make_move_iterator(new_nodes.end()));
                // NOTE: Doesn't advance the iterator above, we want to re-visit the new node
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
            Expand_Mod(crate, modstack, node.m_local_mod->path(), *node.m_local_mod, mod_item_count);
        }

        this->modstack = mv$(prev_modstack);
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

        auto core_crate = (crate.m_load_std == ::AST::Crate::LOAD_NONE ? "" : "core");
        auto path_Ok  = ::AST::Path(core_crate, {::AST::PathNode("result"), ::AST::PathNode("Result"), ::AST::PathNode("Ok")});
        auto ok_node = ::AST::ExprNodeP(new ::AST::ExprNode_CallPath( mv$(path_Ok), ::make_vec1(mv$(node.m_inner)) ));
        auto break_node = AST::ExprNodeP(new AST::ExprNode_Flow(AST::ExprNode_Flow::BREAK, loop_name, mv$(ok_node)));
        this->replacement = AST::ExprNodeP(new AST::ExprNode_Loop(loop_name, mv$(break_node)));
    }
    void visit(::AST::ExprNode_Asm& node) override {
        for(auto& v : node.m_output)
            this->visit_nodelete(node, v.value);
        for(auto& v : node.m_input)
            this->visit_nodelete(node, v.value);
    }
    void visit(::AST::ExprNode_Flow& node) override {
        this->visit_nodelete(node, node.m_value);
    }
    void visit(::AST::ExprNode_LetBinding& node) override {
        Expand_Type(crate, modstack, this->cur_mod(),  node.m_type);
        Expand_Pattern(crate, modstack, this->cur_mod(),  node.m_pat, false);
        this->visit_nodelete(node, node.m_value);
    }
    void visit(::AST::ExprNode_Assign& node) override {
        this->visit_nodelete(node, node.m_slot);
        this->visit_nodelete(node, node.m_value);
    }
    void visit(::AST::ExprNode_CallPath& node) override {
        Expand_Path(crate, modstack, this->cur_mod(),  node.m_path);
        this->visit_vector(node.m_args);
    }
    void visit(::AST::ExprNode_CallMethod& node) override {
        // TODO: Path params.
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
        Expand_Pattern(crate, modstack, this->cur_mod(),  node.m_pattern, (node.m_type == ::AST::ExprNode_Loop::WHILELET));
        if(node.m_type == ::AST::ExprNode_Loop::FOR)
        {
            auto core_crate = (crate.m_load_std == ::AST::Crate::LOAD_NONE ? "" : "core");
            auto path_Some = ::AST::Path(core_crate, {::AST::PathNode("option"), ::AST::PathNode("Option"), ::AST::PathNode("Some")});
            auto path_None = ::AST::Path(core_crate, {::AST::PathNode("option"), ::AST::PathNode("Option"), ::AST::PathNode("None")});
            auto path_IntoIterator = ::AST::Path(core_crate, {::AST::PathNode("iter"), ::AST::PathNode("IntoIterator")});
            auto path_Iterator = ::AST::Path(core_crate, {::AST::PathNode("iter"), ::AST::PathNode("Iterator")});
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
                nullptr,
                mv$(node.m_code)
                ) );
            // - `None => break label`
            arms.push_back( ::AST::ExprNode_Match_Arm(
                ::make_vec1( ::AST::Pattern(::AST::Pattern::TagValue(), node.span(), ::AST::Pattern::Value::make_Named(path_None)) ),
                nullptr,
                ::AST::ExprNodeP(new ::AST::ExprNode_Flow(::AST::ExprNode_Flow::BREAK, node.m_label, nullptr))
                ) );

            replacement.reset(new ::AST::ExprNode_Match(
                ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                    ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(node.span()), path_IntoIterator, { ::AST::PathNode("into_iter") } ),
                    ::make_vec1( mv$(node.m_cond) )
                    )),
                ::make_vec1(::AST::ExprNode_Match_Arm(
                    ::make_vec1( ::AST::Pattern(::AST::Pattern::TagBind(), node.span(), "it") ),
                    nullptr,
                    ::AST::ExprNodeP(new ::AST::ExprNode_Loop(
                        node.m_label,
                        ::AST::ExprNodeP(new ::AST::ExprNode_Match(
                            ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                                ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(node.span()), path_Iterator, { ::AST::PathNode("next") } ),
                                ::make_vec1( ::AST::ExprNodeP(new ::AST::ExprNode_UniOp(
                                    ::AST::ExprNode_UniOp::REFMUT,
                                    ::AST::ExprNodeP(new ::AST::ExprNode_NamedValue( ::AST::Path("it") ))
                                    )) )
                                )),
                            mv$(arms)
                            ))
                        )) )
                    )
                ) );
        }
    }
    void visit(::AST::ExprNode_Match& node) override {
        this->visit_nodelete(node, node.m_val);
        for(auto& arm : node.m_arms)
        {
            Expand_Attrs_CfgAttr( arm.m_attrs );
            Expand_Attrs(arm.m_attrs, AttrStage::Pre ,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate,  arm); });
            if( arm.m_patterns.size() == 0 )
                continue ;
            for(auto& pat : arm.m_patterns) {
                Expand_Pattern(crate, modstack, this->cur_mod(),  pat, true);
            }
            this->visit_nodelete(node, arm.m_cond);
            this->visit_nodelete(node, arm.m_code);
            Expand_Attrs(arm.m_attrs, AttrStage::Post,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate,  arm); });
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
        for(auto& pat : node.m_patterns)
            Expand_Pattern(crate, modstack, this->cur_mod(),  pat, true);
        this->visit_nodelete(node, node.m_value);
        this->visit_nodelete(node, node.m_true);
        this->visit_nodelete(node, node.m_false);
    }
    void visit(::AST::ExprNode_Integer& node) override { }
    void visit(::AST::ExprNode_Float& node) override { }
    void visit(::AST::ExprNode_Bool& node) override { }
    void visit(::AST::ExprNode_String& node) override { }
    void visit(::AST::ExprNode_ByteString& node) override { }
    void visit(::AST::ExprNode_Closure& node) override {
        for(auto& arg : node.m_args) {
            Expand_Pattern(crate, modstack, this->cur_mod(),  arg.first, false);
            Expand_Type(crate, modstack, this->cur_mod(),  arg.second);
        }
        Expand_Type(crate, modstack, this->cur_mod(),  node.m_return);
        this->visit_nodelete(node, node.m_code);
    }
    void visit(::AST::ExprNode_StructLiteral& node) override {
        this->visit_nodelete(node, node.m_base_value);
        for(auto& val : node.m_values)
        {
            Expand_Attrs_CfgAttr(val.attrs);
            Expand_Attrs(val.attrs, AttrStage::Pre ,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate,  val); });
            if( !val.value )
                continue ;
            this->visit_nodelete(node, val.value);
            Expand_Attrs(val.attrs, AttrStage::Post,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate,  val); });
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
        Expand_Path(crate, modstack, this->cur_mod(),  node.m_path);
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
        Expand_Type(crate, modstack, this->cur_mod(),  node.m_type);
    }
    void visit(::AST::ExprNode_TypeAnnotation& node) override {
        this->visit_nodelete(node, node.m_value);
        Expand_Type(crate, modstack, this->cur_mod(),  node.m_type);
    }
    void visit(::AST::ExprNode_BinOp& node) override {
        this->visit_nodelete(node, node.m_left);
        this->visit_nodelete(node, node.m_right);

        switch(node.m_type)
        {
        case ::AST::ExprNode_BinOp::RANGE: {
            // NOTE: Not language items
            auto core_crate = (crate.m_load_std == ::AST::Crate::LOAD_NONE ? "" : "core");
            auto path_Range     = ::AST::Path(core_crate, {::AST::PathNode("ops"), ::AST::PathNode("Range") });
            auto path_RangeFrom = ::AST::Path(core_crate, {::AST::PathNode("ops"), ::AST::PathNode("RangeFrom") });
            auto path_RangeTo   = ::AST::Path(core_crate, {::AST::PathNode("ops"), ::AST::PathNode("RangeTo") });
            auto path_RangeFull = ::AST::Path(core_crate, {::AST::PathNode("ops"), ::AST::PathNode("RangeFull") });

            ::AST::ExprNode_StructLiteral::t_values values;
            if( node.m_left && node.m_right )
            {
                values.push_back({ {}, "start", mv$(node.m_left ) });
                values.push_back({ {}, "end"  , mv$(node.m_right) });
                replacement.reset( new ::AST::ExprNode_StructLiteral(mv$(path_Range), nullptr, mv$(values)) );
            }
            else if( node.m_left )
            {
                values.push_back({ {}, "start", mv$(node.m_left ) });
                replacement.reset( new ::AST::ExprNode_StructLiteral(mv$(path_RangeFrom), nullptr, mv$(values)) );
            }
            else if( node.m_right )
            {
                values.push_back({ {}, "end"  , mv$(node.m_right) });
                replacement.reset( new ::AST::ExprNode_StructLiteral(mv$(path_RangeTo), nullptr, mv$(values)) );
            }
            else
            {
                replacement.reset( new ::AST::ExprNode_StructLiteral(mv$(path_RangeFull), nullptr, mv$(values)) );
            }
            replacement->set_span( node.span() );
            break; }
        case ::AST::ExprNode_BinOp::RANGE_INC: {
            // NOTE: Not language items
            auto core_crate = (crate.m_load_std == ::AST::Crate::LOAD_NONE ? "" : "core");
            auto path_None = ::AST::Path(core_crate, { ::AST::PathNode("option"), ::AST::PathNode("Option"), ::AST::PathNode("None") });
            auto path_RangeInclusive_NonEmpty = ::AST::Path(core_crate, { ::AST::PathNode("ops"), ::AST::PathNode("RangeInclusive") });
            auto path_RangeToInclusive        = ::AST::Path(core_crate, { ::AST::PathNode("ops"), ::AST::PathNode("RangeToInclusive") });

            if( node.m_left )
            {
                ::AST::ExprNode_StructLiteral::t_values values;
                values.push_back({ {}, "start", mv$(node.m_left)  });
                values.push_back({ {}, "end"  , mv$(node.m_right) });
                if( TARGETVER_1_29 )
                    values.push_back({ {}, "is_empty", ::AST::ExprNodeP(new ::AST::ExprNode_NamedValue(mv$(path_None))) });
                replacement.reset( new ::AST::ExprNode_StructLiteral(mv$(path_RangeInclusive_NonEmpty), nullptr, mv$(values)) );
            }
            else
            {
                ::AST::ExprNode_StructLiteral::t_values values;
                values.push_back({ {}, "end",  mv$(node.m_right) });
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
            auto core_crate = (crate.m_load_std == ::AST::Crate::LOAD_NONE ? "" : "core");
            auto path_Ok  = ::AST::Path(core_crate, {::AST::PathNode("result"), ::AST::PathNode("Result"), ::AST::PathNode("Ok")});
            auto path_Err = ::AST::Path(core_crate, {::AST::PathNode("result"), ::AST::PathNode("Result"), ::AST::PathNode("Err")});
            auto path_From = ::AST::Path(core_crate, {::AST::PathNode("convert"), ::AST::PathNode("From")});
            path_From.nodes().back().args().m_types.push_back( ::TypeRef(node.span()) );
            // TODO: Lang item (needs lang items enumerated earlier)
            //auto it = crate.m_lang_items.find("try");
            //ASSERT_BUG(node.span(), it != crate.m_lang_items.end(), "Can't find the `try` lang item");
            //auto path_Try = it->second;
            auto path_Try = ::AST::Path(core_crate, {::AST::PathNode("ops"), ::AST::PathNode("Try")});
            auto path_Try_into_result = ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(node.span()), path_Try, { ::AST::PathNode("into_result") });
            auto path_Try_from_error  = ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(node.span()), path_Try, { ::AST::PathNode("from_error") });

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
                ::make_vec1( ::AST::Pattern(::AST::Pattern::TagNamedTuple(), node.span(), path_Ok, ::make_vec1( ::AST::Pattern(::AST::Pattern::TagBind(), node.span(), "v") )) ),
                nullptr,
                ::AST::ExprNodeP( new ::AST::ExprNode_NamedValue( ::AST::Path(::AST::Path::TagLocal(), "v") ) )
                ));
            // `Err(e) => return Try::from_error(From::from(e)),`
            arms.push_back(::AST::ExprNode_Match_Arm(
                ::make_vec1( ::AST::Pattern(::AST::Pattern::TagNamedTuple(), node.span(), path_Err, ::make_vec1( ::AST::Pattern(::AST::Pattern::TagBind(), node.span(), "e") )) ),
                nullptr,
                ::AST::ExprNodeP(new ::AST::ExprNode_Flow(
                    (m_try_stack.empty() ? ::AST::ExprNode_Flow::RETURN : ::AST::ExprNode_Flow::BREAK),   // NOTE: uses `break 'tryblock` instead of return if in a try block.
                    (m_try_stack.empty() ? RcString("") : m_try_stack.back()),
                    ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                        ::AST::Path(path_Try_from_error),
                        ::make_vec1(
                            ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                                ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(node.span()), mv$(path_From), { ::AST::PathNode("from") }),
                                ::make_vec1( ::AST::ExprNodeP( new ::AST::ExprNode_NamedValue( ::AST::Path(::AST::Path::TagLocal(), "e") ) ) )
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
    }
};

void Expand_Expr(::AST::Crate& crate, LList<const AST::Module*> modstack, ::std::unique_ptr<AST::ExprNode>& node)
{
    TRACE_FUNCTION_F("unique_ptr");
    auto visitor = CExpandExpr(crate, modstack);
    visitor.visit(node);
}
void Expand_Expr(::AST::Crate& crate, LList<const AST::Module*> modstack, ::std::shared_ptr<AST::ExprNode>& node)
{
    TRACE_FUNCTION_F("shared_ptr");
    auto visitor = CExpandExpr(crate, modstack);
    node->visit(visitor);
    if( visitor.replacement ) {
        node.reset( visitor.replacement.release() );
    }
}
void Expand_Expr(::AST::Crate& crate, LList<const AST::Module*> modstack, AST::Expr& node)
{
    TRACE_FUNCTION_F("AST::Expr");
    auto visitor = CExpandExpr(crate, modstack);
    node.visit_nodes(visitor);
    if( visitor.replacement ) {
        node = AST::Expr( mv$(visitor.replacement) );
    }
}

void Expand_GenericParams(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod,  ::AST::GenericParams& params)
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
            Expand_Type(crate, modstack, mod,  ty_def.get_default());
            }
        TU_ARMA(Value, val_def) {
            Expand_Type(crate, modstack, mod,  val_def.type());
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
            Expand_Type(crate, modstack, mod,  be.type);
            ),
        (IsTrait,
            Expand_Type(crate, modstack, mod,  be.type);
            Expand_Path(crate, modstack, mod,  be.trait);
            ),
        (MaybeTrait,
            Expand_Type(crate, modstack, mod,  be.type);
            Expand_Path(crate, modstack, mod,  be.trait);
            ),
        (NotTrait,
            Expand_Type(crate, modstack, mod,  be.type);
            Expand_Path(crate, modstack, mod,  be.trait);
            ),
        (Equality,
            Expand_Type(crate, modstack, mod,  be.type);
            Expand_Type(crate, modstack, mod,  be.replacement);
            )
        )
    }
}

void Expand_BareExpr(const ::AST::Crate& crate, const AST::Module& mod, ::std::unique_ptr<AST::ExprNode>& node)
{
    Expand_Expr(const_cast< ::AST::Crate&>(crate), LList<const AST::Module*>(nullptr, &mod), node);
}

void Expand_Impl(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Path modpath, ::AST::Module& mod, ::AST::Impl& impl)
{
    TRACE_FUNCTION_F(impl.def());
    Expand_Attrs_CfgAttr(impl.def().attrs());
    Expand_Attrs(impl.def().attrs(), AttrStage::Pre,  crate, mod, impl.def());
    if( impl.def().type().is_wildcard() ) {
        DEBUG("Deleted");
        return ;
    }
    Expand_GenericParams(crate, modstack, mod,  impl.def().params());

    Expand_Type(crate, modstack, mod,  impl.def().type());
    Expand_Path(crate, modstack, mod,  impl.def().trait().ent);

    DEBUG("> Items");
    for( unsigned int idx = 0; idx < impl.items().size(); idx ++ )
    {
        auto& i = impl.items()[idx];
        DEBUG("  - " << i.name << " :: " << i.attrs);

        // TODO: Make a path from the impl definition? Requires having the impl def resolved to be correct
        // - Does it? the namespace is essentially the same. There may be issues with wherever the path is used though
        //::AST::Path path = modpath + i.name;

        auto attrs = mv$(i.attrs);
        Expand_Attrs_CfgAttr(attrs);
        Expand_Attrs(attrs, AttrStage::Pre,  crate, AST::Path(), mod, *i.data); // TODO: UFCS path

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

                auto ttl = Expand_Macro(crate, modstack, mod, mi_owned);

                if( ttl.get() )
                {
                    // Re-parse tt
                    while( ttl->lookahead(0) != TOK_EOF )
                    {
                        Parse_Impl_Item(*ttl, impl);
                    }
                    // - Any new macro invocations ends up at the end of the list and handled
                }
                // Move back in (using the index, as the old pointr may be invalid)
                impl.items()[idx].data->as_MacroInv() = mv$(mi_owned);
            }
            }
        TU_ARMA(Function, e) {
            TRACE_FUNCTION_F("fn " << i.name);
            for(auto& arg : e.args()) {
                Expand_Pattern(crate, modstack, mod,  arg.first, false);
                Expand_Type(crate, modstack, mod,  arg.second);
            }
            Expand_Type(crate, modstack, mod,  e.rettype());
            Expand_Expr(crate, modstack, e.code());
            }
        TU_ARMA(Static, e) {
            TRACE_FUNCTION_F("static " << i.name);
            Expand_Expr(crate, modstack, e.value());
            Expand_Type(crate, modstack, mod,  e.type());
            }
        TU_ARMA(Type, e) {
            TRACE_FUNCTION_F("type " << i.name);
            Expand_Type(crate, modstack, mod,  e.type());
            }
        }

        // Run post-expansion decorators and restore attributes
        {
            auto& i = impl.items()[idx];
            Expand_Attrs(attrs, AttrStage::Post,  crate, AST::Path(), mod, *i.data); // TODO: UFCS path
            // TODO: How would this be populated? It got moved out?
            if( i.attrs.m_items.size() == 0 )
                i.attrs = mv$(attrs);
        }
    }

    Expand_Attrs(impl.def().attrs(), AttrStage::Post,  crate, mod, impl.def());
}
void Expand_ImplDef(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Path modpath, ::AST::Module& mod, ::AST::ImplDef& impl_def)
{
    Expand_Attrs_CfgAttr(impl_def.attrs());
    Expand_Attrs(impl_def.attrs(), AttrStage::Pre,  crate, mod, impl_def);
    if( impl_def.type().is_wildcard() ) {
        DEBUG("Deleted");
        return ;
    }
    Expand_GenericParams(crate, modstack, mod,  impl_def.params());

    Expand_Type(crate, modstack, mod,  impl_def.type());
    //Expand_Type(crate, modstack, mod,  impl_def.trait());

    Expand_Attrs(impl_def.attrs(), AttrStage::Post,  crate, mod, impl_def);
}

void Expand_Mod(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Path modpath, ::AST::Module& mod, unsigned int first_item)
{
    TRACE_FUNCTION_F("modpath = " << modpath << ", first_item=" << first_item);

    for( const auto& mi: mod.macro_imports_res() )
        DEBUG("- Imports '" << mi.name << "'");
    // Import all macros from parent module.
    if( first_item == 0 )
    {
        for( const auto& mi: mod.macro_imports_res() )
            DEBUG("- Imports '" << mi.name << "'");
        if( modstack.m_prev )
        {
            for(const auto& mac : modstack.m_prev->m_item->m_macro_imports)
            {
                mod.m_macro_imports.push_back(mac);
            }
        }
        for( const auto& mi: mod.m_macro_imports )
            DEBUG("- Imports '" << mi.path << "'");
    }

    // Insert prelude if: Enabled for this module, present for the crate, and this module is not an anon
    if( crate.m_prelude_path != AST::Path() )
    {
        if( mod.m_insert_prelude && ! mod.is_anon() ) {
            DEBUG("> Adding custom prelude " << crate.m_prelude_path);
            mod.add_item(Span(), false, "", ::AST::UseItem { Span(), ::make_vec1(::AST::UseItem::Ent { Span(), crate.m_prelude_path, "" }) }, {});
        }
        else {
            DEBUG("> Not inserting custom prelude (anon or disabled)");
        }
    }

    DEBUG("Items");
    for( unsigned int idx = first_item; idx < mod.items().size(); idx ++ )
    {
        auto& i = mod.items()[idx];

        DEBUG("- " << modpath << "::" << i.name << " (" << ::AST::Item::tag_to_str(i.data.tag()) << ") :: " << i.attrs);
        ::AST::Path path = modpath + i.name;

        auto attrs = mv$(i.attrs);
        Expand_Attrs_CfgAttr(attrs);
        Expand_Attrs(attrs, AttrStage::Pre,  crate, path, mod, i.data);

        auto dat = mv$(i.data);

        TU_MATCH_HDRA( (dat), {)
        TU_ARMA(None, e) {
            // Skip: nothing
            }
        TU_ARMA(MacroInv, e) {
            // Move out of the module to avoid invalidation if a new macro invocation is added
            auto mi_owned = mv$(e);

            TRACE_FUNCTION_F("Macro invoke " << mi_owned.path());

            auto ttl = Expand_Macro(crate, modstack, mod, mi_owned);
            assert( mi_owned.path().is_valid() );

            if( ttl.get() )
            {
                // Re-parse tt
                // TODO: All new items should be placed just after this?
                assert(ttl.get());
                DEBUG("-- Parsing as mod items");
                Parse_ModRoot_Items(*ttl, mod);
            }
            dat.as_MacroInv() = mv$(mi_owned);
            }
        TU_ARMA(Macro, e) {
            mod.add_macro(i.is_pub, i.name, mv$(e));
            }
        TU_ARMA(Use, e) {
            // No inner expand.
            }
        TU_ARMA(ExternBlock, e) {
            // TODO: Run expand on inner items?
            // HACK: Just convert inner items into outer items
            auto items = mv$( e.items() );
            for(auto& i2 : items)
            {
                mod.items().push_back( mv$(i2) );
            }
            }
        TU_ARMA(Impl, e) {
            Expand_Impl(crate, modstack, modpath, mod,  e);
            if( e.def().type().is_wildcard() ) {
                dat = AST::Item();
            }
            }
        TU_ARMA(NegImpl, e) {
            Expand_ImplDef(crate, modstack, modpath, mod,  e);
            if( e.type().is_wildcard() ) {
                dat = AST::Item();
            }
            }
        TU_ARMA(Module, e) {
            LList<const AST::Module*>   sub_modstack(&modstack, &e);
            Expand_Mod(crate, sub_modstack, path, e);
            }
        TU_ARMA(Crate, e) {
            // Can't recurse into an `extern crate`
            if(crate.m_extern_crates.count(e.name) == 0)
            {
                e.name = crate.load_extern_crate( i.span, e.name );
            }
            }

        TU_ARMA(Struct, e) {
            Expand_GenericParams(crate, modstack, mod,  e.params());
            TU_MATCH(AST::StructData, (e.m_data), (sd),
            (Unit,
                ),
            (Struct,
                for(auto it = sd.ents.begin(); it != sd.ents.end(); ) {
                    auto& si = *it;
                    Expand_Attrs_CfgAttr(si.m_attrs);
                    Expand_Attrs(si.m_attrs, AttrStage::Pre, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });
                    Expand_Type(crate, modstack, mod,  si.m_type);
                    Expand_Attrs(si.m_attrs, AttrStage::Post, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });

                    if( si.m_name == "" )
                        it = sd.ents.erase(it);
                    else
                        ++it;
                }
                ),
            (Tuple,
                for(auto it = sd.ents.begin(); it != sd.ents.end(); ) {
                    auto& si = *it;
                    Expand_Attrs_CfgAttr(si.m_attrs);
                    Expand_Attrs(si.m_attrs, AttrStage::Pre, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });
                    Expand_Type(crate, modstack, mod,  si.m_type);
                    Expand_Attrs(si.m_attrs, AttrStage::Post, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });

                    if( ! si.m_type.is_valid() )
                        it = sd.ents.erase(it);
                    else
                        ++it;
                }
                )
            )
            }
        TU_ARMA(Enum, e) {
            Expand_GenericParams(crate, modstack, mod,  e.params());
            for(auto& var : e.variants()) {
                Expand_Attrs_CfgAttr(var.m_attrs);
                Expand_Attrs(var.m_attrs, AttrStage::Pre,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, var); });
                TU_MATCH(::AST::EnumVariantData, (var.m_data), (e),
                (Value,
                    Expand_Expr(crate, modstack,  e.m_value);
                    ),
                (Tuple,
                    for(auto& ty : e.m_sub_types) {
                        Expand_Type(crate, modstack, mod,  ty);
                    }
                    ),
                (Struct,
                    for(auto it = e.m_fields.begin(); it != e.m_fields.end(); ) {
                        auto& si = *it;
                        Expand_Attrs_CfgAttr(si.m_attrs);
                        Expand_Attrs(si.m_attrs, AttrStage::Pre, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });
                        Expand_Type(crate, modstack, mod,  si.m_type);
                        Expand_Attrs(si.m_attrs, AttrStage::Post, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });

                        if( si.m_name == "" )
                            it = e.m_fields.erase(it);
                        else
                            ++it;
                    }
                    )
                )
                Expand_Attrs(var.m_attrs, AttrStage::Post,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, var); });
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
            Expand_GenericParams(crate, modstack, mod,  e.m_params);
            for(auto it = e.m_variants.begin(); it != e.m_variants.end(); )
            {
                auto& si = *it;
                Expand_Attrs_CfgAttr(si.m_attrs);
                Expand_Attrs(si.m_attrs, AttrStage::Pre, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });
                Expand_Type(crate, modstack, mod,  si.m_type);
                Expand_Attrs(si.m_attrs, AttrStage::Post, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });

                if( si.m_name == "" )
                    it = e.m_variants.erase(it);
                else
                    ++it;
            }
            }
        TU_ARMA(Trait, e) {
            Expand_GenericParams(crate, modstack, mod,  e.params());
            auto& trait_items = e.items();
            for(size_t idx = 0; idx < trait_items.size(); idx ++)
            {
                auto& ti = trait_items[idx];
                DEBUG(" - " << ti.name << " " << ti.data.tag_str());
                auto attrs = mv$(ti.attrs);
                Expand_Attrs_CfgAttr(attrs);
                Expand_Attrs(attrs, AttrStage::Pre,  crate, AST::Path(), mod, ti.data);

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

                        auto ttl = Expand_Macro(crate, modstack, mod, mi_owned);

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
                        }
                        // Move back in (using the index, as the old pointer may be invalid)
                        trait_items[idx].data.as_MacroInv() = mv$(mi_owned);
                    }
                    }
                TU_ARMA(Function, e) {
                    Expand_GenericParams(crate, modstack, mod,  e.params());
                    for(auto& arg : e.args()) {
                        Expand_Pattern(crate, modstack, mod,  arg.first, false);
                        Expand_Type(crate, modstack, mod,  arg.second);
                    }
                    Expand_Type(crate, modstack, mod,  e.rettype());
                    Expand_Expr(crate, modstack, e.code());
                    }
                TU_ARMA(Static, e) {
                    Expand_Expr(crate, modstack, e.value());
                    Expand_Type(crate, modstack, mod,  e.type());
                    }
                TU_ARMA(Type, e) {
                    Expand_Type(crate, modstack, mod,  e.type());
                    }
                }

                {
                    auto& ti = trait_items[idx];

                    Expand_Attrs(attrs, AttrStage::Post,  crate, AST::Path(), mod, ti.data);
                    if( ti.attrs.m_items.size() == 0 )
                        ti.attrs = mv$(attrs);
                }
            }
            }
        TU_ARMA(Type, e) {
            Expand_Type(crate, modstack, mod,  e.type());
            }

        TU_ARMA(Function, e) {
            Expand_GenericParams(crate, modstack, mod,  e.params());
            for(auto& arg : e.args()) {
                Expand_Pattern(crate, modstack, mod,  arg.first, false);
                Expand_Type(crate, modstack, mod,  arg.second);
            }
            Expand_Type(crate, modstack, mod,  e.rettype());
            Expand_Expr(crate, modstack, e.code());
            }
        TU_ARMA(Static, e) {
            Expand_Expr(crate, modstack, e.value());
            Expand_Type(crate, modstack, mod,  e.type());
            }
        }
        Expand_Attrs(attrs, AttrStage::Post,  crate, path, mod, dat);

        {

            auto& i = mod.items()[idx];
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

    for(auto& i : mod.items())
    {
        DEBUG("- " << i.data.tag_str() << " '" << i.name << "'");
        TU_IFLET(::AST::Item, (i.data), Module, e,
            Expand_Mod_IndexAnon(crate, e);

            // TODO: Also ensure that all #[macro_export] macros end up in parent
        )
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
void Expand(::AST::Crate& crate)
{
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
    for(const auto& e : g_decorators)
    {
        DEBUG("Decorator: " << e.first);
    }
    for(const auto& e : g_macros)
    {
        DEBUG("Macro: " << e.first);
    }

    auto modstack = LList<const ::AST::Module*>(nullptr, &crate.m_root_module);

    // 1. Crate attributes
    Expand_Attrs_CfgAttr(crate.m_attrs);
    Expand_Attrs(crate.m_attrs, AttrStage::Pre,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate); });

    // Insert magic for libstd/libcore
    // NOTE: The actual crates are loaded in "LoadCrates" using magic in AST::Crate::load_externs
    switch( crate.m_load_std )
    {
    case ::AST::Crate::LOAD_STD:
        if( crate.m_prelude_path == AST::Path() )
            crate.m_prelude_path = AST::Path("std", {AST::PathNode("prelude"), AST::PathNode("v1")});
        crate.m_extern_crates.at("std").with_all_macros([&](const auto& name, const auto& mac) {
            crate.m_root_module.add_macro_import( name, mac );
            });
        crate.m_root_module.add_ext_crate(Span(), /*is_pub=*/false, "std", "std", /*attrs=*/{});
        break;
    case ::AST::Crate::LOAD_CORE:
        if( crate.m_prelude_path == AST::Path() )
            crate.m_prelude_path = AST::Path("core", {AST::PathNode("prelude"), AST::PathNode("v1")});
        crate.m_extern_crates.at("core").with_all_macros([&](const auto& name, const auto& mac) {
            crate.m_root_module.add_macro_import( name, mac );
            });
        crate.m_root_module.add_ext_crate(Span(), /*is_pub=*/false, "core", "core", /*attrs=*/{});
        break;
    case ::AST::Crate::LOAD_NONE:
        break;
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
    Expand_Mod(crate, modstack, ::AST::Path("",{}), crate.m_root_module);

    //Expand_Attrs(crate.m_attrs, AttrStage::Post,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate); });

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

            for(auto& i : mod.items()) {
                if( i.data.is_Module() )
                    mods.push_back( &i.data.as_Module() );
            }
        } while( mods.size() > 0 );

        // - Exported macros imported by the root (is this needed?)
        for( auto& mac : crate.m_root_module.macro_imports_res() ) {
            if( mac.data->m_exported && mac.name != "" ) {
                auto v = ::std::make_pair( mac.name, mac.data );
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
        // - Re-exported macros (ignore proc macros for now?)
        for( const auto& mac : crate.m_root_module.m_macro_imports )
        {
            if( mac.is_pub )
            {
                if( !mac.macro_ptr ) {
                    continue ;
                }
                auto v = ::std::make_pair( mac.name, mac.macro_ptr );

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


