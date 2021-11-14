/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/main_bindings.hpp
 * - Functions in the "HIR Conversion" group called by main
 */
#pragma once

struct Span;
namespace HIR {
    class Crate;
    class ItemPath;
    class ExprPtr;
    class Enum;
    struct SimplePath;
    class GenericParams;
    struct PathParams;
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
extern void ConvertHIR_ConstantEvaluate_Enum(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip, const ::HIR::Enum& enm);
void ConvertHIR_ConstantEvaluate_MethodParams(
    const Span& sp,
    const ::HIR::Crate& crate, const HIR::SimplePath& mod_path, const ::HIR::GenericParams* impl_generics, const ::HIR::GenericParams* item_generics,
    const ::HIR::GenericParams& params_def,
    ::HIR::PathParams& params
);

