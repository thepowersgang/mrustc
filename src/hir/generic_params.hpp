/*
 */
#pragma once

namespace HIR {

struct TypeParamDef
{
    ::std::string   m_name;
    ::HIR::TypeRef  m_default;
};

struct GenericBound
{
};

struct GenericParams
{
    ::std::vector<TypeParamDef>   m_types;
    ::std::vector< ::std::string>   m_lifetimes;
    
    ::std::vector<GenericBound>    m_bounds;
};

}   // namespace HIR

