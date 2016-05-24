/*
 */
#pragma once

namespace HIR {
    class Crate;
};

extern void ConvertHIR_ExpandAliases(::HIR::Crate& crate);
extern void ConvertHIR_ConstantEvaluate(::HIR::Crate& hir_crate);
