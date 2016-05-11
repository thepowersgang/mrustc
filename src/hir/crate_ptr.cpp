/*
 */
#include "crate_ptr.hpp"
#include "hir.hpp"

::HIR::CratePtr::CratePtr():
    m_ptr(nullptr)
{
}
::HIR::CratePtr::CratePtr(HIR::Crate c):
    m_ptr( new ::HIR::Crate(mv$(c)) )
{
}
::HIR::CratePtr::~CratePtr()
{
    if( m_ptr ) {
        delete m_ptr, m_ptr = nullptr;
    }
}

