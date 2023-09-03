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
extern bool visit_ty_with(const ::HIR::TypeRef& , t_cb_visit_ty callback);
extern bool visit_trait_path_tys_with(const ::HIR::TraitPath& , t_cb_visit_ty callback);
extern bool visit_path_tys_with(const ::HIR::Path& , t_cb_visit_ty callback);

typedef ::std::function<bool(::HIR::TypeRef&)> t_cb_visit_ty_mut;
extern bool visit_ty_with_mut(::HIR::TypeRef& ty, t_cb_visit_ty_mut callback);
extern bool visit_path_tys_with_mut(::HIR::Path& ty, t_cb_visit_ty_mut callback);

typedef ::std::function<bool(const ::HIR::TypeRef&, ::HIR::TypeRef&)>   t_cb_clone_ty;
/// Clones a type, calling the provided callback on every type (optionally providing a replacement)
extern ::HIR::TypeRef clone_ty_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_clone_ty callback);
extern ::HIR::PathParams clone_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_clone_ty callback);

extern void check_type_class_primitive(const Span& sp, const ::HIR::TypeRef& type, ::HIR::InferClass ic, ::HIR::CoreType ct);

class StaticTraitResolve;
extern void Typecheck_Expressions_ValidateOne(const StaticTraitResolve& resolve, const ::std::vector<::std::pair< ::HIR::Pattern, ::HIR::TypeRef>>& args, const ::HIR::TypeRef& ret_ty, const ::HIR::ExprPtr& code);


