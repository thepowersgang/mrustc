/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/monomorphise.hpp
 * - MIR monomorphisation
 */
#pragma once

#include <mir/mir_ptr.hpp>
#include "trans_list.hpp"

namespace HIR {
    class Crate;
}

extern ::MIR::FunctionPointer Trans_Monomorphise(const ::StaticTraitResolve& crate, const Trans_Params& params, const ::MIR::FunctionPointer& tpl);
