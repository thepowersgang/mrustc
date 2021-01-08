/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/main_bindings.hpp
 * - Functions in the "HIR Conversion" group called by main
 */
#pragma once

namespace HIR {
    class Crate;
    class ItemPath;
    class ExprPtr;
};

extern void ConvertHIR_ExpandAliases(::HIR::Crate& crate);
extern void ConvertHIR_ExpandAliases_Self(::HIR::Crate& crate);
extern void ConvertHIR_Bind(::HIR::Crate& crate);
extern void ConvertHIR_ResolveUFCS_SortImpls(::HIR::Crate& crate);
extern void ConvertHIR_ResolveUFCS_Outer(::HIR::Crate& crate);
extern void ConvertHIR_ResolveUFCS(::HIR::Crate& crate);
extern void ConvertHIR_Markings(::HIR::Crate& crate);
extern void ConvertHIR_ConstantEvaluate(::HIR::Crate& hir_crate);

extern void ConvertHIR_ConstantEvaluate_Expr(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip, ::HIR::ExprPtr& exp);

