/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/main_bindings.hpp
 * - main.cpp binding
 */
#pragma once

namespace HIR {
class Crate;
}

extern void HIR_GenerateMIR(::HIR::Crate& crate);
