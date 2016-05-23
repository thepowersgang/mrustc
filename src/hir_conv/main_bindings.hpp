/*
 */
#pragma once

namespace HIR {
    class Crate;
};

extern void ConvertHIR_ExpandAliases(::HIR::Crate& crate);
