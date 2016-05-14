
#ifndef _HIR_PATH_HPP_
#define _HIR_PATH_HPP_
#pragma once

#include <common.hpp>

namespace HIR {

class TypeRef;

/// Simple path - Absolute with no generic parameters
struct SimplePath
{
    SimplePath():
        m_crate_name("")
    {
    }
    SimplePath(::std::string crate):
        m_crate_name( mv$(crate) )
    {
    }

    ::std::string   m_crate_name;
    ::std::vector< ::std::string>   m_components;

    
    SimplePath operator+(const ::std::string& s) const;
};
/// Generic path - Simple path with one lot of generic params
class GenericPath
{
public:
    SimplePath  m_path;
    ::std::vector<TypeRef>  m_params;
};

class Path
{
    // Two possibilities
    // - UFCS
    // - Generic path
public:
    Path(GenericPath _);
    Path(SimplePath _);
};

}   // namespace HIR

#endif

