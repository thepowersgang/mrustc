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
#include <hir_typeck/monomorph.hpp>


typedef ::std::function<bool(const ::HIR::TypeRef&)> t_cb_visit_ty;
/// Calls the provided callback on every type seen when recursing the type.
/// If the callback returns `true`, no further types are visited and the function returns `true`.
extern bool visit_ty_with(const ::HIR::TypeRef& ty, t_cb_visit_ty callback);
extern bool visit_path_tys_with(const ::HIR::Path& ty, t_cb_visit_ty callback);
typedef ::std::function<bool(const ::HIR::TypeRef&)> t_cb_visit_ty_mut;

extern bool visit_ty_with_mut(::HIR::TypeRef& ty, t_cb_visit_ty_mut callback);
extern bool visit_path_tys_with_mut(::HIR::Path& ty, t_cb_visit_ty_mut callback);

typedef ::std::function<bool(const ::HIR::TypeRef&, ::HIR::TypeRef&)>   t_cb_clone_ty;
/// Clones a type, calling the provided callback on every type (optionally providing a replacement)
extern ::HIR::TypeRef clone_ty_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_clone_ty callback);
extern ::HIR::PathParams clone_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_clone_ty callback);

extern void check_type_class_primitive(const Span& sp, const ::HIR::TypeRef& type, ::HIR::InferClass ic, ::HIR::CoreType ct);

class StaticTraitResolve;
extern void Typecheck_Expressions_ValidateOne(const StaticTraitResolve& resolve, const ::std::vector<::std::pair< ::HIR::Pattern, ::HIR::TypeRef>>& args, const ::HIR::TypeRef& ret_ty, const ::HIR::ExprPtr& code);



template<typename I>
struct WConst {
    typedef const I T;
};
//template<typename I>
//struct WMut {
//    typedef I T;
//};
template<template<typename> class W>
struct TyVisitor
{
    bool visit_path_params(typename W<::HIR::PathParams>::T& tpl)
    {
        for(const auto& ty : tpl.m_types)
            if( visit_type(ty) )
                return true;
        return false;
    }

    virtual bool visit_trait_path(typename W<::HIR::TraitPath>::T& tpl)
    {
        if( visit_path_params(tpl.m_path.m_params) )
            return true;
        for(const auto& assoc : tpl.m_type_bounds)
            if( visit_type(assoc.second) )
                return true;
        return false;
    }
    virtual bool visit_path(typename W<HIR::Path>::T& path)
    {
        TU_MATCH_HDRA((path.m_data), {)
        TU_ARMA(Generic, e) {
            return visit_path_params(e.m_params);
            }
        TU_ARMA(UfcsInherent, e) {
            return visit_type(e.type) || visit_path_params(e.params);
            }
        TU_ARMA(UfcsKnown, e) {
            return visit_type(e.type) || visit_path_params(e.trait.m_params) || visit_path_params(e.params);
            }
        TU_ARMA(UfcsUnknown, e) {
            return visit_type(e.type) || visit_path_params(e.params);
            }
        }
        throw "";
    }
    virtual bool visit_type(typename W<HIR::TypeRef>::T& ty)
    {
        TU_MATCH_HDRA( (ty.data()), {)
        TU_ARMA(Infer, e) {
            }
        TU_ARMA(Diverge, e) {
            }
        TU_ARMA(Primitive, e) {
            }
        TU_ARMA(Generic, e) {
            }
        TU_ARMA(Path, e) {
            return visit_path(e.path);
            }
        TU_ARMA(TraitObject, e) {
            if( visit_trait_path(e.m_trait) )
                return true;
            for(const auto& trait : e.m_markers)
                if( visit_path_params(trait.m_params) )
                    return true;
            return false;
            }
        TU_ARMA(ErasedType, e) {
            if( visit_path(e.m_origin) )
                return true;
            for(const auto& trait : e.m_traits)
                if( visit_trait_path(trait) )
                    return true;
            return false;
            }
        TU_ARMA(Array, e) {
            return visit_type(e.inner);
            }
        TU_ARMA(Slice, e) {
            return visit_type(e.inner);
            }
        TU_ARMA(Tuple, e) {
            for(const auto& ty : e) {
                if( visit_type(ty) )
                    return true;
            }
            return false;
            }
        TU_ARMA(Borrow, e) {
            return visit_type(e.inner);
            }
        TU_ARMA(Pointer, e) {
            return visit_type(e.inner);
            }
        TU_ARMA(Function, e) {
            for(const auto& ty : e.m_arg_types) {
                if( visit_type(ty) )
                    return true;
            }
            return visit_type(e.m_rettype);
            }
        TU_ARMA(Closure, e) {
            for(const auto& ty : e.m_arg_types) {
                if( visit_type(ty) )
                    return true;
            }
            return visit_type(e.m_rettype);
            }
        }
        return false;
    }
};
