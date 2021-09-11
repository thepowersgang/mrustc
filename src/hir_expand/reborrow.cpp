/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/reborrow.cpp
 * - Insert reborrows when a &mut would be moved
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

            // 1. Convert into an ExprNodeP
            auto np = root.into_unique();
            // 2. Pass to do_reborrow
            np = do_reborrow(mv$(np));
            // 3. Convert back
            root.reset( np.release() );
        }

        void visit_node_ptr(::HIR::ExprNodeP& node) override {
            const auto& node_ref = *node;
            const char* node_ty = typeid(node_ref).name();
            TRACE_FUNCTION_FR(&*node << " " << node_ty << " : " << node->m_res_type, node_ty);
            assert( node );
            node->visit(*this);
        }

        ::HIR::ExprNodeP do_reborrow(::HIR::ExprNodeP node_ptr)
        {
            assert(node_ptr);
            if(const auto* e = node_ptr->m_res_type.data().opt_Borrow())
            {
                if( e->type == ::HIR::BorrowType::Unique )
                {
                    if( dynamic_cast< ::HIR::ExprNode_Index*>(node_ptr.get())
                     || dynamic_cast< ::HIR::ExprNode_Variable*>(node_ptr.get())
                     || dynamic_cast< ::HIR::ExprNode_Field*>(node_ptr.get())
                     || dynamic_cast< ::HIR::ExprNode_Deref*>(node_ptr.get())
                        )
                    {
                        DEBUG("Insert reborrow - " << node_ptr->span() << " - type=" << node_ptr->m_res_type);
                        auto sp = node_ptr->span();
                        auto ty_mut = node_ptr->m_res_type.clone();
                        auto ty = e->inner.clone();
                        node_ptr = NEWNODE(mv$(ty_mut), Borrow, sp, ::HIR::BorrowType::Unique,
                            NEWNODE(mv$(ty), Deref, sp,  mv$(node_ptr))
                            );
                    }
                    // Recurse into blocks - Neater this way
                    else if( auto p = dynamic_cast< ::HIR::ExprNode_Block*>(node_ptr.get()) )
                    {
                        if( p->m_value_node )
                        {
                            p->m_value_node = do_reborrow(mv$(p->m_value_node));
                        }
                        else
                        {
                            const auto* node = node_ptr.get();
                            DEBUG("Node " << node << " is a non-yielding block");
                        }
                    }
                    else
                    {
                        // Not a node that should have reborrow applied (likely generated an owned &mut)
                        const auto* node = node_ptr.get();
                        DEBUG("Node " << node << " " << typeid(*node).name() << " cannot have a reborrow");
                    }
                }
            }
            return node_ptr;
        }

        void visit(::HIR::ExprNode_Cast& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            node.m_value = do_reborrow(mv$(node.m_value));
        }
        void visit(::HIR::ExprNode_Emplace& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            node.m_value = do_reborrow(mv$(node.m_value));
        }
        void visit(::HIR::ExprNode_Assign& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            node.m_value = do_reborrow(mv$(node.m_value));
        }
        void visit(::HIR::ExprNode_CallPath& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            for(auto& arg : node.m_args) {
                arg = do_reborrow(mv$(arg));
            }
        }
        void visit(::HIR::ExprNode_CallValue& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            for(auto& arg : node.m_args) {
                arg = do_reborrow(mv$(arg));
            }
        }
        void visit(::HIR::ExprNode_CallMethod& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            for(auto& arg : node.m_args) {
                arg = do_reborrow(mv$(arg));
            }
        }

        void visit(::HIR::ExprNode_ArrayList& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            for(auto& arg : node.m_vals) {
                arg = do_reborrow(mv$(arg));
            }
        }
        void visit(::HIR::ExprNode_Tuple& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            for(auto& arg : node.m_vals) {
                arg = do_reborrow(mv$(arg));
            }
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            for(auto& arg : node.m_args) {
                arg = do_reborrow(mv$(arg));
            }
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            for(auto& arg : node.m_values) {
                arg.second = do_reborrow(mv$(arg.second));
            }
        }
        void visit(::HIR::ExprNode_Unsize& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            node.m_value = do_reborrow(mv$(node.m_value));
        }
        void visit(::HIR::ExprNode_Closure& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            for(auto& arg : node.m_captures) {
                arg = do_reborrow(mv$(arg));
            }
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
            if(auto* e = ty.data_mut().opt_Array())
            {
                this->visit_type( e->inner );
                DEBUG("Array size " << ty);
                if( auto* cg = e->size.opt_Unevaluated() ) {
                    ExprVisitor_Mutate  ev(m_crate);
                    if(cg->is_Unevaluated())
                        ev.visit_node_ptr( *cg->as_Unevaluated() );
                }
            }
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
            if(auto* e = item.m_data.opt_Value())
            {
                for(auto& var : e->variants)
                {
                    DEBUG("Enum value " << p << " - " << var.name);

                    if( var.expr )
                    {
                        ExprVisitor_Mutate  ev(m_crate);
                        ev.visit_node_ptr(var.expr);
                    }
                }
            }
        }
    };
}   // namespace

void HIR_Expand_Reborrows_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp)
{
    TRACE_FUNCTION;
    ExprVisitor_Mutate  ev(crate);
    ev.visit_node_ptr( exp );
}
void HIR_Expand_Reborrows(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}
