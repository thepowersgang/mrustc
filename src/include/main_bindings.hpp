/*
 */
#ifndef _MAIN_BINDINGS_HPP_
#define _MAIN_BINDINGS_HPP_

#include "../ast/ast.hpp"

extern AST::Crate Parse_Crate(::std::string mainfile);
extern void ResolvePaths(AST::Crate& crate);
extern void Typecheck_GenericBounds(AST::Crate& crate);
extern void Typecheck_GenericParams(AST::Crate& crate);
extern void Typecheck_Expr(AST::Crate& crate);
extern AST::Flat Convert_Flatten(const AST::Crate& crate);

extern void Dump_Rust(const char *Filename, const AST::Crate& crate);

#endif

