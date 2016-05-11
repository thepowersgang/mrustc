/*
 * High-level intermediate representation
 */
#pragma once

namespace HIR {

class Crate;

class CratePtr
{
    Crate*  m_ptr;
    
public:
    CratePtr();
    CratePtr(Crate c);
    CratePtr(CratePtr&&) = default;
    CratePtr& operator=(CratePtr&&) = default;
    ~CratePtr();
};

}   // namespace HIR

