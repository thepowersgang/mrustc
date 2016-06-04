/*
 */
#include <hir/expr.hpp>

::HIR::ExprNode::~ExprNode()
{
}
const Span& ::HIR::ExprNode::span() const
{
    static Span rv = Span();
    return rv;
}

#define DEF_VISIT(nt, n, code)   void ::HIR::nt::visit(ExprVisitor& nv) { nv.visit_node(*this); nv.visit(*this); } void ::HIR::ExprVisitorDef::visit(::HIR::nt& n) { code }

void ::HIR::ExprVisitor::visit_node(::HIR::ExprNode& node) {
}
DEF_VISIT(ExprNode_Block, node,
    for(const auto& subnode : node.m_nodes) {
        assert(subnode);
        subnode->visit(*this);
    }
)
DEF_VISIT(ExprNode_Return, node,
    assert(node.m_value);
    node.m_value->visit(*this);
)
DEF_VISIT(ExprNode_Let, node,
    if( node.m_value ) {
        node.m_value->visit(*this);
    }
)
DEF_VISIT(ExprNode_Loop, node,
    assert(node.m_code);
    node.m_code->visit(*this);
)
DEF_VISIT(ExprNode_LoopControl, , )
DEF_VISIT(ExprNode_Match, node,
    assert(node.m_value);
    node.m_value->visit(*this);
    for(auto& arm : node.m_arms)
    {
        if( arm.m_cond )
            arm.m_cond->visit(*this);
        assert(arm.m_code);
        arm.m_code->visit(*this);
    }
)
DEF_VISIT(ExprNode_If, node,
    node.m_cond->visit(*this);
    node.m_true->visit(*this);
    if( node.m_false )
        node.m_false->visit(*this);
)

DEF_VISIT(ExprNode_Assign, node,
    node.m_slot->visit(*this);
    node.m_value->visit(*this);
)
DEF_VISIT(ExprNode_BinOp, node,
    node.m_left->visit(*this);
    node.m_right->visit(*this);
)
DEF_VISIT(ExprNode_UniOp, node,
    node.m_value->visit(*this);
)
DEF_VISIT(ExprNode_Cast, node,
    node.m_value->visit(*this);
)
DEF_VISIT(ExprNode_Unsize, node,
    node.m_value->visit(*this);
)
DEF_VISIT(ExprNode_Index, node,
    node.m_val->visit(*this);
    node.m_index->visit(*this);
)
DEF_VISIT(ExprNode_Deref, node,
    node.m_val->visit(*this);
)

DEF_VISIT(ExprNode_CallPath, node,
    for(auto& arg : node.m_args)
        arg->visit(*this);
)
DEF_VISIT(ExprNode_CallValue, node,
    node.m_val->visit(*this);
    for(auto& arg : node.m_args)
        arg->visit(*this);
)
DEF_VISIT(ExprNode_CallMethod, node,
    node.m_val->visit(*this);
    for(auto& arg : node.m_args)
        arg->visit(*this);
)
DEF_VISIT(ExprNode_Field, node,
    node.m_val->visit(*this);
)

DEF_VISIT(ExprNode_Literal, , )
DEF_VISIT(ExprNode_PathValue, , )
DEF_VISIT(ExprNode_Variable, , )
DEF_VISIT(ExprNode_StructLiteral, node,
    if( node.m_base_value )
        node.m_base_value->visit(*this);
    for(auto& val : node.m_values)
        val.second->visit( *this );
)
DEF_VISIT(ExprNode_Tuple, node,
    for(auto& val : node.m_vals)
        val->visit( *this );
)
DEF_VISIT(ExprNode_ArrayList, node,
    for(auto& val : node.m_vals)
        val->visit( *this );
)
DEF_VISIT(ExprNode_ArraySized, node,
    node.m_val->visit( *this );
    node.m_size->visit( *this );
)

DEF_VISIT(ExprNode_Closure, node,
    node.m_code->visit( *this );
)

#undef DEF_VISIT

