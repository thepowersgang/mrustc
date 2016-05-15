/*
 */
#include <hir/expr_ptr.hpp>
#include <hir/expr.hpp>
#include <ast/expr.hpp>
#include <ast/ast.hpp>


struct Visitor:
    public ::AST::NodeVisitor
{
};

::HIR::ExprPtr LowerHIR_ExprNode(const ::AST::ExprNode& e)
{
    //Visitor v;
    //const_cast<::AST::ExprNode*>(&e)->visit( v );
    throw ::std::runtime_error("TODO: LowerHIR_ExprNode");
}
