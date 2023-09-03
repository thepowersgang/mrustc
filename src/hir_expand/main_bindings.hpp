/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/main_bindings.hpp
 * - Functions defined in this folder that are called by main
 */
#pragma once
#include <vector>
#include <utility>  // std::pair

namespace HIR {
    class Crate;
    class ExprPtr;
    class TypeRef;
    struct Pattern;
    class ItemPath;
};

extern void HIR_Expand_AnnotateUsage(::HIR::Crate& crate);
extern void HIR_Expand_VTables(::HIR::Crate& crate);
extern void HIR_Expand_Closures(::HIR::Crate& crate);
extern void HIR_Expand_UfcsEverything(::HIR::Crate& crate);
extern void HIR_Expand_Reborrows(::HIR::Crate& crate);
extern void HIR_Expand_ErasedType(::HIR::Crate& crate);
extern void HIR_Expand_StaticBorrowConstants_Mark(::HIR::Crate& crate);
extern void HIR_Expand_StaticBorrowConstants(::HIR::Crate& crate);

extern void HIR_Expand_AnnotateUsage_Expr(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip, ::HIR::ExprPtr& exp);
extern void HIR_Expand_Closures_Expr(const ::HIR::Crate& crate, ::HIR::TypeRef& exp_ty, ::HIR::ExprPtr& exp);
extern void HIR_Expand_UfcsEverything_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp);
extern void HIR_Expand_Reborrows_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp);
extern void HIR_Expand_StaticBorrowConstants_Mark_Expr(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip, ::HIR::ExprPtr& exp);
extern void HIR_Expand_StaticBorrowConstants_Expr(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip, ::HIR::ExprPtr& exp);
extern void HIR_Expand_LifetimeInfer(::HIR::Crate& crate);
extern void HIR_Expand_LifetimeInfer_Validate(::HIR::Crate& crate);
extern void HIR_Expand_LifetimeInfer_Expr(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip, const ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >& args, const HIR::TypeRef& ret_ty, ::HIR::ExprPtr& exp);
