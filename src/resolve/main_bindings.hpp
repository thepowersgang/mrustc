/*
 */

#pragma once

namespace AST {
    class Crate;
};

extern void Resolve_Use(::AST::Crate& crate);
extern void Resolve_Index(::AST::Crate& crate);
extern void Resolve_Absolutise(::AST::Crate& crate);
