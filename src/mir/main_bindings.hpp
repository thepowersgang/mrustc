/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/main_bindings.hpp
 * - main.cpp binding
 */
#pragma once
#include <iostream>
#include <hir/hir.hpp>

class TransList;

extern void HIR_GenerateMIR(::HIR::Crate& crate);
extern void MIR_Dump(::std::ostream& sink, const ::HIR::Crate& crate);
extern void MIR_CheckCrate(/*const*/ ::HIR::Crate& crate);
extern void MIR_CheckCrate_Full(/*const*/ ::HIR::Crate& crate);
extern void MIR_BorrowCheck_Crate(::HIR::Crate& crate);

extern void MIR_CleanupCrate(::HIR::Crate& crate);
extern void MIR_OptimiseCrate(::HIR::Crate& crate, bool minimal_optimisations);
extern void MIR_OptimiseCrate_Inlining(const ::HIR::Crate& crate, TransList& list, bool post_save);


extern void HIR_GenerateMIR_Expr(const ::HIR::Crate& crate, const ::HIR::ItemPath& path, ::HIR::ExprPtr& expr_ptr, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& res_ty);
