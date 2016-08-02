/*
 */
#pragma once

namespace HIR {
    class Crate;
};

extern void Typecheck_ModuleLevel(::HIR::Crate& crate);
extern void Typecheck_Expressions(::HIR::Crate& crate);
extern void Typecheck_Expressions_Validate(::HIR::Crate& crate);
