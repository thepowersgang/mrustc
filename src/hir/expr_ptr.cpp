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
::std::unique_ptr< ::HIR::ExprNode> HIR::ExprPtr::into_unique()
{
    ::std::unique_ptr< ::HIR::ExprNode> rv( this->node );
    this->node = nullptr;
    return rv;
}
void ::HIR::ExprPtr::reset(::HIR::ExprNode* p)
{
    delete node;
    node = p;
}
