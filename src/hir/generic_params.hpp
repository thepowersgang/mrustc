/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/generic_params.hpp
 * - HIR version of generic definition blocks
 */
#pragma once
#include <string>
#include <vector>
#include <iostream>
#include "generic_ref.hpp"
#include "path.hpp"
#include "type_ref.hpp"

namespace HIR {

struct TypeParamDef
{
    RcString    m_name;
    ::HIR::TypeRef  m_default;
    bool    m_is_sized;
    Ordering ord(const TypeParamDef& x) const {
        ORD(m_name, x.m_name);
        ORD(m_default, x.m_default);
        ORD(m_is_sized, x.m_is_sized);
        return OrdEqual;
    }
};
struct LifetimeDef
{
    RcString    m_name;
    Ordering ord(const LifetimeDef& x) const {
        ORD(m_name, x.m_name);
        return OrdEqual;
    }
};
struct ValueParamDef
{
    RcString    m_name;
    ::HIR::TypeRef  m_type;
    ::HIR::ExprPtr  m_default;
    Ordering ord(const ValueParamDef& x) const {
        ORD(m_name, x.m_name);
        ORD(m_type, x.m_type);
        //ORD(m_default, x.m_default);
        return OrdEqual;
    }
};

class GenericParams;

TAGGED_UNION_EX(GenericBound, (), Lifetime, (
    (Lifetime, struct {
        LifetimeRef test;
        LifetimeRef valid_for;
        }),
    (TypeLifetime, struct {
        ::HIR::TypeRef  type;
        LifetimeRef valid_for;
        }),
    (TraitBound, struct {
        ::std::unique_ptr<::HIR::GenericParams> hrtbs;
        ::HIR::TypeRef  type;
        ::HIR::TraitPath    trait;
        })/*,
    (NotTrait, struct {
        ::HIR::TypeRef  type;
        ::HIR::GenricPath    trait;
        })*/,
    (TypeEquality, struct {
        ::HIR::TypeRef  type;
        ::HIR::TypeRef  other_type;
        })
    ), (), (), (
    GenericBound clone() const;
    Ordering ord(const GenericBound& x) const;
    ));
extern ::std::ostream& operator<<(::std::ostream& os, const GenericBound& x);

class GenericParams
{
public:
    ::std::vector<TypeParamDef> m_types;
    ::std::vector<LifetimeDef>  m_lifetimes;
    ::std::vector<ValueParamDef>    m_values;

    ::std::vector<GenericBound>    m_bounds;

    //GenericParams() {}

    GenericParams clone() const;
    bool is_empty() const {
        if(!m_types.empty())        return false;
        if(!m_lifetimes.empty())    return false;
        if(!m_values.empty())       return false;
        if(!m_bounds.empty())       return false;
        return true;
    }
    bool is_generic() const {
        if(!m_types.empty())    return true;
        // Note: Lifetimes don't matter
        if(!m_values.empty())    return true;
        return false;
    }

    /// Create a PathParams instance that doesn't monomorphise at all
    PathParams make_nop_params(unsigned level, bool lifetimes_only=false) const;
    PathParams make_empty_params(bool lifetimes_only=false) const {
        assert(lifetimes_only);
        PathParams  rv;
        rv.m_lifetimes = ThinVector<LifetimeRef>(m_lifetimes.size());
        return rv;
    }

    struct PrintArgs {
        const GenericParams& gp;
        PrintArgs(const GenericParams& gp): gp(gp) {}
        friend ::std::ostream& operator<<(::std::ostream& os, const PrintArgs& x);
    };
    PrintArgs fmt_args() const { return PrintArgs(*this); }
    struct PrintBounds {
        const GenericParams& gp;
        PrintBounds(const GenericParams& gp): gp(gp) {}
        friend ::std::ostream& operator<<(::std::ostream& os, const PrintBounds& x);
    };
    PrintBounds fmt_bounds() const { return PrintBounds(*this); }

    Ordering ord(const HIR::GenericParams& x) const {
        ORD(m_types, x.m_types);
        ORD(m_lifetimes, x.m_lifetimes);
        ORD(m_values, x.m_values);
        ORD(m_bounds, x.m_bounds);
        return OrdEqual;
    }
};

}   // namespace HIR

