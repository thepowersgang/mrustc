/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/trans_list.hpp
 * - A list of items that require translation
 */
#pragma once

#include <hir/type.hpp>
#include <hir/path.hpp>

namespace HIR {
class Function;
class Static;
}

class TransList
{
    ::std::vector< ::std::pair<::HIR::Path, const ::HIR::Function*> > m_functions;
    ::std::vector< ::std::pair<::HIR::Path, const ::HIR::Static*> > m_statics;
public:
    bool add_function(::HIR::Path p, const ::HIR::Function& f);
    bool add_static(::HIR::Path p, const ::HIR::Static& s);
};

