#pragma once

#include <string>
#include "types.hpp"

namespace AST {


class TypeParam
{
    ::std::string   m_name;
    TypeRef m_default;
public:
    TypeParam(TypeParam&& x) = default;
    TypeParam& operator=(TypeParam&& x) = default;
    TypeParam(const TypeParam& x):
        m_name(x.m_name),
        m_default(x.m_default.clone())
    {}
    //TypeParam(): m_name("") {}
    TypeParam(::std::string name):
        m_name( ::std::move(name) ),
        m_default( Span() )
    {}
    void setDefault(TypeRef type) {
        assert(m_default.is_wildcard());
        m_default = ::std::move(type);
    }

    const ::std::string&    name() const { return m_name; }

    const TypeRef& get_default() const { return m_default; }
          TypeRef& get_default()       { return m_default; }

    friend ::std::ostream& operator<<(::std::ostream& os, const TypeParam& tp);
};

TAGGED_UNION_EX( GenericBound, (), Lifetime,
    (
    // Lifetime bound: 'test must be valid for 'bound
    (Lifetime, struct {
        ::std::string   test;
        ::std::string   bound;
        }),
    // Type lifetime bound
    (TypeLifetime, struct {
        TypeRef type;
        ::std::string   bound;
        }),
    // Standard trait bound: "Type: [for<'a>] Trait"
    (IsTrait, struct {
        TypeRef type;
        ::std::vector< ::std::string>   hrls; // Higher-ranked lifetimes
        AST::Path   trait;
        }),
    // Removed trait bound: "Type: ?Trait"
    (MaybeTrait, struct {
        TypeRef type;
        AST::Path   trait;
        }),
    // Negative trait bound: "Type: !Trait"
    (NotTrait, struct {
        TypeRef type;
        AST::Path   trait;
        }),
    // Type equality: "Type = Replacement"
    (Equality, struct {
        TypeRef type;
        TypeRef replacement;
        })
    ),

    (, span(x.span) ), ( span = x.span; ),
    (
    public:

        Span    span;

        GenericBound clone() const {
            TU_MATCH(GenericBound, ( (*this) ), (ent),
            (Lifetime,     return make_Lifetime({ent.test, ent.bound});     ),
            (TypeLifetime, return make_TypeLifetime({ent.type.clone(), ent.bound}); ),
            (IsTrait,    return make_IsTrait({ent.type.clone(), ent.hrls, ent.trait}); ),
            (MaybeTrait, return make_MaybeTrait({ent.type.clone(), ent.trait}); ),
            (NotTrait,   return make_NotTrait({ent.type.clone(), ent.trait}); ),
            (Equality,   return make_Equality({ent.type.clone(), ent.replacement.clone()}); )
            )
            return GenericBound();
        }
        )
    );

::std::ostream& operator<<(::std::ostream& os, const GenericBound& x);

class GenericParams
{
    ::std::vector<TypeParam>    m_type_params;
    ::std::vector< ::std::string > m_lifetime_params;
    ::std::vector<GenericBound>    m_bounds;
public:
    GenericParams() {}
    GenericParams(GenericParams&& x) = default;
    GenericParams& operator=(GenericParams&& x) = default;
    GenericParams(const GenericParams& x) = delete;

    GenericParams clone() const {
        GenericParams   rv;
        rv.m_type_params = ::std::vector<TypeParam>( m_type_params );   // Copy-constructable
        rv.m_lifetime_params = m_lifetime_params;
        rv.m_bounds.reserve( m_bounds.size() );
        for(auto& e: m_bounds)
            rv.m_bounds.push_back( e.clone() );
        return rv;
    }

    const ::std::vector<TypeParam>& ty_params() const { return m_type_params; }
          ::std::vector<TypeParam>& ty_params()       { return m_type_params; }
    const ::std::vector< ::std::string>&    lft_params() const { return m_lifetime_params; }
    const ::std::vector<GenericBound>& bounds() const { return m_bounds; }
          ::std::vector<GenericBound>& bounds()       { return m_bounds; }

    void add_ty_param(TypeParam param) { m_type_params.push_back( ::std::move(param) ); }
    void add_lft_param(::std::string name) { m_lifetime_params.push_back( ::std::move(name) ); }
    void add_bound(GenericBound bound) {
        m_bounds.push_back( ::std::move(bound) );
    }

    int find_name(const char* name) const;
    bool check_params(Crate& crate, const ::std::vector<TypeRef>& types) const;
    bool check_params(Crate& crate, ::std::vector<TypeRef>& types, bool allow_infer) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const GenericParams& tp);
};


}

