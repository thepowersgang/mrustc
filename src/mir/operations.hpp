/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/operations.hpp
 * - Common header for operations performed on MIR functions
 */
#include <hir_typeck/static.hpp>
#include <hir/item_path.hpp>

// Check that the MIR is well-formed
extern void MIR_Validate(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, const ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type);
// -
extern void MIR_Validate_Full(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, const ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type);
// Perform needed changes to the generated MIR (virtualisation, Unsize/CoerceUnsize, ...)
extern void MIR_Cleanup(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type);
// Optimise the MIR
extern void MIR_Optimise(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type, bool do_inline=true);
extern void MIR_OptimiseMin(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type);
extern void MIR_SortBlocks(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn);

extern void MIR_BorrowCheck(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type);

extern void MIR_Dump_Fcn(::std::ostream& sink, const ::MIR::Function& fcn, unsigned int il=0);
