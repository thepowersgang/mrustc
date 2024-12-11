/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * resolve/common.hpp
 * - Common core to the resolve phase
 */
#pragma once
#include <stdexcept>
#include <tagged_union.hpp>

struct Span;
class ExpandProcMacro;
class MacroRules;
namespace AST {
    class Crate;
    class Module;
    class Item;
    class Path;

    struct AbsolutePath;
};
namespace HIR {
    class Crate;
    class Module;
    class ProcMacro;
    class TypeItem;
    class ValueItem;
};

TAGGED_UNION(ResolveModuleRef, None,
    (None, struct {}),
    (ImplicitPrelude, struct {}),
    (Ast, const AST::Module*),
    (Hir, const HIR::Module*)
    );

TAGGED_UNION(ResolveItemRef_Macro, None,
    (None, struct {}),
    (InternalMacro, ExpandProcMacro*),
    (ProcMacro, const HIR::ProcMacro*),
    (MacroRules, const MacroRules*)
    );
TAGGED_UNION(ResolveItemRef_Type, None,
    (None, struct {}),
    (Ast, const AST::Item*),
    (Hir, const HIR::TypeItem*),
    (HirRoot, const HIR::Crate*)
    );
TAGGED_UNION(ResolveItemRef_Value, None,
    (None, struct {}),
    (Ast, const AST::Item*),
    (Hir, const HIR::ValueItem*)
    );

TAGGED_UNION(ResolveItemRef, None,
    (None, struct {}),
    (Namespace, ResolveItemRef_Type),
    (Value, ResolveItemRef_Value),
    (Macro, ResolveItemRef_Macro)
    );

enum class ResolveNamespace
{
    Namespace,
    Value,
    Macro,
};
extern ::std::ostream& operator<<(::std::ostream& os, ResolveNamespace ns);

/// <summary>
/// Obtain a reference to the module pointed to by `path` (relative to `base_path`)
/// </summary>
/// <param name="span"></param>
/// <param name="crate"></param>
/// <param name="base_path"></param>
/// <param name="path"></param>
/// <param name="ignore_last">Ignore the last node of the path</param>
/// <param name="out_path"></param>
/// <returns></returns>
extern ResolveModuleRef Resolve_Lookup_GetModule(const Span& span, const AST::Crate& crate, const ::AST::Path& base_path, ::AST::Path path, bool ignore_last, ::AST::AbsolutePath* out_path);
extern ResolveItemRef_Macro Resolve_Lookup_Macro(const Span& span, const AST::Crate& crate, const ::AST::Path& base_path, ::AST::Path path, ::AST::AbsolutePath* out_path);
// Returns the module that contains the provided name
extern ResolveModuleRef Resolve_Lookup_GetModuleForName(const Span& sp, const AST::Crate& crate, const ::AST::Path& base_path, const ::AST::Path& path, ResolveNamespace ns, ::AST::AbsolutePath* out_path);
