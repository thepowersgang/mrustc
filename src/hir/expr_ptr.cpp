/*
 */
#include <hir/expr_ptr.hpp>
#include <hir/expr.hpp>

::HIR::ExprPtr::ExprPtr():
    node(nullptr)
{
}
::HIR::ExprPtr::ExprPtr(::std::unique_ptr< ::HIR::ExprNode> v):
    node( v.release() )
{
}
::HIR::ExprPtr::~ExprPtr()
{
    delete node;
}
