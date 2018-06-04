/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/main_bindings.hpp
 * - Functions in HIR typecheck called by main
 */
#pragma once

namespace HIR {
    class Crate;
};

extern void Typecheck_ModuleLevel(::HIR::Crate& crate);
extern void Typecheck_Expressions(::HIR::Crate& crate);
extern void Typecheck_Expressions_Validate(::HIR::Crate& crate);
