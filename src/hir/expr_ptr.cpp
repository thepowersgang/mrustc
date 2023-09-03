/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/expr_ptr.cpp
 * - HIR Expression
 */
#include <hir/expr_ptr.hpp>
#include <hir/expr.hpp>
#include <hir/expr_state.hpp>

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

::HIR::ExprStatePtr::ExprStatePtr(ExprState x):
    ptr(new ExprState( ::std::move(x) ))
{
}
::HIR::ExprStatePtr::~ExprStatePtr()
{
    delete ptr;
    ptr = nullptr;
}
::HIR::ExprStatePtr HIR::ExprStatePtr::clone() const
{
    auto rv = ::HIR::ExprStatePtr(::HIR::ExprState((*this)->m_module, (*this)->m_mod_path));
    rv->m_traits = (*this)->m_traits;
    rv->m_impl_generics = (*this)->m_impl_generics;
    rv->m_item_generics = (*this)->m_item_generics;
    rv->stage = (*this)->stage;
    return rv;
}


const Span& HIR::ExprPtr::span() const
{
    static Span static_sp;
    if( *this )
        return (*this)->span();
    return static_sp;
}
const ::MIR::Function* HIR::ExprPtr::get_mir_opt() const
{
    if(!this->m_mir)
        return nullptr;
    return &*this->m_mir;
}
const ::MIR::Function& HIR::ExprPtr::get_mir_or_error(const Span& sp) const
{
    if(!this->m_mir)
        BUG(sp, "No MIR");
    return *this->m_mir;
}
::MIR::Function& HIR::ExprPtr::get_mir_or_error_mut(const Span& sp)
{
    if(!this->m_mir)
        BUG(sp, "No MIR");
    return *this->m_mir;
}
const ::MIR::Function* HIR::ExprPtr::get_ext_mir() const
{
    if(this->node)
        return nullptr;
    if(!this->m_mir)
        return nullptr;
    return &*this->m_mir;
}
::MIR::Function* HIR::ExprPtr::get_ext_mir_mut()
{
    if(this->node)
        return nullptr;
    if(!this->m_mir)
        return nullptr;
    return &*this->m_mir;
}
void HIR::ExprPtr::set_mir(::MIR::FunctionPointer mir)
{
    assert( !this->m_mir );
    m_mir = ::std::move(mir);
    // Reset the HIR tree to be a placeholder node (thus freeing the backing memory)
    if( false && node )
    {
        auto sp = node->span();
        node = ExprPtrInner(::std::unique_ptr<HIR::ExprNode>(new ::HIR::ExprNode_Loop(
                        sp, "",
                        ::std::unique_ptr<HIR::ExprNode>(new ::HIR::ExprNode_Tuple(sp, {}))
                        )));
    }
}


