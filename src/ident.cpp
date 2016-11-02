/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/ident.cpp
 * - Identifiers with hygiene
 */
#include <iostream>
#include <ident.hpp>
#include <debug.hpp>
#include <common.hpp>   // vector print

unsigned int Ident::Hygiene::g_next_scope = 0;

bool Ident::Hygiene::is_visible(const Hygiene& src) const
{
    // HACK: Disable hygiene for now
    return true;
    //return this->scope_index == src.scope_index;
}

::std::ostream& operator<<(::std::ostream& os, const Ident& x) {
    os << x.name << x.hygiene;
    return os;
}

::std::ostream& operator<<(::std::ostream& os, const Ident::Hygiene& x) {
    os << "{" << x.scope_index << "}";
    return os;
}

