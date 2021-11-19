/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/static.cpp
 * - Non-inferred type checking
 */
#pragma once

#include <hir/hir.hpp>
#include "common.hpp"
#include "impl_ref.hpp"
#include <range_vec_map.hpp>
#include "resolve_common.hpp"

enum class MetadataType {
    Unknown,    // Unknown still
    None,   // Sized pointer
    Zero,   // No metadata, but still unsized
    Slice,  // usize metadata
    TraitObject,    // VTable pointer metadata
};
static inline std::ostream& operator<<(std::ostream& os, const MetadataType& x) {
    switch(x)
    {
    case MetadataType::Unknown: return os << "Unknown";
    case MetadataType::None:    return os << "None";
    case MetadataType::Zero:    return os << "Zero";
    case MetadataType::Slice:   return os << "Slice";
    case MetadataType::TraitObject:    return os << "TraitObject";
    }
    return os << "?";
}


class StaticTraitResolve:
    public TraitResolveCommon
{
    mutable ::std::map< ::HIR::TypeRef, bool >  m_copy_cache;
    mutable ::std::map< ::HIR::TypeRef, bool >  m_clone_cache;
    mutable ::std::map< ::HIR::TypeRef, bool >  m_drop_cache;
    mutable ::std::map< ::HIR::Path, HIR::TypeRef>  m_aty_cache;

public:
    StaticTraitResolve(const ::HIR::Crate& crate):
        TraitResolveCommon(crate)
    {
    }

private:
    void prep_indexes() {
        m_copy_cache.clear();
        m_clone_cache.clear();
        m_drop_cache.clear();
        m_aty_cache.clear();
        TraitResolveCommon::prep_indexes(Span());
    }
public:

    /// \brief State manipulation
    /// \{
    NullOnDrop<const ::HIR::GenericParams> set_impl_generics(const ::HIR::GenericParams& gps) {
        set_impl_generics_raw(gps);
        return NullOnDrop<const ::HIR::GenericParams>(m_impl_generics);
    }
    NullOnDrop<const ::HIR::GenericParams> set_item_generics(const ::HIR::GenericParams& gps) {
        set_item_generics_raw(gps);
        return NullOnDrop<const ::HIR::GenericParams>(m_item_generics);
    }
    void set_impl_generics_raw(const ::HIR::GenericParams& gps) {
        assert( !m_impl_generics );
        m_impl_generics = &gps;
        prep_indexes();
    }
    void clear_impl_generics() {
        m_impl_generics = nullptr;
        prep_indexes();
    }
    void set_item_generics_raw(const ::HIR::GenericParams& gps) {
        assert( !m_item_generics );
        m_item_generics = &gps;
        prep_indexes();
    }
    void clear_item_generics() {
        m_item_generics = nullptr;
        prep_indexes();
    }
    void set_both_generics_raw(const ::HIR::GenericParams* gps_impl, const ::HIR::GenericParams* gps_fcn) {
        assert( !m_impl_generics );
        assert( !m_item_generics );
        m_impl_generics = gps_impl;
        m_item_generics = gps_fcn;
        prep_indexes();
    }
    void clear_both_generics() {
        m_impl_generics = nullptr;
        m_item_generics = nullptr;
        prep_indexes();
    }
    /// \}

    /// \brief Lookups
    /// \{
    typedef ::std::function<bool(ImplRef, bool is_fuzzed)> t_cb_find_impl;

    bool find_impl(
        const Span& sp,
        const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& trait_params,
        const ::HIR::TypeRef& type,
        t_cb_find_impl found_cb
        ) const
    {
        return this->find_impl(sp, trait_path, &trait_params, type, found_cb);
    }
    bool find_impl(
        const Span& sp,
        const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
        const ::HIR::TypeRef& type,
        t_cb_find_impl found_cb,
        bool dont_handoff_to_specialised = false
        ) const;

private:
    bool find_impl__bounds(
        const Span& sp,
        const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
        const ::HIR::TypeRef& type,
        t_cb_find_impl found_cb
        ) const;
    bool find_impl__check_crate(
        const Span& sp,
        const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
        const ::HIR::TypeRef& type,
        t_cb_find_impl found_cb,
        const ::HIR::TraitImpl& impl
        ) const;
    bool find_impl__check_crate_raw(
        const Span& sp,
        const ::HIR::SimplePath& des_trait_path, const ::HIR::PathParams* des_trait_params, const ::HIR::TypeRef& des_type,
        const ::HIR::GenericParams& impl_params_def, const ::HIR::PathParams& impl_trait_params, const ::HIR::TypeRef& impl_type,
        ::std::function<bool(HIR::PathParams, ::HIR::Compare)>
        ) const;
    ::HIR::Compare check_auto_trait_impl_destructure(
        const Span& sp,
        const ::HIR::SimplePath& trait, const ::HIR::PathParams* params_ptr,
        const ::HIR::TypeRef& type
        ) const;

public:

    void expand_associated_types(const Span& sp, ::HIR::TypeRef& input) const;
    void expand_associated_types_path(const Span& sp, ::HIR::Path& input) const;
    bool expand_associated_types_single(const Span& sp, ::HIR::TypeRef& input) const;

    // Helper: Run monomorphise+EAT if the type contains generics
    const ::HIR::TypeRef& monomorph_expand_opt(const Span& sp, ::HIR::TypeRef& tmp, const ::HIR::TypeRef& input, const Monomorphiser& m) const {
        if( monomorphise_type_needed(input) ) {
            return tmp = monomorph_expand(sp, input, m);
        }
        else {
            return input;
        }
    }
    ::HIR::TypeRef monomorph_expand(const Span& sp, const ::HIR::TypeRef& input, const Monomorphiser& m) const {
        auto rv = m.monomorph_type(sp, input);
        expand_associated_types(sp, rv);
        return rv;
    }

    void expand_associated_types_tp(const Span& sp, ::HIR::TraitPath& input) const;
private:
    void expand_associated_types_params(const Span& sp, ::HIR::PathParams& input) const;
    void expand_associated_types_inner(const Span& sp, ::HIR::TypeRef& input) const;
    bool expand_associated_types__UfcsKnown(const Span& sp, ::HIR::TypeRef& input, bool recurse=true) const;
    bool replace_equalities(::HIR::TypeRef& input) const;

public:
    /// \}

    /// Locate a named trait in the provied trait (either itself or as a parent trait)
    bool find_named_trait_in_trait(const Span& sp,
            const ::HIR::SimplePath& des, const ::HIR::PathParams& params,
            const ::HIR::Trait& trait_ptr, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& pp,
            const ::HIR::TypeRef& self_type,
            ::std::function<bool(const ::HIR::PathParams&, ::HIR::TraitPath::assoc_list_t)> callback
            ) const;
    ///
    bool trait_contains_type(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const char* name,  ::HIR::GenericPath& out_path) const;
    bool iterate_aty_bounds(const Span& sp, const ::HIR::Path::Data::Data_UfcsKnown& pe, ::std::function<bool(const ::HIR::TraitPath&)> cb) const;


    // --------------
    // Common bounds
    // -------------
    bool type_is_copy(const Span& sp, const ::HIR::TypeRef& ty) const;
    bool type_is_clone(const Span& sp, const ::HIR::TypeRef& ty) const; // 1.29
    bool type_is_sized(const Span& sp, const ::HIR::TypeRef& ty) const;
    bool type_is_impossible(const Span& sp, const ::HIR::TypeRef& ty) const;
    bool can_unsize(const Span& sp, const ::HIR::TypeRef& dst, const ::HIR::TypeRef& src) const;
    /// Check if the passed type contains an UnsafeCell (i.e. is interior mutable)
    /// Returns:
    /// - `Fuzzy` if generic (can't know for sure yet)
    /// - `Equal` if it does contain an UnsafeCell
    //  - `Unequal` if it doesn't (shared=immutable)
    HIR::Compare type_is_interior_mutable(const Span& sp, const ::HIR::TypeRef& ty) const;

    MetadataType metadata_type(const Span& sp, const ::HIR::TypeRef& ty, bool err_on_unknown=false) const;

    /// Returns `true` if the passed type either implements Drop, or contains a type that implements Drop
    bool type_needs_drop_glue(const Span& sp, const ::HIR::TypeRef& ty) const;

    const ::HIR::TypeRef* is_type_owned_box(const ::HIR::TypeRef& ty) const;
    const ::HIR::TypeRef* is_type_phantom_data(const ::HIR::TypeRef& ty) const;


    TAGGED_UNION(ValuePtr, NotFound,
    (NotFound, struct{}),
    (NotYetKnown, struct{}),
    (Constant, const ::HIR::Constant*),
    (Static, const ::HIR::Static*),
    (Function, const ::HIR::Function*),
    (EnumConstructor, struct { const ::HIR::Enum* e; size_t v; }),
    (EnumValue, struct { const ::HIR::Enum* e; size_t v; }),
    (StructConstructor, struct { const ::HIR::SimplePath* p; const ::HIR::Struct* s; }),
    (StructConstant, struct { const ::HIR::SimplePath* p; const ::HIR::Struct* s; })
    );

    /// `signature_only` - Returns a pointer to an item with the correct signature, not the actual implementation (faster)
    ValuePtr get_value(const Span& sp, const ::HIR::Path& p, MonomorphState& out_params, bool signature_only=false, const HIR::GenericParams** out_impl_params_def=nullptr) const;
};

