/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/operations.hpp
 * - Common header for operations performed on MIR functions
 */
#include <hir_typeck/static.hpp>
#include <hir/item_path.hpp>

extern void MIR_Validate(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, const ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type);
extern void MIR_Cleanup(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type);
