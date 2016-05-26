/*
 * Evaluate constants
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>


namespace {
    ::HIR::Function& get_function(const Span& sp, const ::HIR::Crate& crate, const ::HIR::Path& path)
    {
        TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
        (Generic,
            TODO(sp, "get_function(path = " << path << ")");
            ),
        (UfcsInherent,
            // Easy (ish)
            TODO(sp, "get_function(path = " << path << ")");
            ),
        (UfcsKnown,
            TODO(sp, "get_function(path = " << path << ")");
            ),
        (UfcsUnknown,
            // TODO - Since this isn't known, can it be searched properly?
            TODO(sp, "get_function(path = " << path << ")");
            )
        )
        throw "";
    }
    
    ::HIR::Literal evaluate_constant(const ::HIR::Crate& crate, const ::HIR::ExprNode& expr)
    {
        struct Visitor:
            public ::HIR::ExprVisitor
        {
            const ::HIR::Crate& m_crate;
            
            ::HIR::Literal  m_rv;
            
            Visitor(const ::HIR::Crate& crate):
                m_crate(crate)
            {}
            
            void badnode(const ::HIR::ExprNode& node) const {
                ERROR(node.span(), E0000, "Node not allowed in constant expression");
            }
            
            void visit(::HIR::ExprNode_Block& node) override {
                TODO(node.span(), "ExprNode_Block");
            }
            void visit(::HIR::ExprNode_Return& node) override {
                TODO(node.span(), "ExprNode_Return");
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
                node.m_left->visit(*this);
                auto left = mv$(m_rv);
                node.m_right->visit(*this);
                auto right = mv$(m_rv);
                
                if( left.tag() != right.tag() ) {
                    ERROR(node.span(), E0000, "ExprNode_BinOp - Sides mismatched");
                }
                
                switch(node.m_op)
                {
                case ::HIR::ExprNode_BinOp::Op::CmpEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpNEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpLt:
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:
                case ::HIR::ExprNode_BinOp::Op::CmpGt:
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:
                    ERROR(node.span(), E0000, "ExprNode_BinOp - Comparisons");
                    break;
                case ::HIR::ExprNode_BinOp::Op::BoolAnd:
                case ::HIR::ExprNode_BinOp::Op::BoolOr:
                    ERROR(node.span(), E0000, "ExprNode_BinOp - Logicals");
                    break;

                case ::HIR::ExprNode_BinOp::Op::Add:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le + re); ),
                    (Float,     m_rv = ::HIR::Literal(le + re); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Sub:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le - re); ),
                    (Float,     m_rv = ::HIR::Literal(le - re); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Mul:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le * re); ),
                    (Float,     m_rv = ::HIR::Literal(le * re); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Div:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le / re); ),
                    (Float,     m_rv = ::HIR::Literal(le / re); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Mod:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le % re); ),
                    (Float,     ERROR(node.span(), E0000, "modulo operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::And:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le % re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise and operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Or:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le | re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise or operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Xor:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le ^ re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise xor operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Shr:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le >> re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise shift right operator on float in constant"); )
                    )
                    break;
                case ::HIR::ExprNode_BinOp::Op::Shl:
                    TU_MATCH_DEF(::HIR::Literal, (left, right), (le, re),
                    ( throw ""; ),
                    (Integer,   m_rv = ::HIR::Literal(le << re); ),
                    (Float,     ERROR(node.span(), E0000, "bitwise shift left operator on float in constant"); )
                    )
                    break;
                }
            }
            void visit(::HIR::ExprNode_UniOp& node) override {
                TODO(node.span(), "ExprNode_UniOp");
            }
            void visit(::HIR::ExprNode_Cast& node) override {
                TODO(node.span(), "ExprNode_Cast");
            }
            void visit(::HIR::ExprNode_Index& node) override {
                badnode(node);
            }
            void visit(::HIR::ExprNode_Deref& node) override {
                badnode(node);
            }
            
            void visit(::HIR::ExprNode_CallPath& node) override {
                ::std::vector<HIR::Literal> arg_vals;
                for(const auto& arg : node.m_args) {
                    arg->visit(*this);
                    arg_vals.push_back( mv$(m_rv) );
                }
                
                auto& fcn = get_function(node.span(), m_crate, node.m_path);
                if( ! fcn.m_const ) {
                    ERROR(node.span(), E0000, "Calling non-const function in const context");
                }
                TODO(node.span(), "exec const fn - " << node.m_path);
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
                    TODO(node.span(), "Byte literal in constant");
                    //m_rv = ::HIR::Literal::make_String(e);
                    )
                )
            }
            void visit(::HIR::ExprNode_PathValue& node) override {
                TODO(node.span(), "ExprNode_PathValue");
            }
            void visit(::HIR::ExprNode_Variable& node) override {
                TODO(node.span(), "ExprNode_Variable");
            }
            
            void visit(::HIR::ExprNode_StructLiteral& node) override {
                TODO(node.span(), "ExprNode_StructLiteral");
            }
            void visit(::HIR::ExprNode_Tuple& node) override {
                TODO(node.span(), "ExprNode_Tuple");
            }
            void visit(::HIR::ExprNode_ArrayList& node) override {
                TODO(node.span(), "ExprNode_ArrayList");
            }
            void visit(::HIR::ExprNode_ArraySized& node) override {
                TODO(node.span(), "ExprNode_ArraySized");
            }
            
            void visit(::HIR::ExprNode_Closure& node) override {
                badnode(node);
            }
        };
        
        Visitor v { crate };
        const_cast<::HIR::ExprNode&>(expr).visit(v);
        
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
                auto val = evaluate_constant(m_crate, *e.size);
                if( !val.is_Integer() )
                    ERROR(e.size->span(), E0000, "Array size isn't an integer");
                e.size_val = val.as_Integer();
                DEBUG("Array " << ty << " - size = " << e.size_val);
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }
        void visit_constant(::HIR::Constant& item) override
        {
            item.m_value_res = evaluate_constant(m_crate, *item.m_value);
            DEBUG("constant = " << item.m_value_res);
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
                        ERROR(node.span(), E0000, "Array size isn't an integer");
                    node.m_size_val = val.as_Integer();
                    DEBUG("Array literal [?; " << node.m_size_val << "]");
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
