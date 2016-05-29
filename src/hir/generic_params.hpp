/*
 */
#pragma once

namespace HIR {

struct TypeParamDef
{
    ::std::string   m_name;
    ::HIR::TypeRef  m_default;
    bool    m_is_sized;
};

TAGGED_UNION(GenericBound, Lifetime,
    (Lifetime, struct {
        ::std::string   test;
        ::std::string   valid_for;
        }),
    (TypeLifetime, struct {
        ::HIR::TypeRef  type;
        ::std::string   valid_for;
        }),
    (TraitBound, struct {
        ::HIR::TypeRef  type;
        ::HIR::TraitPath    trait;
        }),
    //(NotTrait, struct {
    //    ::HIR::TypeRef  type;
    //    ::HIR::GenricPath    trait;
    //    }),
    (TypeEquality, struct {
        ::HIR::TypeRef  type;
        ::HIR::TypeRef  other_type;
        })
    );

struct GenericParams
{
    ::std::vector<TypeParamDef>   m_types;
    ::std::vector< ::std::string>   m_lifetimes;
    
    ::std::vector<GenericBound>    m_bounds;
};

}   // namespace HIR

