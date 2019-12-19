/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/main_bindings.hpp
 * - Functions defined in this folder that are called by main
 */
#pragma once

namespace HIR {
    class Crate;
    class ExprPtr;
};

extern void HIR_Expand_AnnotateUsage(::HIR::Crate& crate);
extern void HIR_Expand_VTables(::HIR::Crate& crate);
extern void HIR_Expand_Closures(::HIR::Crate& crate);
extern void HIR_Expand_UfcsEverything(::HIR::Crate& crate);
extern void HIR_Expand_Reborrows(::HIR::Crate& crate);
extern void HIR_Expand_ErasedType(::HIR::Crate& crate);
extern void HIR_Expand_StaticBorrowConstants(::HIR::Crate& crate);

extern void HIR_Expand_AnnotateUsage_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp);
extern void HIR_Expand_Closures_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp);
extern void HIR_Expand_UfcsEverything_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp);
extern void HIR_Expand_Reborrows_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp);
//extern void HIR_Expand_StaticBorrowConstants_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp);
