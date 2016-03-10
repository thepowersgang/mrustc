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


extern bool check_cfg(const ::AST::MetaItem& mi);

::std::map< ::std::string, ::std::unique_ptr<ExpandDecorator> >  g_decorators;
::std::map< ::std::string, ::std::unique_ptr<ExpandProcMacro> >  g_macros;

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
            if( check_cfg(a.items().m_items.at(0)) ) {
                Expand_Attr(a.items().m_items.at(1), stage, f);
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
    bool is_early, LList<const AST::Module*> modstack, ::AST::Module& mod,
    Span mi_span, const ::std::string& name, const ::std::string& input_ident, const TokenTree& input_tt
    )
{
    for( const auto& m : g_macros )
    {
        if( name == m.first && m.second->expand_early() == is_early )
        {
            auto e = m.second->expand(mi_span, input_ident, input_tt, mod);
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

void Expand_Expr(bool is_early, ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Path item_path, AST::Expr& node)
{
    struct CExpandExpr:
        public ::AST::NodeVisitor
    {
        bool is_early;
        LList<const AST::Module*>   modstack;
        ::std::unique_ptr<::AST::ExprNode> replacement;
        
        CExpandExpr(bool is_early, LList<const AST::Module*> ms):
            is_early(is_early),
            modstack(ms)
        {
        }
        
        void visit(::std::unique_ptr<AST::ExprNode>& cnode) {
            if(cnode.get())
                Expand_Attrs(cnode->attrs(), stage_pre(is_early),  [&](const auto& d, const auto& a){ d.handle(a, cnode); });
            if(cnode.get())
            {
                cnode->visit(*this);
                if( this->replacement.get() ) {
                    cnode = mv$(this->replacement);
                }
            }
            
            if(cnode.get())
                Expand_Attrs(cnode->attrs(), stage_post(is_early),  [&](const auto& d, const auto& a){ d.handle(a, cnode); });
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
            auto ttl = Expand_Macro(
                is_early, modstack, *(::AST::Module*)(modstack.m_item),
                Span(node.get_pos()),
                node.m_name, node.m_ident, node.m_tokens
                );
            if( ttl.get() != nullptr && ttl->lookahead(0) != TOK_EOF )
            {
                // Reparse as expression / item
                auto newexpr = Parse_Expr0(*ttl);
                // Then call visit on it again
                this->visit(newexpr);
                // And schedule it to replace the previous
                replacement = mv$(newexpr);
            }
        }
        
        void visit(::AST::ExprNode_Block& node) override {
            this->visit_vector(node.m_nodes);
        }
        void visit(::AST::ExprNode_Flow& node) override {
            this->visit_nodelete(node, node.m_value);
        }
        void visit(::AST::ExprNode_LetBinding& node) override {
            // TODO: Pattern and type
            this->visit_nodelete(node, node.m_value);
        }
        void visit(::AST::ExprNode_Assign& node) override {
            this->visit_nodelete(node, node.m_slot);
            this->visit_nodelete(node, node.m_value);
        }
        void visit(::AST::ExprNode_CallPath& node) override {
            // TODO: path?
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
            // TODO: Arms
        }
        void visit(::AST::ExprNode_If& node) override {
            this->visit_nodelete(node, node.m_cond);
            this->visit_nodelete(node, node.m_true);
            this->visit_nodelete(node, node.m_false);   // TODO: Can the false branch be `#[cfg]`d off?
        }
        void visit(::AST::ExprNode_IfLet& node) override {
            // TODO: Pattern
            this->visit_nodelete(node, node.m_value);
            this->visit_nodelete(node, node.m_true);
            this->visit_nodelete(node, node.m_false);   // TODO: Can the false branch be `#[cfg]`d off?
        }
        void visit(::AST::ExprNode_Integer& node) override { }
        void visit(::AST::ExprNode_Float& node) override { }
        void visit(::AST::ExprNode_Bool& node) override { }
        void visit(::AST::ExprNode_String& node) override { }
        void visit(::AST::ExprNode_Closure& node) override {
            // TODO: Arg patterns and types
            // TODO: Return type
            this->visit_nodelete(node, node.m_code);
        }
        void visit(::AST::ExprNode_StructLiteral& node) override {
            this->visit_nodelete(node, node.m_base_value);
            // TODO: Values (with #[cfg] support)
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
            // TODO: Type
        }
        void visit(::AST::ExprNode_BinOp& node) override {
            this->visit_nodelete(node, node.m_left);
            this->visit_nodelete(node, node.m_right);
        }
        void visit(::AST::ExprNode_UniOp& node) override {
            this->visit_nodelete(node, node.m_value);
        }
    };

    auto visitor = CExpandExpr(is_early, modstack);
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
            
            auto ttl = Expand_Macro(is_early, modstack, mod, mi_owned.span(), mi_owned.name(), mi_owned.input_ident(), mi_owned.input_tt());

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
        
        Expand_Attrs(i.data.attrs, stage_pre(is_early),  crate, path, mod, i.data);
        
        TU_MATCH(::AST::Item, (i.data), (e),
        (None,
            // Skip, nothing
            ),
        (Module,
            LList<const AST::Module*>   sub_modstack(&modstack, &e.e);
            Expand_Mod(is_early, crate, sub_modstack, path, e.e);
            ),
        (Crate,
            // Skip, no recursion
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
            // TODO: Do type aliases require recursion?
            ),
        
        (Function,
            // TODO: Recurse into argument patterns + types
            Expand_Expr(is_early, crate, modstack, path, e.e.code());
            ),
        (Static,
            // Recurse into static values
            Expand_Expr(is_early, crate, modstack, path, e.e.value());
            )
        )
        
        Expand_Attrs(i.data.attrs, stage_post(is_early),  crate, path, mod, i.data);
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
    for( auto& a : crate.m_attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( d.first == a.name() && d.second->stage() == AttrStage::EarlyPre ) {
                d.second->handle(a, crate);
            }
        }
    }
    
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
    Expand_Mod(true , crate, modstack, ::AST::Path(), crate.m_root_module);
    Expand_Mod(false, crate, modstack, ::AST::Path(), crate.m_root_module);
    
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


