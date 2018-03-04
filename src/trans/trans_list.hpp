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

class StaticTraitResolve;
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
    ::HIR::TypeRef monomorph(const ::StaticTraitResolve& resolve, const ::HIR::TypeRef& p) const;
    ::HIR::Path monomorph(const ::StaticTraitResolve& resolve, const ::HIR::Path& p) const;
    ::HIR::GenericPath monomorph(const ::StaticTraitResolve& resolve, const ::HIR::GenericPath& p) const;
    ::HIR::PathParams monomorph(const ::StaticTraitResolve& resolve, const ::HIR::PathParams& p) const;

    bool has_types() const {
        return pp_method.m_types.size() > 0 || pp_impl.m_types.size() > 0;
    }
};

struct CachedFunction {
    ::HIR::TypeRef  ret_ty;
    ::HIR::Function::args_t arg_tys;
    ::MIR::FunctionPointer  code;
};
struct TransList_Function
{
    const ::HIR::Function*  ptr;
    Trans_Params    pp;
    // If `pp.has_types` is true, the below is valid
    CachedFunction  monomorphised;
};
struct TransList_Static
{
    const ::HIR::Static*    ptr;
    Trans_Params    pp;
};

class TransList
{
public:
    TransList() = default;
    TransList(TransList&&) = default;
    TransList(const TransList&) = delete;
    TransList& operator=(TransList&&) = default;
    TransList& operator=(const TransList&) = delete;

    ::std::map< ::HIR::Path, ::std::unique_ptr<TransList_Function> > m_functions;
    ::std::map< ::HIR::Path, ::std::unique_ptr<TransList_Static> > m_statics;
    ::std::map< ::HIR::Path, Trans_Params> m_vtables;
    /// Required type_id values
    ::std::set< ::HIR::TypeRef> m_typeids;
    /// Required struct/enum constructor impls
    ::std::set< ::HIR::GenericPath> m_constructors;

    // .second is `true` if this is a from a reference to the type
    ::std::vector< ::std::pair<::HIR::TypeRef, bool> >  m_types;

    TransList_Function* add_function(::HIR::Path p);
    TransList_Static* add_static(::HIR::Path p);
    bool add_vtable(::HIR::Path p, Trans_Params pp) {
        return m_vtables.insert( ::std::make_pair( mv$(p), mv$(pp) ) ).second;
    }
};

