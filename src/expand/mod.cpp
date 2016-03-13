/*
 */
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>
#include <synext.hpp>
#include <map>
#include "macro_rules.hpp"
#include "../parse/common.hpp"  // For reparse from macros
#include <ast/expr.hpp>
#include "cfg.hpp"

::std::map< ::std::string, ::std::unique_ptr<ExpandDecorator> >  g_decorators;
::std::map< ::std::string, ::std::unique_ptr<ExpandProcMacro> >  g_macros;

void Expand_Attrs(const ::AST::MetaItems& attrs, AttrStage stage,  ::std::function<void(const ExpandDecorator& d,const ::AST::MetaItem& a)> f);
void Expand_Mod(bool is_early, ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Path modpath, ::AST::Module& mod);
void Expand_Expr(bool is_early, ::AST::Crate& crate, LList<const AST::Module*> modstack, AST::Expr& node);

void Register_Synext_Decorator(::std::string name, ::std::unique_ptr<ExpandDecorator> handler) {
    g_decorators[name] = mv$(handler);
}
void Register_Synext_Macro(::std::string name, ::std::unique_ptr<ExpandProcMacro> handler) {
    g_macros[name] = mv$(handler);
}

namespace {
    AttrStage stage_pre(bool is_early) {
        return (is_early ? AttrStage::EarlyPre : AttrStage::LatePre);
    }
    AttrStage stage_post(bool is_early) {
        return (is_early ? AttrStage::EarlyPost : AttrStage::LatePost);
    }
}

void Expand_Attr(const ::AST::MetaItem& a, AttrStage stage,  ::std::function<void(const ExpandDecorator& d,const ::AST::MetaItem& a)> f)
{
    for( auto& d : g_decorators ) {
        if( d.first == a.name() ) {
            DEBUG("#[" << d.first << "] " << (int)d.second->stage() << "-" << (int)stage);
            if( d.second->stage() == stage ) {
                f(*d.second, a);
            }
        }
    }
}
void Expand_Attrs(const ::AST::MetaItems& attrs, AttrStage stage,  ::std::function<void(const ExpandDecorator& d,const ::AST::MetaItem& a)> f)
{
    for( auto& a : attrs.m_items )
    {
        if( a.name() == "cfg_attr" ) {
            if( check_cfg(Span(), a.items().at(0)) ) {
                Expand_Attr(a.items().at(1), stage, f);
            }
        }
        else {
            Expand_Attr(a, stage, f);
        }
    }
}
void Expand_Attrs(const ::AST::MetaItems& attrs, AttrStage stage,  ::AST::Crate& crate, const ::AST::Path& path, ::AST::Module& mod, ::AST::Item& item)
{
    Expand_Attrs(attrs, stage,  [&](const auto& d, const auto& a){ d.handle(a, crate, path, mod, item); });
}

::std::unique_ptr<TokenStream> Expand_Macro(
    bool is_early, const ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod,
    Span mi_span, const ::std::string& name, const ::std::string& input_ident, const TokenTree& input_tt
    )
{
    for( const auto& m : g_macros )
    {
        if( name == m.first && m.second->expand_early() == is_early )
        {
            auto e = m.second->expand(mi_span, crate, input_ident, input_tt, mod);
            return e;
        }
    }
    
    
    // Iterate up the module tree, using the first located macro
    for(const auto* ll = &modstack; ll; ll = ll->m_prev)
    {
        const auto& mac_mod = *ll->m_item;
        for( const auto& mr : mac_mod.macros() )
        {
            //DEBUG("- " << mr.name);
            if( mr.name == name )
            {
                if( input_ident != "" )
                    ERROR(mi_span, E0000, "macro_rules! macros can't take an ident");
                
                auto e = Macro_Invoke(name.c_str(), mr.data, input_tt, mod);
                return e;
            }
        }
        for( const auto& mri : mac_mod.macro_imports_res() )
        {
            //DEBUG("- " << mri.name);
            if( mri.name == name )
            {
                if( input_ident != "" )
                    ERROR(mi_span, E0000, "macro_rules! macros can't take an ident");
                
                auto e = Macro_Invoke(name.c_str(), *mri.data, input_tt, mod);
                return e;
            }
        }
    }
    
    if( ! is_early ) {
        // Error - Unknown macro name
        ERROR(mi_span, E0000, "Unknown macro '" << name << "'");
    }
    
    // Leave valid and return an empty expression
    return ::std::unique_ptr<TokenStream>();
}
::std::unique_ptr<TokenStream> Expand_Macro(bool is_early, const ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod, const ::AST::MacroInvocation& mi)
{
    return Expand_Macro(is_early, crate, modstack, mod,  mi.span(), mi.name(), mi.input_ident(), mi.input_tt());
}

void Expand_Pattern(bool is_early, ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod, ::AST::Pattern& pat)
{
    TU_MATCH(::AST::Pattern::Data, (pat.data()), (e),
    (MaybeBind,
        ),
    (Macro,
        auto tt = Expand_Macro(is_early, crate, modstack, mod,  *e.inv);
        TODO(e.inv->span(), "Expand macro invocation in pattern");
        ),
    (Any,
        ),
    (Box,
        Expand_Pattern(is_early, crate, modstack, mod,  *e.sub);
        ),
    (Ref,
        Expand_Pattern(is_early, crate, modstack, mod,  *e.sub);
        ),
    (Value,
        Expand_Expr(is_early, crate, modstack, e.start);
        Expand_Expr(is_early, crate, modstack, e.end);
        ),
    (Tuple,
        for(auto& sp : e.sub_patterns)
            Expand_Pattern(is_early, crate, modstack, mod, sp);
        ),
    (StructTuple,
        for(auto& sp : e.sub_patterns)
            Expand_Pattern(is_early, crate, modstack, mod, sp);
        ),
    (Struct,
        for(auto& sp : e.sub_patterns)
            Expand_Pattern(is_early, crate, modstack, mod, sp.second);
        ),
    (Slice,
        for(auto& sp : e.leading)
            Expand_Pattern(is_early, crate, modstack, mod, sp);
        for(auto& sp : e.trailing)
            Expand_Pattern(is_early, crate, modstack, mod, sp);
        )
    )
}

void Expand_Type(bool is_early, ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod, ::TypeRef& ty)
{
    TU_MATCH(::TypeData, (ty.m_data), (e),
    (None,
        ),
    (Any,
        ),
    (Unit,
        ),
    (Macro,
        auto tt = Expand_Macro(is_early, crate, modstack, mod,  e.inv);
        TODO(e.inv.span(), "Expand macro invocation in type");
        ),
    (Primitive,
        ),
    (Function,
        TODO(ty.span(), "Expand function type " << ty);
        ),
    (Tuple,
        for(auto& st : e.inner_types)
            Expand_Type(is_early, crate, modstack, mod,  st);
        ),
    (Borrow,
        Expand_Type(is_early, crate, modstack, mod,  *e.inner);
        ),
    (Pointer,
        Expand_Type(is_early, crate, modstack, mod,  *e.inner);
        ),
    (Array,
        Expand_Type(is_early, crate, modstack, mod,  *e.inner);
        // TODO: Array size expression
        //Expand_Expr(is_early, crate, modstack,  e.size);
        ),
    (Generic,
        ),
    (Path,
        ),
    (TraitObject,
        )
    )
}

struct CExpandExpr:
    public ::AST::NodeVisitor
{
    bool is_early;
    ::AST::Crate&    crate;
    LList<const AST::Module*>   modstack;
    ::std::unique_ptr<::AST::ExprNode> replacement;
    
    CExpandExpr(bool is_early, ::AST::Crate& crate, LList<const AST::Module*> ms):
        is_early(is_early),
        crate(crate),
        modstack(ms)
    {
    }
    
    ::AST::Module& cur_mod() {
        return *(::AST::Module*)(modstack.m_item);
    }
    
    void visit(::std::unique_ptr<AST::ExprNode>& cnode) {
        if(cnode.get())
            Expand_Attrs(cnode->attrs(), stage_pre(is_early),  [&](const auto& d, const auto& a){ d.handle(a, this->crate, cnode); });
        if(cnode.get())
        {
            cnode->visit(*this);
            if( auto* n_mac = dynamic_cast<AST::ExprNode_Macro*>(cnode.get()) )
            {
                if( n_mac->m_name == "" )
                    cnode.reset();
            }
            if( this->replacement.get() ) {
                cnode = mv$(this->replacement);
            }
        }
        
        if(cnode.get())
            Expand_Attrs(cnode->attrs(), stage_post(is_early),  [&](const auto& d, const auto& a){ d.handle(a, this->crate, cnode); });
    }
    void visit_nodelete(const ::AST::ExprNode& parent, ::std::unique_ptr<AST::ExprNode>& cnode) {
        if( cnode.get() != nullptr )
        {
            this->visit(cnode);
            if(cnode.get() == nullptr)
                ERROR(parent.get_pos(), E0000, "#[cfg] not allowed in this position");
        }
    }
    void visit_vector(::std::vector< ::std::unique_ptr<AST::ExprNode> >& cnodes) {
        for( auto& child : cnodes ) {
            this->visit(child);
        }
        // Delete null children
        for( auto it = cnodes.begin(); it != cnodes.end(); ) {
            if( it->get() == nullptr ) {
                it = cnodes.erase( it );
            }
            else {
                ++ it;
            }
        }
    }
    
    void visit(::AST::ExprNode_Macro& node) override {
        auto& mod = this->cur_mod();
        auto ttl = Expand_Macro(
            is_early, crate, modstack, mod,
            Span(node.get_pos()),
            node.m_name, node.m_ident, node.m_tokens
            );
        if( ttl.get() != nullptr )
        {
            if( ttl->lookahead(0) != TOK_EOF )
            {
                SET_MODULE( (*ttl), mod );
                // Reparse as expression / item
                auto newexpr = Parse_Expr0(*ttl);
                // Then call visit on it again
                this->visit(newexpr);
                // And schedule it to replace the previous
                replacement = mv$(newexpr);
            }
            else
            {
                node.m_name = "";
            }
        }
    }
    
    void visit(::AST::ExprNode_Block& node) override {
        if( node.m_local_mod ) {
            Expand_Mod(is_early, crate, modstack, node.m_local_mod->path(), *node.m_local_mod);
        }
        this->visit_vector(node.m_nodes);
    }
    void visit(::AST::ExprNode_Flow& node) override {
        this->visit_nodelete(node, node.m_value);
    }
    void visit(::AST::ExprNode_LetBinding& node) override {
        Expand_Type(is_early, crate, modstack, this->cur_mod(),  node.m_type);
        Expand_Pattern(is_early, crate, modstack, this->cur_mod(),  node.m_pat);
        this->visit_nodelete(node, node.m_value);
    }
    void visit(::AST::ExprNode_Assign& node) override {
        this->visit_nodelete(node, node.m_slot);
        this->visit_nodelete(node, node.m_value);
    }
    void visit(::AST::ExprNode_CallPath& node) override {
        this->visit_vector(node.m_args);
    }
    void visit(::AST::ExprNode_CallMethod& node) override {
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
    }
    void visit(::AST::ExprNode_Match& node) override {
        this->visit_nodelete(node, node.m_val);
        for(auto& arm : node.m_arms)
        {
            // TODO: Attributes on match arms (is it only #[cfg] that's allowed?)
            for(auto& pat : arm.m_patterns) {
                Expand_Pattern(is_early, crate, modstack, this->cur_mod(),  pat);
            }
            this->visit_nodelete(node, arm.m_cond);
            this->visit_nodelete(node, arm.m_code);
        }
    }
    void visit(::AST::ExprNode_If& node) override {
        this->visit_nodelete(node, node.m_cond);
        this->visit_nodelete(node, node.m_true);
        this->visit_nodelete(node, node.m_false);
    }
    void visit(::AST::ExprNode_IfLet& node) override {
        Expand_Pattern(is_early, crate, modstack, this->cur_mod(),  node.m_pattern);
        this->visit_nodelete(node, node.m_value);
        this->visit_nodelete(node, node.m_true);
        this->visit_nodelete(node, node.m_false);
    }
    void visit(::AST::ExprNode_Integer& node) override { }
    void visit(::AST::ExprNode_Float& node) override { }
    void visit(::AST::ExprNode_Bool& node) override { }
    void visit(::AST::ExprNode_String& node) override { }
    void visit(::AST::ExprNode_Closure& node) override {
        for(auto& arg : node.m_args) {
            Expand_Pattern(is_early, crate, modstack, this->cur_mod(),  arg.first);
            Expand_Type(is_early, crate, modstack, this->cur_mod(),  arg.second);
        }
        Expand_Type(is_early, crate, modstack, this->cur_mod(),  node.m_return);
        this->visit_nodelete(node, node.m_code);
    }
    void visit(::AST::ExprNode_StructLiteral& node) override {
        this->visit_nodelete(node, node.m_base_value);
        for(auto& val : node.m_values)
        {
            // TODO: Attributes on struct literal items (#[cfg] only?)
            this->visit_nodelete(node, val.second);
        }
    }
    void visit(::AST::ExprNode_Array& node) override {
        this->visit_nodelete(node, node.m_size);
        this->visit_vector(node.m_values);
    }
    void visit(::AST::ExprNode_Tuple& node) override {
        this->visit_vector(node.m_values);
    }
    void visit(::AST::ExprNode_NamedValue& node) override { }
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
        Expand_Type(is_early, crate, modstack, this->cur_mod(),  node.m_type);
    }
    void visit(::AST::ExprNode_BinOp& node) override {
        this->visit_nodelete(node, node.m_left);
        this->visit_nodelete(node, node.m_right);
    }
    void visit(::AST::ExprNode_UniOp& node) override {
        this->visit_nodelete(node, node.m_value);
    }
};

void Expand_Expr(bool is_early, ::AST::Crate& crate, LList<const AST::Module*> modstack, ::std::unique_ptr<AST::ExprNode>& node) {
    auto visitor = CExpandExpr(is_early, crate, modstack);
    visitor.visit(node);
}
void Expand_Expr(bool is_early, ::AST::Crate& crate, LList<const AST::Module*> modstack, AST::Expr& node)
{
    auto visitor = CExpandExpr(is_early, crate, modstack);
    node.visit_nodes(visitor);
}

void Expand_Mod(bool is_early, ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Path modpath, ::AST::Module& mod)
{
    TRACE_FUNCTION_F("is_early = " << is_early << ", modpath = " << modpath);
    
    for( const auto& mi: mod.macro_imports_res() )
        DEBUG("- Imports '" << mi.name << "'");
    
    // TODO: Have the AST representation of a module include the definition order,
    //  mixing macro invocations, general items, use statements, and `impl`s
    
    // 1. Macros first
    //for( auto& mi : mod.macro_invs() )
    for(unsigned int i = 0; i < mod.macro_invs().size(); i ++ )
    {
        auto& mi = mod.macro_invs()[i];
        DEBUG("> Macro invoke '"<<mi.name()<<"'");
        if( mi.name() != "" )
        {
            // Move out of the module to avoid invalidation if a new macro invocation is added
            auto mi_owned = mv$(mi);
            
            auto ttl = Expand_Macro(is_early, crate, modstack, mod, mi_owned);

            if( ! ttl.get() )
            {
                // - Return ownership to the list
                mod.macro_invs()[i] = mv$(mi_owned);
            }
            else
            {
                // Re-parse tt
                assert(ttl.get());
                Parse_ModRoot_Items(*ttl, mod, false, "-");
                // - Any new macro invocations ends up at the end of the list and handled
            }
        }
    }
    
    // 2. General items
    DEBUG("Items");
    for( auto& i : mod.items() )
    {
        DEBUG("- " << i.name << " :: " << i.data.attrs);
        ::AST::Path path = modpath + i.name;
        
        auto attrs = mv$(i.data.attrs);
        Expand_Attrs(attrs, stage_pre(is_early),  crate, path, mod, i.data);
        
        TU_MATCH(::AST::Item, (i.data), (e),
        (None,
            // Skip, nothing
            ),
        (Module,
            LList<const AST::Module*>   sub_modstack(&modstack, &e.e);
            Expand_Mod(is_early, crate, sub_modstack, path, e.e);
            ),
        (Crate,
            // Can't recurse into an `extern crate`
            ),
        
        (Struct,
            // TODO: Struct items
            ),
        (Enum,
            // TODO: Enum variants
            ),
        (Trait,
            // TODO: Trait definition
            ),
        (Type,
            Expand_Type(is_early, crate, modstack, mod,  e.e.type());
            ),
        
        (Function,
            for(auto& arg : e.e.args()) {
                Expand_Pattern(is_early, crate, modstack, mod,  arg.first);
                Expand_Type(is_early, crate, modstack, mod,  arg.second);
            }
            Expand_Type(is_early, crate, modstack, mod,  e.e.rettype());
            Expand_Expr(is_early, crate, modstack, e.e.code());
            ),
        (Static,
            Expand_Expr(is_early, crate, modstack, e.e.value());
            )
        )
        
        Expand_Attrs(attrs, stage_post(is_early),  crate, path, mod, i.data);
        if( i.data.attrs.m_items.size() == 0 )
            i.data.attrs = mv$(attrs);
    }
    
    DEBUG("Impls");
    for( auto& i : mod.impls() )
    {
        DEBUG("- " << i);
    }
    
    for( const auto& mi: mod.macro_imports_res() )
        DEBUG("- Imports '" << mi.name << "'");

    // 3. Post-recurse macros (everything else)
}
void Expand(::AST::Crate& crate)
{
    auto modstack = LList<const ::AST::Module*>(nullptr, &crate.m_root_module);
    
    // 1. Crate attributes
    Expand_Attrs(crate.m_attrs, AttrStage::EarlyPre,  [&](const auto& d, const auto& a){ d.handle(a, crate); });
    
    // TODO: Load std/core
    
    // 2. Module attributes
    for( auto& a : crate.m_attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( d.first == a.name() && d.second->stage() == AttrStage::EarlyPre ) {
                //d.second->handle(a, crate, ::AST::Path(), crate.m_root_module, crate.m_root_module);
            }
        }
    }
    
    // 3. Module tree
    Expand_Mod(true , crate, modstack, ::AST::Path("",{}), crate.m_root_module);
    Expand_Mod(false, crate, modstack, ::AST::Path("",{}), crate.m_root_module);
    
    // Post-process
    #if 0
    for( auto& a : crate.m_attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( d.first == a.name() && d.second->expand_before_macros() == false ) {
                //d.second->handle(a, crate, ::AST::Path(), crate.m_root_module, crate.m_root_module);
            }
        }
    }
    for( auto& a : crate.m_attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( d.first == a.name() && d.second->expand_before_macros() == false ) {
                d.second->handle(a, crate);
            }
        }
    }
    #endif
}


