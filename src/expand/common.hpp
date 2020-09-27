//
//
//
#pragma once
#include <ast/ast.hpp>
#include "../macro_rules/macro_rules.hpp"

TAGGED_UNION(MacroRef, None,
    (None, struct {}),
    (MacroRules, const MacroRules*),
    (BuiltinProcMacro, ExpandProcMacro*),
    (ExternalProcMacro, struct {
        std::vector<RcString>   path;
        })
    );
extern MacroRef Expand_LookupMacro(const Span& mi_span, const ::AST::Crate& crate, LList<const AST::Module*> modstack, const AST::Path& path);