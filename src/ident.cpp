/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/ident.cpp
 * - Identifiers with hygine
 */
#include <iostream>
#include <ident.hpp>
#include <debug.hpp>
#include <common.hpp>   // vector print

bool Ident::Hygine::is_visible(const Hygine& src) const
{
    // HACK: Disable hygine for now
    return true;
    
    DEBUG("*this = " << *this << ", src=" << src);
    if( this->file_num != src.file_num ) {
        DEBUG("- Different file");
        return false;
    }
    
    // `this` is the item, `src` is the ident used to access it

    // If this is from a deeper point than the source, it won't work.
    if( this->indexes.size() > src.indexes.size() ) {
        DEBUG("- Not subset: len");
        return false;
    }
    
    // If this hygine list (barring the last) is a subset of the source
    for(unsigned int i = 0; i < this->indexes.size()-1 - 1; i ++)
    {
        if( this->indexes[i] != src.indexes[i] ) {
            DEBUG("- Not subset: " << i);
            return false;
        }
    }
    
    unsigned int end = this->indexes.size()-1-1;
    // Allow match if this ident is from before the addressing ident
    if( this->indexes[end] < src.indexes[end] ) {
        return true;
    }
    
    DEBUG("- Not before");
    return false;
}

::std::ostream& operator<<(::std::ostream& os, const Ident& x) {
    os << x.name;
    return os;
}

::std::ostream& operator<<(::std::ostream& os, const Ident::Hygine& x) {
    os << "{" << x.file_num << ": [" << x.indexes << "]}";
    return os;
}

