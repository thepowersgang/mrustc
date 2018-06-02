/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/mangling.hpp
 * - Name mangling support
 */
#pragma once
#include <string>
#include <debug.hpp>

namespace HIR {
    struct SimplePath;
    class GenericPath;
    class Path;
    class TypeRef;
}

extern ::FmtLambda Trans_Mangle(const ::HIR::SimplePath& path);
extern ::FmtLambda Trans_Mangle(const ::HIR::GenericPath& path);
extern ::FmtLambda Trans_Mangle(const ::HIR::Path& path);
extern ::FmtLambda Trans_Mangle(const ::HIR::TypeRef& ty);

