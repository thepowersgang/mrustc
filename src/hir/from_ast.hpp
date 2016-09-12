/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/from_ast.hpp
 * - Shared code definitions for constructing the HIR from AST
 */
#pragma once

#include <hir/expr_ptr.hpp>

extern ::HIR::ExprPtr LowerHIR_ExprNode(const ::AST::ExprNode& e);
extern ::HIR::Path LowerHIR_Path(const Span& sp, const ::AST::Path& path);
extern ::HIR::GenericPath   LowerHIR_GenericPath(const Span& sp, const ::AST::Path& path, bool allow_assoc=false);
extern ::HIR::SimplePath    LowerHIR_SimplePath(const Span& sp, const ::AST::Path& path, bool allow_final_generic = false);
extern ::HIR::TypeRef LowerHIR_Type(const ::TypeRef& ty);
extern ::HIR::Pattern LowerHIR_Pattern(const ::AST::Pattern& pat);

extern ::std::string   g_core_crate;
extern ::HIR::Crate*   g_crate_ptr;
