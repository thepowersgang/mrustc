/*
 */
#include <hir/expr.hpp>

::HIR::ExprNode::~ExprNode()
{
}

#define DEF_VISIT(nt, n, code)   void ::HIR::nt::visit(ExprVisitor& nv) { nv.visit_node(*this); nv.visit(*this); } void ::HIR::ExprVisitorDef::visit(::HIR::nt& n) { code }

void ::HIR::ExprVisitor::visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) {
    assert(node_ptr);
    node_ptr->visit(*this);
}
void ::HIR::ExprVisitor::visit_node(::HIR::ExprNode& node) {
}
DEF_VISIT(ExprNode_Block, node,
    for(auto& subnode : node.m_nodes) {
        visit_node_ptr(subnode);
    }
)
DEF_VISIT(ExprNode_Return, node,
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_Let, node,
    if( node.m_value ) {
        visit_node_ptr(node.m_value);
    }
)
DEF_VISIT(ExprNode_Loop, node,
    visit_node_ptr(node.m_code);
)
DEF_VISIT(ExprNode_LoopControl, , )
DEF_VISIT(ExprNode_Match, node,
    visit_node_ptr(node.m_value);
    for(auto& arm : node.m_arms)
    {
        if( arm.m_cond )
            visit_node_ptr(arm.m_cond);
        visit_node_ptr(arm.m_code);
    }
)
DEF_VISIT(ExprNode_If, node,
    visit_node_ptr(node.m_cond);
    visit_node_ptr(node.m_true);
    if( node.m_false )
        visit_node_ptr(node.m_false);
)

DEF_VISIT(ExprNode_Assign, node,
    visit_node_ptr(node.m_slot);
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_BinOp, node,
    visit_node_ptr(node.m_left);
    visit_node_ptr(node.m_right);
)
DEF_VISIT(ExprNode_UniOp, node,
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_Cast, node,
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_Unsize, node,
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_Index, node,
    visit_node_ptr(node.m_value);
    visit_node_ptr(node.m_index);
)
DEF_VISIT(ExprNode_Deref, node,
    visit_node_ptr(node.m_value);
)

DEF_VISIT(ExprNode_CallPath, node,
    for(auto& arg : node.m_args)
        visit_node_ptr(arg);
)
DEF_VISIT(ExprNode_CallValue, node,
    visit_node_ptr(node.m_value);
    for(auto& arg : node.m_args)
        visit_node_ptr(arg);
)
DEF_VISIT(ExprNode_CallMethod, node,
    visit_node_ptr(node.m_value);
    for(auto& arg : node.m_args)
        visit_node_ptr(arg);
)
DEF_VISIT(ExprNode_Field, node,
    visit_node_ptr(node.m_value);
)

DEF_VISIT(ExprNode_Literal, , )
DEF_VISIT(ExprNode_PathValue, , )
DEF_VISIT(ExprNode_Variable, , )
DEF_VISIT(ExprNode_StructLiteral, node,
    if( node.m_base_value )
        visit_node_ptr(node.m_base_value);
    for(auto& val : node.m_values)
        visit_node_ptr(val.second);
)
DEF_VISIT(ExprNode_Tuple, node,
    for(auto& val : node.m_vals)
        visit_node_ptr(val);
)
DEF_VISIT(ExprNode_ArrayList, node,
    for(auto& val : node.m_vals)
        visit_node_ptr(val);
)
DEF_VISIT(ExprNode_ArraySized, node,
    visit_node_ptr(node.m_val);
    visit_node_ptr(node.m_size);
)

DEF_VISIT(ExprNode_Closure, node,
    visit_node_ptr(node.m_code);
)

#undef DEF_VISIT

