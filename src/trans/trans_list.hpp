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
#include <hir_typeck/common.hpp>

namespace HIR {
class Function;
class Static;
}

struct Trans_Params
{
    Span    sp;
    ::HIR::PathParams   pp_method;
    ::HIR::PathParams   pp_impl;
    ::HIR::TypeRef  self_type;
    
    t_cb_generic get_cb() const;
    ::HIR::TypeRef monomorph(const ::HIR::TypeRef& p) const;
    ::HIR::Path monomorph(const ::HIR::Path& p) const;
};

struct TransList_Function
{
    const ::HIR::Function*  ptr;
    Trans_Params    pp;
};
struct TransList_Static
{
    const ::HIR::Static*    ptr;
    Trans_Params    pp;
};

class TransList
{
    ::std::map< ::HIR::Path, ::std::unique_ptr<TransList_Function> > m_functions;
    ::std::map< ::HIR::Path, ::std::unique_ptr<TransList_Static> > m_statics;
public:
    TransList_Function* add_function(::HIR::Path p);
    TransList_Static* add_static(::HIR::Path p);
};

