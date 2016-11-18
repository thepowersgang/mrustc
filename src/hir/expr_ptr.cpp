/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/expr_ptr.cpp
 * - HIR Expression
 */
#include <hir/expr_ptr.hpp>
#include <hir/expr.hpp>

::HIR::ExprPtr::ExprPtr(::std::unique_ptr< ::HIR::ExprNode> v):
    node( mv$(v) )
{
}
::std::unique_ptr< ::HIR::ExprNode> HIR::ExprPtr::into_unique()
{
    return node.into_unique();
}


::HIR::ExprPtrInner::ExprPtrInner(::std::unique_ptr< ::HIR::ExprNode> v):
    ptr( v.release() )
{
}
::HIR::ExprPtrInner::~ExprPtrInner()
{
    delete ptr;
}
::std::unique_ptr< ::HIR::ExprNode> HIR::ExprPtrInner::into_unique()
{
    ::std::unique_ptr< ::HIR::ExprNode> rv( this->ptr );
    this->ptr = nullptr;
    return rv;
}
