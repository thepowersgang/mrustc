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
DEF_VISIT(ExprNode_Match)
DEF_VISIT(ExprNode_If)

DEF_VISIT(ExprNode_Assign)
DEF_VISIT(ExprNode_BinOp)
DEF_VISIT(ExprNode_UniOp)
DEF_VISIT(ExprNode_Cast)
DEF_VISIT(ExprNode_Index)
DEF_VISIT(ExprNode_Deref)

DEF_VISIT(ExprNode_CallPath)
DEF_VISIT(ExprNode_CallValue)
DEF_VISIT(ExprNode_CallMethod)
DEF_VISIT(ExprNode_Field)

DEF_VISIT(ExprNode_Literal)
DEF_VISIT(ExprNode_PathValue);
DEF_VISIT(ExprNode_Variable);
DEF_VISIT(ExprNode_StructLiteral);
DEF_VISIT(ExprNode_Tuple)
DEF_VISIT(ExprNode_ArrayList)
DEF_VISIT(ExprNode_ArraySized)

DEF_VISIT(ExprNode_Closure);

#undef DEF_VISIT

