/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/mangling.hpp
 * - Name mangling support
 */
#pragma once
#include <string>

namespace HIR {
    class GenericPath;
    class Path;
    class TypeRef;
}

extern ::std::string Trans_Mangle(const ::HIR::GenericPath& path);
extern ::std::string Trans_Mangle(const ::HIR::Path& path);
extern ::std::string Trans_Mangle(const ::HIR::TypeRef& ty);

