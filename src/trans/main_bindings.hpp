/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/main_bindings.hpp
 * - Trans functions called by main()
 */
#pragma once

#include "trans_list.hpp"

namespace HIR {
class Crate;
}

extern TransList Trans_Enumerate_Main(const ::HIR::Crate& crate);
extern TransList Trans_Enumerate_Public(const ::HIR::Crate& crate);

extern void Trans_Codegen(const ::std::string& outfile, const ::HIR::Crate& crate, const TransList& list);
