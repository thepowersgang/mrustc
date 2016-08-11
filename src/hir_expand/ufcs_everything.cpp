/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/ufcs_everything.cpp
 * - HIR Expansion - All calls (methods, values, ...) as UFCS path calls
 */
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/static.hpp>
#include <algorithm>
#include "main_bindings.hpp"

namespace {
    inline HIR::ExprNodeP mk_exprnodep(HIR::ExprNode* en, ::HIR::TypeRef ty){ en->m_res_type = mv$(ty); return HIR::ExprNodeP(en); }
}
#define NEWNODE(TY, CLASS, ...)  mk_exprnodep(new HIR::ExprNode_##CLASS(__VA_ARGS__), TY)

namespace {
    
    class ExprVisitor_Mutate:
        public ::HIR::ExprVisitorDef
    {
        const ::HIR::Crate& m_crate;
        ::HIR::ExprNodeP    m_replacement;
        
    public:
        ExprVisitor_Mutate(const ::HIR::Crate& crate):
            m_crate(crate)
        {
        }
        void visit_node_ptr(::HIR::ExprPtr& root) {
            const auto& node_ref = *root;
            const char* node_ty = typeid(node_ref).name();
            TRACE_FUNCTION_FR(&*root << " " << node_ty << " : " << root->m_res_type, node_ty);
            root->visit(*this);
            if( m_replacement ) {
                root.reset( m_replacement.release() );
            }
        }
        
        void visit_node_ptr(::HIR::ExprNodeP& node) override {
            const auto& node_ref = *node;
            const char* node_ty = typeid(node_ref).name();
            TRACE_FUNCTION_FR(&*node << " " << node_ty << " : " << node->m_res_type, node_ty);
            assert( node );
            node->visit(*this);
            if( m_replacement ) {
                node = mv$(m_replacement);
            }
        }
        
        
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            const auto& sp = node.span();
            
            ::HIR::ExprVisitorDef::visit(node);
            
            ::HIR::PathParams   trait_args;
            {
                ::std::vector< ::HIR::TypeRef>  arg_types;
                // NOTE: In this case, m_arg_types is just the argument types
                for(const auto& arg_ty : node.m_arg_types)
                    arg_types.push_back(arg_ty.clone());
                trait_args.m_types.push_back( ::HIR::TypeRef(mv$(arg_types)) );
            }
            
            // TODO: You can call via &-ptrs, but that currently isn't handled in typeck
            TU_IFLET(::HIR::TypeRef::Data, node.m_value->m_res_type.m_data, Closure, e,
                if( node.m_trait_used == ::HIR::ExprNode_CallValue::TraitUsed::Unknown )
                {
                    // NOTE: Closure node still exists, and will do until MIR construction deletes the HIR
                    switch(e.node->m_class)
                    {
                    case ::HIR::ExprNode_Closure::Class::Unknown:
                        BUG(sp, "References an ::Unknown closure");
                    case ::HIR::ExprNode_Closure::Class::NoCapture:
                    case ::HIR::ExprNode_Closure::Class::Shared:
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Fn;
                        // TODO: Add borrow.
                        break;
                    case ::HIR::ExprNode_Closure::Class::Mut:
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::FnMut;
                        // TODO: Add borrow.
                        break;
                    case ::HIR::ExprNode_Closure::Class::Once:
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::FnOnce;
                        // TODO: Add borrow.
                        break;
                    }
                }
            )
            
            // Use marking in node to determine trait to use
            ::HIR::Path   method_path(::HIR::SimplePath{});
            switch(node.m_trait_used)
            {
            case ::HIR::ExprNode_CallValue::TraitUsed::Fn:
                method_path = ::HIR::Path(
                    node.m_value->m_res_type.clone(),
                    ::HIR::GenericPath( m_crate.get_lang_item_path(sp, "fn"), mv$(trait_args) ),
                    "call"
                    );
                break;
            case ::HIR::ExprNode_CallValue::TraitUsed::FnMut:
                method_path = ::HIR::Path(
                    node.m_value->m_res_type.clone(),
                    ::HIR::GenericPath( m_crate.get_lang_item_path(sp, "fn_mut"), mv$(trait_args) ),
                    "call_mut"
                    );
                break;
            case ::HIR::ExprNode_CallValue::TraitUsed::FnOnce:
                method_path = ::HIR::Path(
                    node.m_value->m_res_type.clone(),
                    ::HIR::GenericPath( m_crate.get_lang_item_path(sp, "fn_once"), mv$(trait_args) ),
                    "call_once"
                    );
                break;
            
            //case ::HIR::ExprNode_CallValue::TraitUsed::Unknown:
            default:
                BUG(node.span(), "Encountered CallValue with TraitUsed::Unknown, ty=" << node.m_value->m_res_type);
            }
            
            auto self_arg_type = node.m_value->m_res_type.clone();
            // Construct argument list for the output
            ::std::vector< ::HIR::ExprNodeP>    args;
            args.reserve( 1 + node.m_args.size() );
            args.push_back( mv$(node.m_value) );
            for(auto& arg : node.m_args)
                args.push_back( mv$(arg) );
            
            m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                mv$(method_path),
                mv$(args)
                );
            
            auto& arg_types = dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache.m_arg_types;
            arg_types.push_back( mv$(self_arg_type) );
            for(auto& ty : node.m_arg_types)
                arg_types.push_back( mv$(ty) );
        }
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            const auto& sp = node.span();
            
            ::HIR::ExprVisitorDef::visit(node);
            
            ::std::vector< ::HIR::ExprNodeP>    args;
            args.reserve( 1 + node.m_args.size() );
            args.push_back( mv$(node.m_value) );
            for(auto& arg : node.m_args)
                args.push_back( mv$(arg) );
            
            // Replace using known function path
            m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                mv$(node.m_method_path),
                mv$(args)
                );
            dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache = mv$(node.m_cache);
        }
    };
    class OuterVisitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_crate(crate)
        {
        }
        
        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                DEBUG("Array size " << ty);
                if( e.size ) {
                    ExprVisitor_Mutate  ev(m_crate);
                    ev.visit_node_ptr( e.size );
                }
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }
        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            //auto _ = this->m_ms.set_item_generics(item.m_params);
            if( item.m_code )
            {
                DEBUG("Function code " << p);
                ExprVisitor_Mutate  ev(m_crate);
                ev.visit_node_ptr( item.m_code );
            }
            else
            {
                DEBUG("Function code " << p << " (none)");
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            if( item.m_value )
            {
                ExprVisitor_Mutate  ev(m_crate);
                ev.visit_node_ptr(item.m_value);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                ExprVisitor_Mutate  ev(m_crate);
                ev.visit_node_ptr(item.m_value);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    DEBUG("Enum value " << p << " - " << var.first);
                    
                    ExprVisitor_Mutate  ev(m_crate);
                    ev.visit_node_ptr(e);
                )
            }
        }
    };
}   // namespace

void HIR_Expand_UfcsEverything(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

