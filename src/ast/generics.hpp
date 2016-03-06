#pragma once

#include <string>
#include "../types.hpp"

namespace AST {


class TypeParam:
    public Serialisable
{
    ::std::string   m_name;
    TypeRef m_default;
public:
    TypeParam(): m_name("") {}
    TypeParam(::std::string name):
        m_name( ::std::move(name) )
    {}
    void setDefault(TypeRef type) {
        assert(m_default.is_wildcard());
        m_default = ::std::move(type);
    }
    
    const ::std::string&    name() const { return m_name; }
    const TypeRef& get_default() const { return m_default; }
    
    TypeRef& get_default() { return m_default; }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const TypeParam& tp);
    SERIALISABLE_PROTOTYPES();
};

TAGGED_UNION_EX( GenericBound, (: public Serialisable), Lifetime,
    (
    // Lifetime bound: 'test must be valid for 'bound
    (Lifetime, (
        ::std::string   test;
        ::std::string   bound;
        )),
    // Type lifetime bound
    (TypeLifetime, (
        TypeRef type;
        ::std::string   bound;
        )),
    // Standard trait bound: "Type: [for<'a>] Trait"
    (IsTrait, (
        TypeRef type;
        ::std::vector< ::std::string>   hrls; // Higher-ranked lifetimes
        AST::Path   trait;
        )),
    // Removed trait bound: "Type: ?Trait"
    (MaybeTrait, (
        TypeRef type;
        AST::Path   trait;
        )),
    // Negative trait bound: "Type: !Trait"
    (NotTrait, (
        TypeRef type;
        AST::Path   trait;
        )),
    // Type equality: "Type = Replacement"
    (Equality, (
        TypeRef type;
        TypeRef replacement;
        ))
    ),
    (
    public:
        SERIALISABLE_PROTOTYPES();
        
        GenericBound clone() const {
            TU_MATCH(GenericBound, ( (*this) ), (ent),
            (Lifetime,     return make_Lifetime({ent.test, ent.bound});     ),
            (TypeLifetime, return make_TypeLifetime({ent.type, ent.bound}); ),
            (IsTrait,    return make_IsTrait({ent.type, ent.hrls, ent.trait}); ),
            (MaybeTrait, return make_MaybeTrait({ent.type, ent.trait}); ),
            (NotTrait,   return make_NotTrait({ent.type, ent.trait}); ),
            (Equality,   return make_Equality({ent.type, ent.replacement}); )
            )
            return GenericBound();
        }
        )
    );

::std::ostream& operator<<(::std::ostream& os, const GenericBound& x);

class GenericParams:
    public Serialisable
{
    ::std::vector<TypeParam>    m_type_params;
    ::std::vector< ::std::string > m_lifetime_params;
    ::std::vector<GenericBound>    m_bounds;
public:
    GenericParams() {}
    GenericParams(GenericParams&& x) noexcept:
        m_type_params( mv$(x.m_type_params) ),
        m_lifetime_params( mv$(x.m_lifetime_params) ),
        m_bounds( mv$(x.m_bounds) )
    {}
    GenericParams& operator=(GenericParams&& x) {
        m_type_params = mv$(x.m_type_params);
        m_lifetime_params = mv$(x.m_lifetime_params);
        m_bounds = mv$(x.m_bounds);
        return *this;
    }
    GenericParams(const GenericParams& x):
        m_type_params(x.m_type_params),
        m_lifetime_params(x.m_lifetime_params),
        m_bounds()
    {
        m_bounds.reserve( x.m_bounds.size() );
        for(auto& e: x.m_bounds)
            m_bounds.push_back( e.clone() );
    }
    
    const ::std::vector<TypeParam>& ty_params() const { return m_type_params; }
    const ::std::vector< ::std::string>&    lft_params() const { return m_lifetime_params; }
    const ::std::vector<GenericBound>& bounds() const { return m_bounds; }
    ::std::vector<TypeParam>& ty_params() { return m_type_params; }
    ::std::vector<GenericBound>& bounds() { return m_bounds; }
    
    void add_ty_param(TypeParam param) { m_type_params.push_back( ::std::move(param) ); }
    void add_lft_param(::std::string name) { m_lifetime_params.push_back( ::std::move(name) ); }
    void add_bound(GenericBound bound) {
        m_bounds.push_back( ::std::move(bound) );
    }
    
    int find_name(const char* name) const;
    bool check_params(Crate& crate, const ::std::vector<TypeRef>& types) const;
    bool check_params(Crate& crate, ::std::vector<TypeRef>& types, bool allow_infer) const;
    
    friend ::std::ostream& operator<<(::std::ostream& os, const GenericParams& tp);
    SERIALISABLE_PROTOTYPES();
};


}

