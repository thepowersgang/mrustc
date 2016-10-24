/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/mir_ptr.cpp
 * - Destructor for MIR function pointers (cold path code)
 */
#include "mir_ptr.hpp"
#include "mir.hpp"


void ::MIR::FunctionPointer::reset()
{
    if( this->ptr ) {
        delete this->ptr;
        this->ptr = nullptr;
    }
}

