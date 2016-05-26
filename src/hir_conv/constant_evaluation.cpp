/*
 * Evaluate constants
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>


namespace {
    ::HIR::Literal evaluate_constant(const ::HIR::Crate& crate, ::HIR::ExprNode& expr)
    {
        struct Visitor:
            public ::HIR::ExprVisitor
        {
            const ::HIR::Crate& crate;
            
            ::HIR::Literal  m_rv;
            
            Visitor(const ::HIR::Crate& crate):
                crate(crate)
            {}
            
            void badnode(const ::HIR::ExprNode& node) const {
                ERROR(Span(), E0000, "Node not allowed in constant expression");
            }
            
            void visit(::HIR::ExprNode_Block& node) override {
                TODO(Span(), "ExprNode_Block");
            }
            void visit(::HIR::ExprNode_Return& node) override {
                TODO(Span(), "ExprNode_Return");
            }
            void visit(::HIR::ExprNode_Let& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_Loop& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_LoopControl& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_Match& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_If& node) override {
                badnode(node);
            }
            
            void visit(::HIR::ExprNode_Assign& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_BinOp& node) override {
                TODO(Span(), "ExprNode_BinOp");
            }
            void visit(::HIR::ExprNode_UniOp& node) override {
                TODO(Span(), "ExprNode_UniOp");
            }
            void visit(::HIR::ExprNode_Cast& node) override {
                TODO(Span(), "ExprNode_Cast");
            }
            void visit(::HIR::ExprNode_Index& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_Deref& node) override {
                badnode(node);
            }
            
            void visit(::HIR::ExprNode_CallPath& node) override {
                TODO(Span(), "exec const fn");
            }
            void visit(::HIR::ExprNode_CallValue& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_CallMethod& node) override {
                // TODO: const methods
                badnode(node);
            }
            void visit(::HIR::ExprNode_Field& node) override {
                badnode(node);
            }

            void visit(::HIR::ExprNode_Literal& node) override {
                TU_MATCH(::HIR::ExprNode_Literal::Data, (node.m_data), (e),
                (Integer,
                    m_rv = ::HIR::Literal(e.m_value);
                    ),
                (Float,
                    m_rv = ::HIR::Literal(e.m_value);
                    ),
                (Boolean,
                    m_rv = ::HIR::Literal(static_cast<uint64_t>(e));
                    ),
                (String,
                    m_rv = ::HIR::Literal(e);
                    ),
                (ByteString,
                    TODO(Span(), "Byte literal in constant");
                    //m_rv = ::HIR::Literal::make_String(e);
                    )
                )
            }
            void visit(::HIR::ExprNode_PathValue& node) override {
                TODO(Span(), "ExprNode_PathValue");
            }
            void visit(::HIR::ExprNode_Variable& node) override {
                TODO(Span(), "ExprNode_Variable");
            }
            
            void visit(::HIR::ExprNode_StructLiteral& node) override {
                TODO(Span(), "ExprNode_StructLiteral");
            }
            void visit(::HIR::ExprNode_Tuple& node) override {
                TODO(Span(), "ExprNode_Tuple");
            }
            void visit(::HIR::ExprNode_ArrayList& node) override {
                TODO(Span(), "ExprNode_ArrayList");
            }
            void visit(::HIR::ExprNode_ArraySized& node) override {
                TODO(Span(), "ExprNode_ArraySized");
            }
            
            void visit(::HIR::ExprNode_Closure& node) override {
                badnode(node);
            }
        };
        
        Visitor v { crate };
        expr.visit(v);
        
        if( v.m_rv.is_Invalid() ) {
            BUG(Span(), "Expression did not yeild a literal");
        }
        
        return mv$(v.m_rv);
    }

    class Expander:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;

    public:
        Expander(const ::HIR::Crate& crate):
            m_crate(crate)
        {}
        
        void visit_type(::HIR::TypeRef& ty)
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                ::HIR::Visitor::visit_type(*e.inner);
                assert(&*e.size != nullptr);
                auto size = evaluate_constant(m_crate, *e.size);
                TODO(Span(), "visit_type - Set array size to " << size << ", ty = " << ty);
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }
        void visit_expr(::HIR::ExprPtr& expr) override
        {
            struct Visitor:
                public ::HIR::ExprVisitorDef
            {
                const ::HIR::Crate& m_crate;
                
                Visitor(const ::HIR::Crate& crate):
                    m_crate(crate)
                {}
                
                void visit(::HIR::ExprNode_ArraySized& node) override {
                    auto val = evaluate_constant(m_crate, *node.m_size);
                    if( !val.is_Integer() )
                        ERROR(Span(), E0000, "Array size isn't an integer");
                    node.m_size_val = val.as_Integer();
                }
            };
            
            if( &*expr != nullptr )
            {
                Visitor v { this->m_crate };
                (*expr).visit(v);
            }
        }
    };
}   // namespace

void ConvertHIR_ConstantEvaluate(::HIR::Crate& crate)
{
    Expander    exp { crate };
    exp.visit_crate( crate );
}
