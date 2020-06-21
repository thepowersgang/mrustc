/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * resolve/common.hpp
 * - Common core to the resolve phase
 */
#pragma once
#include <tagged_union.hpp>

struct Span;
class ExpandProcMacro;
class MacroRules;
namespace AST {
    class Crate;
    class Module;
    class Item;
    class Path;
};
namespace HIR {
    class Module;
};

TAGGED_UNION(ResolveModuleRef, None,
    (None, struct {}),
    (Ast, const AST::Module*),
    (Hir, const HIR::Module*)
    );

#if 0
TAGGED_UNION(ResolveItemRef_Macro, None,
    (None, struct {}),
    (InternalMacro, ExpandProcMacro*),
    (MacroRules, const MacroRules*),
    );

TAGGED_UNION(ResolveItemRef_Type, None,
    (None, struct {}),
    (Ast, const AST::Item*),
    (Hir, const HIR::TypeItem*)
    );
TAGGED_UNION(ResolveItemRef_Value, None,
    (None, struct {}),
    (Ast, const AST::Item*),
    (Hir, const HIR::ValueItem*)
    );
#endif

enum class ResolveNamespace
{
    Namespace,
    Value,
    Macro,
};

extern ResolveModuleRef Resolve_Lookup_GetModule(const Span& span, const AST::Crate& crate, const ::AST::Path& base_path, ::AST::Path path, bool ignore_last, ::AST::Path* out_path);
extern ResolveModuleRef Resolve_Lookup_GetModuleForName(const Span& sp, const AST::Crate& crate, const ::AST::Path& base_path, const ::AST::Path& path, ResolveNamespace ns, ::AST::Path* out_path);
