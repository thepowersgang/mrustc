/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/main_bindings.hpp
 * - Functions in hir/ used by main
 */
#pragma once

#include "crate_ptr.hpp"
#include <iostream>
#include <string>

class RcString;
namespace AST {
    class Crate;
}

extern void HIR_Dump(::std::ostream& sink, const ::HIR::Crate& crate);
extern ::HIR::CratePtr  LowerHIR_FromAST(::AST::Crate crate);
extern void HIR_Serialise(const ::std::string& filename, const ::HIR::Crate& crate);

extern ::HIR::CratePtr HIR_Deserialise(const ::std::string& filename);
extern RcString HIR_Deserialise_JustName(const ::std::string& filename);
