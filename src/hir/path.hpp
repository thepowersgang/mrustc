
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
    ::std::string   m_crate_name;
    ::std::vector< ::std::string>   m_components;

    SimplePath():
        m_crate_name("")
    {
    }
    SimplePath(::std::string crate):
        m_crate_name( mv$(crate) )
    {
    }
    SimplePath(::std::string crate, ::std::vector< ::std::string> components):
        m_crate_name( mv$(crate) ),
        m_components( mv$(components) )
    {
    }

    
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
    (UfcsInherent, struct {
        TypeRefPtr  type;
        ::std::string   item;
        PathParams  params;
        }),
    (UfcsKnown, struct {
        TypeRefPtr  type;
        GenericPath trait;
        ::std::string   item;
        PathParams  params;
        }),
    (UfcsUnknown, struct {
        TypeRefPtr  type;
        //GenericPath ??;
        ::std::string   item;
        PathParams  params;
        })
    );

private:
    Data m_data;

public:
    Path(Data data):
        m_data(mv$(data))
    {}
    Path(GenericPath _);
    Path(TypeRefPtr type, GenericPath trait, ::std::string item, PathParams params);
    Path(SimplePath _);
};

}   // namespace HIR

#endif

