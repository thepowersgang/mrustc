/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * resolve/main_bindings.hpp
 * - Functions in the resolve pass called by main
 */
#pragma once

namespace AST {
    class Crate;
}

extern void Resolve_Use(::AST::Crate& crate);
extern void Resolve_Index(::AST::Crate& crate);
extern void Resolve_Absolutise(::AST::Crate& crate);
