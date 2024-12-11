//
//
//
#pragma once
#include "../macro_rules/macro_rules.hpp"

namespace HIR {
    class ProcMacro;
}
namespace AST {
    class Crate;
    class Module;
    class Path;
}
class ExpandProcMacro;

TAGGED_UNION_EX(MacroRef, (), None, (
    (None, struct {}),
    (MacroRules, const MacroRules*),
    (BuiltinProcMacro, /*const*/ ExpandProcMacro*),
    (ExternalProcMacro, const HIR::ProcMacro*)
    ), (), (), (
        MacroRef clone() const {
            switch(tag())
            {
            case TAGDEAD:   abort();
            case TAG_None:  return make_None({});
            case TAG_MacroRules:  return as_MacroRules();
            case TAG_BuiltinProcMacro:  return as_BuiltinProcMacro();
            case TAG_ExternalProcMacro: return as_ExternalProcMacro();
            }
            abort();
        }
    )
    );
extern MacroRef Expand_LookupMacro(const Span& mi_span, const ::AST::Crate& crate, LList<const AST::Module*> modstack, const AST::AttributeName& path);
extern MacroRef Expand_LookupMacro(const Span& mi_span, const ::AST::Crate& crate, LList<const AST::Module*> modstack, const AST::Path& path);

extern ExpandProcMacro* Expand_FindProcMacro(const RcString& name);
