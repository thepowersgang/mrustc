/*
 */
#pragma once
#ifndef _SYNEXT_HPP_
#define _SYNEXT_HPP_

#include "../common.hpp"   // for LList
#include "synext_decorator.hpp"
#include "synext_macro.hpp"

extern void Expand_BareExpr(const ::AST::Crate& crate, const AST::Module& mod, ::std::unique_ptr<AST::ExprNode>& node);

#endif

