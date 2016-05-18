#pragma once

#include <hir/expr_ptr.hpp>

extern ::HIR::ExprPtr LowerHIR_ExprNode(const ::AST::ExprNode& e);
extern ::HIR::Path LowerHIR_Path(const ::AST::Path& path);
extern ::HIR::GenericPath   LowerHIR_GenericPath(const ::AST::Path& path);
extern ::HIR::TypeRef LowerHIR_Type(const ::TypeRef& ty);
extern ::HIR::Pattern LowerHIR_Pattern(const ::AST::Pattern& pat);

