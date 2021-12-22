/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/synext.hpp
 * - Generic syntax extension support
 */
#pragma once
#ifndef _SYNEXT_HPP_
#define _SYNEXT_HPP_

#include "../common.hpp"   // for LList
#include "synext_decorator.hpp"
#include "synext_macro.hpp"

extern ::AST::ExprNodeP Expand_ParseAndExpand_ExprVal(const ::AST::Crate& crate, const AST::Module& mod, TokenStream& lex);
extern void Expand_BareExpr(const ::AST::Crate& crate, const AST::Module& mod, AST::ExprNodeP& node);

#endif

