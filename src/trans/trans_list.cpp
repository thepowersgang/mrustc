/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/trans_list.cpp
 * - A list of items that require translation
 */
#include "trans_list.hpp"

bool TransList::add_function(::HIR::Path p, const ::HIR::Function& f)
{
    auto rv = m_functions.insert( ::std::make_pair(mv$(p), &f) );
    if( rv.second )
    {
        DEBUG("Function " << rv.first->first);
    }
    return rv.second;
}
bool TransList::add_static(::HIR::Path p, const ::HIR::Static& f)
{
    auto rv = m_statics.insert( ::std::make_pair(mv$(p), &f) );
    if( rv.second )
    {
        DEBUG("Static " << rv.first->first);
    }
    return rv.second;
}
