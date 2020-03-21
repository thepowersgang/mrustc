/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/path.hpp
 * - Item paths
 */
#ifndef _HIR_PATH_HPP_
#define _HIR_PATH_HPP_
#pragma once

#include <common.hpp>
#include <tagged_union.hpp>
#include <span.hpp>

namespace HIR {

class TypeRef;
class Trait;

enum Compare {
    Equal,
    Fuzzy,
    Unequal,
};

typedef ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)> t_cb_resolve_type;
typedef ::std::function< ::HIR::Compare(unsigned int, const RcString&, const ::HIR::TypeRef&) > t_cb_match_generics;

static inline ::std::ostream& operator<<(::std::ostream& os, const Compare& x) {
    switch(x)
    {
    case Compare::Equal:    os << "Equal";  break;
    case Compare::Fuzzy:    os << "Fuzzy";  break;
    case Compare::Unequal:  os << "Unequal"; break;
    }
    return os;
}
static inline Compare& operator &=(Compare& x, const Compare& y) {
    if(x == Compare::Unequal) {
    }
    else if(y == Compare::Unequal) {
        x = Compare::Unequal;
    }
    else if(y == Compare::Fuzzy) {
        x = Compare::Fuzzy;
    }
    else {
        // keep as-is
    }
    return x;
}

/// Simple path - Absolute with no generic parameters
// TODO: Investigate having this be a custom Rc vector-alike
struct SimplePath
{
    RcString   m_crate_name;
    ::std::vector< RcString>   m_components;

    SimplePath():
        m_crate_name("")
    {
    }
    SimplePath(RcString crate):
        m_crate_name( mv$(crate) )
    {
    }
    SimplePath(RcString crate, ::std::vector< RcString> components):
        m_crate_name( mv$(crate) ),
        m_components( mv$(components) )
    {
    }

    SimplePath clone() const;

    SimplePath operator+(const RcString& s) const;
    bool operator==(const SimplePath& x) const {
        return m_crate_name == x.m_crate_name && m_components == x.m_components;
    }
    bool operator!=(const SimplePath& x) const {
        return !(*this == x);
    }
    bool operator<(const SimplePath& x) const {
        if( m_crate_name < x.m_crate_name ) return true;
        if( m_crate_name > x.m_crate_name ) return false;
        return ( m_components < x.m_components );
    }
    Ordering ord(const SimplePath& x) const {
        auto rv = ::ord(m_crate_name, x.m_crate_name);
        if(rv != OrdEqual)  return rv;
        rv = ::ord(m_components, x.m_components);
        return rv;
    }
    friend ::std::ostream& operator<<(::std::ostream& os, const SimplePath& x);
};


struct PathParams
{
    //::std::vector<LifetimeRef>  m_lifetimes;
    ::std::vector<TypeRef>  m_types;

    PathParams();
    PathParams(::HIR::TypeRef );
    PathParams clone() const;
    PathParams(const PathParams&) = delete;
    PathParams& operator=(const PathParams&) = delete;
    PathParams(PathParams&&) = default;
    PathParams& operator=(PathParams&&) = default;

    Compare compare_with_placeholders(const Span& sp, const PathParams& x, t_cb_resolve_type resolve_placeholder) const;
    Compare match_test_generics_fuzz(const Span& sp, const PathParams& x, t_cb_resolve_type resolve_placeholder, t_cb_match_generics) const;

    /// Indicates that params exist (and thus the target requires monomorphisation)
    /// - Ignores lifetime params
    bool has_params() const {
        return !m_types.empty();
    }

    bool operator==(const PathParams& x) const;
    bool operator!=(const PathParams& x) const { return !(*this == x); }
    bool operator<(const PathParams& x) const { return ord(x) == OrdLess; }
    Ordering ord(const PathParams& x) const {
        return ::ord(m_types, x.m_types);
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const PathParams& x);
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

    GenericPath clone() const;
    Compare compare_with_placeholders(const Span& sp, const GenericPath& x, t_cb_resolve_type resolve_placeholder) const;

    bool operator==(const GenericPath& x) const;
    bool operator!=(const GenericPath& x) const { return !(*this == x); }
    bool operator<(const GenericPath& x) const { return ord(x) == OrdLess; }

    Ordering ord(const GenericPath& x) const {
        auto rv = ::ord(m_path, x.m_path);
        if(rv != OrdEqual)  return rv;
        return ::ord(m_params, x.m_params);
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const GenericPath& x);
};

class TraitPath
{
public:
    GenericPath m_path;
    ::std::vector< RcString>   m_hrls;
    // TODO: Each bound should list its origin trait
    ::std::map< RcString, ::HIR::TypeRef>    m_type_bounds;

    const ::HIR::Trait* m_trait_ptr;

    TraitPath clone() const;
    Compare compare_with_placeholders(const Span& sp, const TraitPath& x, t_cb_resolve_type resolve_placeholder) const;

    bool operator==(const TraitPath& x) const;
    bool operator!=(const TraitPath& x) const { return !(*this == x); }
    bool operator<(const TraitPath& x) const { return ord(x) == OrdLess; }

    Ordering ord(const TraitPath& x) const {
        ORD(m_path, x.m_path);
        ORD(m_hrls, x.m_hrls);
        return ::ord(m_type_bounds, x.m_type_bounds);
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const TraitPath& x);
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
        ::std::unique_ptr<TypeRef>  type;
        RcString   item;
        PathParams  params;
        PathParams  impl_params;
        }),
    (UfcsKnown, struct {
        ::std::unique_ptr<TypeRef>  type;
        GenericPath trait;
        RcString   item;
        PathParams  params;
        }),
    (UfcsUnknown, struct {
        ::std::unique_ptr<TypeRef>  type;
        //GenericPath ??;
        RcString   item;
        PathParams  params;
        })
    );

    Data m_data;

    Path(Data data):
        m_data(mv$(data))
    {}
    Path(GenericPath _);
    Path(SimplePath _);

    Path(TypeRef ty, RcString item, PathParams item_params=PathParams());
    Path(TypeRef ty, GenericPath trait, RcString item, PathParams item_params=PathParams());

    Path clone() const;
    Compare compare_with_placeholders(const Span& sp, const Path& x, t_cb_resolve_type resolve_placeholder) const;

    Ordering ord(const Path& x) const;

    bool operator==(const Path& x) const;
    bool operator!=(const Path& x) const { return !(*this == x); }
    bool operator<(const Path& x) const { return ord(x) == OrdLess; }

    friend ::std::ostream& operator<<(::std::ostream& os, const Path& x);
};

}   // namespace HIR

#endif

