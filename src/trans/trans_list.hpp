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
class Crate;
class Function;
class Static;
}

struct Trans_Params
{
    Span    sp;
    ::HIR::PathParams   pp_method;
    ::HIR::PathParams   pp_impl;
    ::HIR::TypeRef  self_type;
    
    Trans_Params() {}
    Trans_Params(const Span& sp):
        sp(sp)
    {}
    
    t_cb_generic get_cb() const;
    ::HIR::TypeRef monomorph(const ::HIR::Crate& crate, const ::HIR::TypeRef& p) const;
    ::HIR::Path monomorph(const ::HIR::Crate& crate, const ::HIR::Path& p) const;
    ::HIR::GenericPath monomorph(const ::HIR::Crate& crate, const ::HIR::GenericPath& p) const;
    
    bool has_types() const {
        return pp_method.m_types.size() > 0 || pp_impl.m_types.size() > 0;
    }
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
public:
    ::std::map< ::HIR::Path, ::std::unique_ptr<TransList_Function> > m_functions;
    ::std::map< ::HIR::Path, ::std::unique_ptr<TransList_Static> > m_statics;

    TransList_Function* add_function(::HIR::Path p);
    TransList_Static* add_static(::HIR::Path p);
};

