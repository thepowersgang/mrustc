/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/annotate_value_usage.cpp
 * - Marks _Variable, _Index, _Deref, and _Field nodes with how the result is used
 */
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/static.hpp>
#include <algorithm>
#include "main_bindings.hpp"

namespace {
    
    class ExprVisitor_Mark:
        public ::HIR::ExprVisitor//Def
    {
        const StaticTraitResolve&    m_resolve;
        ::std::vector< ::HIR::ValueUsage>   m_usage;
        
        struct UsageGuard
        {
            ExprVisitor_Mark& m_parent;
            bool    m_pop;
            UsageGuard(ExprVisitor_Mark& parent, bool pop):
                m_parent(parent),
                m_pop(pop)
            {
            }
            ~UsageGuard()
            {
                if(m_pop) {
                    m_parent.m_usage.pop_back();
                }
            }
        };
        
        ::HIR::ValueUsage get_usage() const {
            return (m_usage.empty() ? ::HIR::ValueUsage::Move : m_usage.back());
        }
        UsageGuard push_usage(::HIR::ValueUsage u) {
            if( get_usage() == u ) {
                return UsageGuard(*this, false);
            }
            else {
                m_usage.push_back( u );
                return UsageGuard(*this, true);
            }
        }
        
    public:
        ExprVisitor_Mark(const StaticTraitResolve& resolve):
            m_resolve(resolve)
        {}
        
        void visit_root(::HIR::ExprPtr& root_ptr)
        {
            assert(root_ptr);
            root_ptr->m_usage = this->get_usage();
            auto expected_size = m_usage.size();
            root_ptr->visit( *this );
            assert( m_usage.size() == expected_size );
        }
        void visit_node_ptr(::HIR::ExprNodeP& node_ptr) override
        {
            assert(node_ptr);
            
            const auto& node_ref = *node_ptr;
            const char* node_tyname = typeid(node_ref).name();
            TRACE_FUNCTION_FR(&*node_ptr << " " << node_tyname, node_ptr->m_usage);
            
            node_ptr->m_usage = this->get_usage();
            
            auto expected_size = m_usage.size();
            node_ptr->visit( *this );
            assert( m_usage.size() == expected_size );
        }
        
        void visit(::HIR::ExprNode_Block& node) override
        {
            auto _ = this->push_usage( ::HIR::ValueUsage::Move );
            
            for( auto& subnode : node.m_nodes ) {
                this->visit_node_ptr(subnode);
            }
        }
        
        void visit(::HIR::ExprNode_Return& node) override
        {
            auto _ = this->push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr( node.m_value );
        }
        void visit(::HIR::ExprNode_Let& node) override
        {
            if( node.m_value )
            {
                auto _ = this->push_usage( this->get_usage_for_pattern(node.m_pattern, node.m_type) );
                this->visit_node_ptr( node.m_value );
            }
        }
        void visit(::HIR::ExprNode_Loop& node) override
        {
            auto _ = this->push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr( node.m_code );
        }
        void visit(::HIR::ExprNode_LoopControl& node) override
        {
            // NOTE: Leaf
        }
        void visit(::HIR::ExprNode_Match& node) override
        {
            {
                const auto& val_ty = node.m_value->m_res_type;
                ::HIR::ValueUsage   vu = ::HIR::ValueUsage::Unknown;
                for( const auto& arm : node.m_arms )
                {
                    for( const auto& pat : arm.m_patterns ) 
                        vu = ::std::max( vu, this->get_usage_for_pattern(pat, val_ty) );
                }
                auto _ = this->push_usage( vu );
                this->visit_node_ptr( node.m_value );
            }
            
            auto _ = this->push_usage( ::HIR::ValueUsage::Move );
            for(auto& arm : node.m_arms)
            {
                if( arm.m_cond ) {
                    this->visit_node_ptr( arm.m_cond );
                }
                this->visit_node_ptr( arm.m_code );
            }
        }
        void visit(::HIR::ExprNode_If& node) override
        {
            auto _ = this->push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr( node.m_cond );
            this->visit_node_ptr( node.m_true );
            if( node.m_false ) {
                this->visit_node_ptr( node.m_false );
            }
        }
        
        void visit(::HIR::ExprNode_Assign& node) override
        {
            {
                auto _ = this->push_usage( ::HIR::ValueUsage::Mutate );
                this->visit_node_ptr(node.m_slot);
            }
            {
                auto _ = this->push_usage( ::HIR::ValueUsage::Move );
                this->visit_node_ptr(node.m_value);
            }
        }
        void visit(::HIR::ExprNode_UniOp& node) override
        {
            m_usage.push_back( ::HIR::ValueUsage::Move );
            
            this->visit_node_ptr(node.m_value);
            
            m_usage.pop_back();
        }
        void visit(::HIR::ExprNode_Borrow& node) override
        {
            switch(node.m_type)
            {
            case ::HIR::BorrowType::Shared:
                m_usage.push_back( ::HIR::ValueUsage::Borrow );
                break;
            case ::HIR::BorrowType::Unique:
                m_usage.push_back( ::HIR::ValueUsage::Mutate );
                break;
            case ::HIR::BorrowType::Owned:
                m_usage.push_back( ::HIR::ValueUsage::Move );
                break;
            }
            
            this->visit_node_ptr(node.m_value);
            
            m_usage.pop_back();
        }
        
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            switch(node.m_op)
            {
            case ::HIR::ExprNode_BinOp::Op::CmpEqu:
            case ::HIR::ExprNode_BinOp::Op::CmpNEqu:
            case ::HIR::ExprNode_BinOp::Op::CmpLt:
            case ::HIR::ExprNode_BinOp::Op::CmpLtE:
            case ::HIR::ExprNode_BinOp::Op::CmpGt:
            case ::HIR::ExprNode_BinOp::Op::CmpGtE:
                m_usage.push_back( ::HIR::ValueUsage::Borrow );
                break;
            default:
                m_usage.push_back( ::HIR::ValueUsage::Move );
                break;
            }
            
            this->visit_node_ptr(node.m_left);
            this->visit_node_ptr(node.m_right);
            
            m_usage.pop_back();
        }
        void visit(::HIR::ExprNode_Cast& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr(node.m_value);
        }
        void visit(::HIR::ExprNode_Unsize& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr(node.m_value);
        }
        void visit(::HIR::ExprNode_Index& node) override
        {
            // TODO: Override to ::Borrow if Res: Copy and moving
            if( this->get_usage() == ::HIR::ValueUsage::Move && type_is_copy(node.m_res_type) ) {
                auto _ = push_usage( ::HIR::ValueUsage::Borrow );
                this->visit_node_ptr(node.m_value);
            }
            else {
                this->visit_node_ptr(node.m_value);
            }
            
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr(node.m_index);
        }
        void visit(::HIR::ExprNode_Deref& node) override
        {
            if( this->get_usage() == ::HIR::ValueUsage::Move && type_is_copy(node.m_res_type) ) {
                auto _ = push_usage( ::HIR::ValueUsage::Borrow );
                this->visit_node_ptr(node.m_value);
            }
            else {
                this->visit_node_ptr(node.m_value);
            }
        }
        
        void visit(::HIR::ExprNode_Field& node) override
        {
            // If taking this field by value, but the type is Copy - pretend it's a borrow.
            if( this->get_usage() == ::HIR::ValueUsage::Move && type_is_copy(node.m_res_type) ) {
                auto _ = push_usage( ::HIR::ValueUsage::Borrow );
                this->visit_node_ptr(node.m_value);
            }
            else {
                this->visit_node_ptr(node.m_value);
            }
        }
        
        void visit(::HIR::ExprNode_TupleVariant& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            
            for( auto& val : node.m_args )
                this->visit_node_ptr(val);
        }
        void visit(::HIR::ExprNode_CallPath& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            
            for( auto& val : node.m_args )
                this->visit_node_ptr(val);
        }
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            
            this->visit_node_ptr(node.m_value);
            for( auto& val : node.m_args )
                this->visit_node_ptr(val);
        }
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            
            this->visit_node_ptr(node.m_value);
            for( auto& val : node.m_args )
                this->visit_node_ptr(val);
        }
        
        void visit(::HIR::ExprNode_Literal& node) override
        {
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override
        {
        }
        void visit(::HIR::ExprNode_PathValue& node) override
        {
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
        }

        void visit(::HIR::ExprNode_StructLiteral& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            
            if( node.m_base_value ) {
                this->visit_node_ptr(node.m_base_value);
            }
            for( auto& fld_val : node.m_values ) {
                this->visit_node_ptr(fld_val.second);
            }
        }
        void visit(::HIR::ExprNode_Tuple& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            for( auto& val : node.m_vals ) {
                this->visit_node_ptr(val);
            }
        }
        void visit(::HIR::ExprNode_ArrayList& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            for( auto& val : node.m_vals ) {
                this->visit_node_ptr(val);
            }
        }
        void visit(::HIR::ExprNode_ArraySized& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr(node.m_val);
        }
        
        void visit(::HIR::ExprNode_Closure& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr(node.m_code);
        }
        
    private:
        ::HIR::ValueUsage get_usage_for_pattern( const ::HIR::Pattern& pat, const ::HIR::TypeRef& ty ) const
        {
            // TODO: Detect if a by-value binding exists for a non-Copy type
            return ::HIR::ValueUsage::Move;
        }
        
        bool type_is_copy(const ::HIR::TypeRef& ty) const
        {
            return m_resolve.type_is_copy(ty);
        }
    };

    
    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve   m_resolve;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_resolve(crate)
        {}
        
        void visit_expr(::HIR::ExprPtr& exp) override {
            if( exp )
            {
                ExprVisitor_Mark    ev { m_resolve };
                ev.visit_root( exp );
            }
        }
        
        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            DEBUG("Function " << p);
            ::HIR::Visitor::visit_function(p, item);
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            // NOTE: No generics
            ::HIR::Visitor::visit_static(p, item);
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            // NOTE: No generics
            ::HIR::Visitor::visit_constant(p, item);
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_enum(p, item);
        }
        
        
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override {
            auto _ = this->m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }
        
        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            
            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
    };
}

void HIR_Expand_AnnotateUsage(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}
