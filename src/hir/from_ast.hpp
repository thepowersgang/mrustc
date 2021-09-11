/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/from_ast.hpp
 * - Shared code definitions for constructing the HIR from AST
 */
#pragma once

#include <hir/expr_ptr.hpp>

enum class FromAST_PathClass {
    Type,
    Value,
    Macro,
};

extern ::HIR::ExprPtr LowerHIR_ExprNode(const ::AST::ExprNode& e);
extern ::HIR::Path LowerHIR_Path(const Span& sp, const ::AST::Path& path, FromAST_PathClass pc);
extern ::HIR::GenericPath   LowerHIR_GenericPath(const Span& sp, const ::AST::Path& path, FromAST_PathClass pc, bool allow_assoc=false);
extern ::HIR::SimplePath    LowerHIR_SimplePath(const Span& sp, const ::AST::Path& path, FromAST_PathClass pc, bool allow_final_generic = false);
extern ::HIR::PathParams LowerHIR_PathParams(const Span& sp, const ::AST::PathParams& src_params, bool allow_assoc);
extern ::HIR::TypeRef LowerHIR_Type(const ::TypeRef& ty);
extern ::HIR::Pattern LowerHIR_Pattern(const ::AST::Pattern& pat);

extern RcString g_core_crate;
extern RcString g_crate_name;
extern ::HIR::Crate*   g_crate_ptr;
