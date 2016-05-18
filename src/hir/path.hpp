
#ifndef _HIR_PATH_HPP_
#define _HIR_PATH_HPP_
#pragma once

#include <common.hpp>
#include <tagged_union.hpp>
#include <hir/type_ptr.hpp>

namespace HIR {

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
    friend ::std::ostream& operator<<(::std::ostream& os, const SimplePath& x);
};


struct PathParams
{
    ::std::vector<TypeRef>  m_types;
    
    PathParams();
};

/// Generic path - Simple path with one lot of generic params
class GenericPath
{
public:
    SimplePath  m_path;
    PathParams  m_params;

    GenericPath();
    GenericPath(::HIR::SimplePath sp);
    GenericPath(::HIR::SimplePath sp, ::HIR::PathParams params);
};

class TraitPath
{
public:
    GenericPath m_path;
    ::std::vector< ::std::string>   m_hrls;
};

class Path
{
public:
    // Two possibilities
    // - UFCS
    // - Generic path
    TAGGED_UNION(Data, Generic,
    (Generic, GenericPath),
    (UFCS, struct {
        TypeRefPtr  type;
        GenericPath trait;
        ::std::string   item;
        PathParams  params;
        })
    );

private:
    Data m_data;

public:
    Path(GenericPath _);
    Path(TypeRefPtr type, GenericPath trait, ::std::string item, PathParams params);
    Path(SimplePath _);
};

}   // namespace HIR

#endif

