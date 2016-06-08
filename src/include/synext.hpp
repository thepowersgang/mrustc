/*
 */
#pragma once
#ifndef _SYNEXT_HPP_
#define _SYNEXT_HPP_

#include "../common.hpp"   // for LList
#include "synext_decorator.hpp"
#include "synext_macro.hpp"

extern void Expand_Expr(bool is_early, ::AST::Crate& crate, LList<const AST::Module*> modstack, ::std::unique_ptr<AST::ExprNode>& node);

#endif

