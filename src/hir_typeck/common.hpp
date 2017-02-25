/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/common.hpp
 * - Typecheck common methods
 */
#pragma once

#include "impl_ref.hpp"
#include <hir/generic_params.hpp>
#include <hir/type.hpp>

// TODO/NOTE - This is identical to ::HIR::t_cb_resolve_type
typedef ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)>   t_cb_generic;

extern bool monomorphise_type_needed(const ::HIR::TypeRef& tpl);
extern bool monomorphise_pathparams_needed(const ::HIR::PathParams& tpl);
static inline bool monomorphise_genericpath_needed(const ::HIR::GenericPath& tpl) {
    return monomorphise_pathparams_needed(tpl.m_params);
}
extern bool monomorphise_path_needed(const ::HIR::Path& tpl);
extern bool monomorphise_traitpath_needed(const ::HIR::TraitPath& tpl);
extern bool monomorphise_type_needed(const ::HIR::TypeRef& tpl);
extern ::HIR::PathParams monomorphise_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_generic callback, bool allow_infer);
extern ::HIR::GenericPath monomorphise_genericpath_with(const Span& sp, const ::HIR::GenericPath& tpl, t_cb_generic callback, bool allow_infer);
extern ::HIR::TraitPath monomorphise_traitpath_with(const Span& sp, const ::HIR::TraitPath& tpl, t_cb_generic callback, bool allow_infer);
extern ::HIR::Path monomorphise_path_with(const Span& sp, const ::HIR::Path& tpl, t_cb_generic callback, bool allow_infer);
extern ::HIR::TypeRef monomorphise_type_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_generic callback, bool allow_infer=true);
extern ::HIR::TypeRef monomorphise_type(const Span& sp, const ::HIR::GenericParams& params_def, const ::HIR::PathParams& params,  const ::HIR::TypeRef& tpl);

typedef ::std::function<bool(const ::HIR::TypeRef&)> t_cb_visit_ty;
/// Calls the provided callback on every type seen when recursing the type.
/// If the callback returns `true`, no further types are visited and the function returns `true`.
extern bool visit_ty_with(const ::HIR::TypeRef& ty, t_cb_visit_ty callback);

typedef ::std::function<bool(const ::HIR::TypeRef&, ::HIR::TypeRef&)>   t_cb_clone_ty;
/// Clones a type, calling the provided callback on every type (optionally providing a replacement)
extern ::HIR::TypeRef clone_ty_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_clone_ty callback);

// Helper for passing a group of params around
struct MonomorphState
{
    const ::HIR::TypeRef*   self_ty;
    const ::HIR::PathParams*    pp_impl;
    const ::HIR::PathParams*    pp_method;

    ::HIR::PathParams   pp_impl_data;

    MonomorphState():
        self_ty(nullptr),
        pp_impl(nullptr),
        pp_method(nullptr)
    {
    }
    MonomorphState(MonomorphState&& x):
        MonomorphState()
    {
        *this = ::std::move(x);
    }

    MonomorphState& operator=(MonomorphState&& x) {
        this->self_ty = x.self_ty;
        this->pp_impl = (x.pp_impl == &x.pp_impl_data ? &this->pp_impl_data : x.pp_impl);
        this->pp_method = x.pp_method;
        this->pp_impl_data = ::std::move(x.pp_impl_data);
        return *this;
    }
    MonomorphState clone() const {
        MonomorphState  rv;
        rv.self_ty = this->self_ty;
        rv.pp_impl = (this->pp_impl == &this->pp_impl_data ? &rv.pp_impl_data : this->pp_impl);
        rv.pp_method = this->pp_method;
        rv.pp_impl_data = this->pp_impl_data.clone();
        return rv;
    }

    t_cb_generic    get_cb(const Span& sp) const;

    ::HIR::TypeRef  monomorph(const Span& sp, const ::HIR::TypeRef& ty, bool allow_infer=true) const {
        return monomorphise_type_with(sp, ty, this->get_cb(sp), allow_infer);
    }
    ::HIR::Path monomorph(const Span& sp, const ::HIR::Path& tpl, bool allow_infer=true) const {
        return monomorphise_path_with(sp, tpl, this->get_cb(sp), allow_infer);
    }
};
extern ::std::ostream& operator<<(::std::ostream& os, const MonomorphState& ms);

static inline t_cb_generic monomorphise_type_get_cb(const Span& sp, const ::HIR::TypeRef* self_ty, const ::HIR::PathParams* params_i, const ::HIR::PathParams* params_m, const ::HIR::PathParams* params_p=nullptr)
{
    return [=](const ::HIR::TypeRef& gt)->const ::HIR::TypeRef& {
        const auto& ge = gt.m_data.as_Generic();
        if( ge.binding == 0xFFFF ) {
            ASSERT_BUG(sp, self_ty, "Self wasn't expected here");
            return *self_ty;
        }
        else if( (ge.binding >> 8) == 0 ) {
            auto idx = ge.binding & 0xFF;
            ASSERT_BUG(sp, params_i, "Impl-level params were not expected - " << gt);
            ASSERT_BUG(sp, idx < params_i->m_types.size(), "Parameter out of range " << gt << " >= " << params_i->m_types.size());
            return params_i->m_types[idx];
        }
        else if( (ge.binding >> 8) == 1 ) {
            auto idx = ge.binding & 0xFF;
            ASSERT_BUG(sp, params_m, "Method-level params were not expected - " << gt);
            ASSERT_BUG(sp, idx < params_m->m_types.size(), "Parameter out of range " << gt << " >= " << params_m->m_types.size());
            return params_m->m_types[idx];
        }
        else if( (ge.binding >> 8) == 2 ) {
            auto idx = ge.binding & 0xFF;
            ASSERT_BUG(sp, params_p, "Placeholder params were not expected - " << gt);
            ASSERT_BUG(sp, idx < params_p->m_types.size(), "Parameter out of range " << gt << " >= " << params_p->m_types.size());
            return params_p->m_types[idx];
        }
        else {
            BUG(sp, "Invalid generic type - " << gt);
        }
        };
}

extern void check_type_class_primitive(const Span& sp, const ::HIR::TypeRef& type, ::HIR::InferClass ic, ::HIR::CoreType ct);
