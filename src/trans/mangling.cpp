/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/mangling.hpp
 * - Name mangling support
 */
#include "mangling.hpp"
#include <hir/type.hpp>
#include <hir/path.hpp>

::std::string Trans_Mangle(const ::HIR::GenericPath& path)
{
    ::std::stringstream ss;
    ss << "_ZN" << path.m_path.m_crate_name.size() << path.m_path.m_crate_name;
    for(const auto& comp : path.m_path.m_components)
        ss << comp.size() << comp;
    
    return ss.str();
}
::std::string Trans_Mangle(const ::HIR::Path& path)
{
    return "";
}
::std::string Trans_Mangle(const ::HIR::TypeRef& ty)
{
    return "";
}

