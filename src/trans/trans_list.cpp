/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/trans_list.cpp
 * - A list of items that require translation
 */
#include "trans_list.hpp"

TransList_Function* TransList::add_function(::HIR::Path p)
{
    auto rv = m_functions.insert( ::std::make_pair(mv$(p), nullptr) );
    if( rv.second )
    {
        DEBUG("Function " << rv.first->first);
        assert( !rv.first->second );
        rv.first->second.reset( new TransList_Function {} );
        return &*rv.first->second;
    }
    else
    {
        return nullptr;
    }
}
TransList_Static* TransList::add_static(::HIR::Path p)
{
    auto rv = m_statics.insert( ::std::make_pair(mv$(p), nullptr) );
    if( rv.second )
    {
        DEBUG("Static " << rv.first->first);
        assert( !rv.first->second );
        rv.first->second.reset( new TransList_Static {} );
        return &*rv.first->second;
    }
    else
    {
        return nullptr;
    }
}
