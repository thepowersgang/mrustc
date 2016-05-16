/*
 */
#include <hir/expr.hpp>

::HIR::ExprNode::~ExprNode()
{
}

#define DEF_VISIT(nt)   void ::HIR::nt::visit(ExprVisitor& nv) { nv.visit(*this); }

DEF_VISIT(ExprNode_Block)
DEF_VISIT(ExprNode_Return)
DEF_VISIT(ExprNode_Let)
DEF_VISIT(ExprNode_Loop)
DEF_VISIT(ExprNode_LoopControl)
DEF_VISIT(ExprNode_Assign)
DEF_VISIT(ExprNode_BinOp)
DEF_VISIT(ExprNode_UniOp)
DEF_VISIT(ExprNode_Cast)

DEF_VISIT(ExprNode_CallPath)
DEF_VISIT(ExprNode_CallMethod)

DEF_VISIT(ExprNode_Literal)
DEF_VISIT(ExprNode_PathValue);
DEF_VISIT(ExprNode_Variable);

#undef DEF_VISIT

