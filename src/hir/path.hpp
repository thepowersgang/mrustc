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
#include "type_ref.hpp"
#include "generic_ref.hpp"
#include "expr_ptr.hpp"

struct EncodedLiteral;

namespace HIR {

class EncodedLiteralPtr {
    EncodedLiteral* p;
public:
    ~EncodedLiteralPtr();
    EncodedLiteralPtr(): p(nullptr) {}
    EncodedLiteralPtr(EncodedLiteral e);

    EncodedLiteralPtr(EncodedLiteralPtr&& x): p(x.p) { x.p = nullptr; }
    EncodedLiteralPtr(const EncodedLiteralPtr& x) = delete;

    EncodedLiteralPtr& operator=(EncodedLiteralPtr&& x) { this->~EncodedLiteralPtr(); this->p = x.p; x.p = nullptr; return *this; }
    EncodedLiteralPtr& operator=(const EncodedLiteralPtr& x) = delete;

    EncodedLiteral& operator*() { assert(p); return *p; }
    const EncodedLiteral& operator*() const { assert(p); return *p; }
    EncodedLiteral* operator->() { assert(p); return p; }
    const EncodedLiteral* operator->() const { assert(p); return p; }
};
TAGGED_UNION_EX(ConstGeneric, (), Infer, (
    (Infer, struct {    // To be inferred
        unsigned index = ~0u;
        }),
    (Unevaluated, std::shared_ptr<HIR::ExprPtr>),    // Unevaluated (or evaluation deferred)
    (Generic, GenericRef),  // A single generic reference
    (Evaluated, EncodedLiteralPtr) // A fully known literal
    ),
    /*extra_move=*/(),
    /*extra_assign=*/(),
    /*extra=*/(
        ConstGeneric clone() const;
        bool operator==(const ConstGeneric& x) const;
        bool operator!=(const ConstGeneric& x) const { return !(*this == x); }
        Ordering ord(const ConstGeneric& x) const;
        )
    );
::std::ostream& operator<<(::std::ostream& os, const ConstGeneric& x);

class TypeRef;
class Trait;

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
// - Would save 3*8 bytes inline, and make comparison/clone cheaper
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
        return ord(x) == OrdLess;
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
    ::std::vector<HIR::ConstGeneric>  m_values;

    PathParams();
    PathParams(::HIR::TypeRef );
    PathParams clone() const;
    PathParams(const PathParams&) = delete;
    PathParams& operator=(const PathParams&) = delete;
    PathParams(PathParams&&) = default;
    PathParams& operator=(PathParams&&) = default;

    Compare compare_with_placeholders(const Span& sp, const PathParams& x, t_cb_resolve_type resolve_placeholder) const;
    Compare match_test_generics_fuzz(const Span& sp, const PathParams& x, t_cb_resolve_type resolve_placeholder, ::HIR::MatchGenerics& match) const;

    /// Indicates that params exist (and thus the target requires monomorphisation)
    /// - Ignores lifetime params
    bool has_params() const {
        return !m_types.empty() || !m_values.empty();
    }

    bool operator==(const PathParams& x) const;
    bool operator!=(const PathParams& x) const { return !(*this == x); }
    bool operator<(const PathParams& x) const { return ord(x) == OrdLess; }
    Ordering ord(const PathParams& x) const {
        if(auto cmp = ::ord(m_types, x.m_types)) return cmp;
        if(auto cmp = ::ord(m_values, x.m_values)) return cmp;
        return OrdEqual;
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
    // TODO: Each bound should list its origin trait
    struct AtyEqual {
        ::HIR::GenericPath  source_trait;
        ::HIR::TypeRef  type;

        Ordering ord(const AtyEqual& x) const {
            ORD(source_trait, x.source_trait);
            ORD(type, x.type);
            return OrdEqual;
        }
        AtyEqual clone() const {
            return AtyEqual {
                source_trait.clone(),
                type.clone()
                };
        }
        friend ::std::ostream& operator<<(::std::ostream& os, const AtyEqual& x) {
            os << x.type;
            return os;
        }

    };
    /// Associated type trait bounds (`Type: Trait`)
    struct AtyBound {
        ::HIR::GenericPath  source_trait;
        std::vector<::HIR::TraitPath>   traits;

        Ordering ord(const AtyBound& x) const {
            ORD(source_trait, x.source_trait);
            ORD(traits, x.traits);
            return OrdEqual;
        }
        AtyBound clone() const {
            std::vector<::HIR::TraitPath>   new_traits;
            new_traits.reserve(traits.size());
            for(const auto& t : traits)
                new_traits.push_back(t.clone());
            return AtyBound {
                source_trait.clone(),
                ::std::move(new_traits)
            };
        }
    };

    typedef ::std::map< RcString, AtyEqual> assoc_list_t;

    GenericPath m_path;
    ::std::vector< RcString>   m_hrls;
    assoc_list_t    m_type_bounds;
    ::std::map< RcString, AtyBound>  m_trait_bounds;

    const ::HIR::Trait* m_trait_ptr;

    TraitPath clone() const;
    Compare compare_with_placeholders(const Span& sp, const TraitPath& x, t_cb_resolve_type resolve_placeholder) const;

    bool operator==(const TraitPath& x) const { return ord(x) == OrdEqual; }
    bool operator!=(const TraitPath& x) const { return ord(x) != OrdEqual; }
    bool operator<(const TraitPath& x) const { return ord(x) == OrdLess; }

    Ordering ord(const TraitPath& x) const {
        ORD(m_path, x.m_path);
        ORD(m_hrls, x.m_hrls);
        ORD(m_trait_bounds, x.m_trait_bounds);
        ORD(m_type_bounds , x.m_type_bounds);
        return OrdEqual;
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
        TypeRef type;
        RcString   item;
        PathParams  params;
        PathParams  impl_params;
        }),
    (UfcsKnown, struct {
        TypeRef type;
        GenericPath trait;
        RcString   item;
        PathParams  params;
        }),
    (UfcsUnknown, struct {
        TypeRef type;
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

