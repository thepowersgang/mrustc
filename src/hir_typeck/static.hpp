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

enum class MetadataType {
    Unknown,    // Unknown still
    None,   // Sized pointer
    Zero,   // No metadata, but still unsized
    Slice,  // usize metadata
    TraitObject,    // VTable pointer metadata
};

class StaticTraitResolve
{
public:
    const ::HIR::Crate&   m_crate;

    const ::HIR::GenericParams*   m_impl_generics;
    const ::HIR::GenericParams*   m_item_generics;


    ::std::map< ::HIR::TypeRef, ::HIR::TypeRef> m_type_equalities;

    ::HIR::SimplePath   m_lang_Copy;
    ::HIR::SimplePath   m_lang_Clone;   // 1.29
    ::HIR::SimplePath   m_lang_Drop;
    ::HIR::SimplePath   m_lang_Sized;
    ::HIR::SimplePath   m_lang_Unsize;
    ::HIR::SimplePath   m_lang_Fn;
    ::HIR::SimplePath   m_lang_FnMut;
    ::HIR::SimplePath   m_lang_FnOnce;
    ::HIR::SimplePath   m_lang_Box;
    ::HIR::SimplePath   m_lang_PhantomData;

private:
    mutable ::std::map< ::HIR::TypeRef, bool >  m_copy_cache;
    mutable ::std::map< ::HIR::TypeRef, bool >  m_clone_cache;
    mutable ::std::map< ::HIR::TypeRef, bool >  m_drop_cache;

public:
    StaticTraitResolve(const ::HIR::Crate& crate):
        m_crate(crate),
        m_impl_generics(nullptr),
        m_item_generics(nullptr)
    {
        m_lang_Copy = m_crate.get_lang_item_path_opt("copy");
        if( TARGETVER_1_29 )
            m_lang_Clone = m_crate.get_lang_item_path_opt("clone");
        m_lang_Drop = m_crate.get_lang_item_path_opt("drop");
        m_lang_Sized = m_crate.get_lang_item_path_opt("sized");
        m_lang_Unsize = m_crate.get_lang_item_path_opt("unsize");
        m_lang_Fn = m_crate.get_lang_item_path_opt("fn");
        m_lang_FnMut = m_crate.get_lang_item_path_opt("fn_mut");
        m_lang_FnOnce = m_crate.get_lang_item_path_opt("fn_once");
        m_lang_Box = m_crate.get_lang_item_path_opt("owned_box");
        m_lang_PhantomData = m_crate.get_lang_item_path_opt("phantom_data");
        prep_indexes();
    }

private:
    void prep_indexes();

public:
    bool has_self() const {
        return m_impl_generics ? true : false;
    }
    const ::HIR::GenericParams& impl_generics() const {
        static ::HIR::GenericParams empty;
        return m_impl_generics ? *m_impl_generics : empty;
    }
    const ::HIR::GenericParams& item_generics() const {
        static ::HIR::GenericParams empty;
        return m_item_generics ? *m_item_generics : empty;
    }

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
        m_type_equalities.clear();
        prep_indexes();
    }
    void clear_impl_generics() {
        m_impl_generics = nullptr;
        m_type_equalities.clear();
        prep_indexes();
    }
    void set_item_generics_raw(const ::HIR::GenericParams& gps) {
        assert( !m_item_generics );
        m_item_generics = &gps;
        m_type_equalities.clear();
        prep_indexes();
    }
    void clear_item_generics() {
        m_item_generics = nullptr;
        m_type_equalities.clear();
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
    bool find_impl__check_bound(
        const Span& sp,
        const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
        const ::HIR::TypeRef& type,
        t_cb_find_impl found_cb,
        const ::HIR::GenericBound& bound
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
        ::std::function<bool(::std::vector<const ::HIR::TypeRef*>, ::std::vector<::HIR::TypeRef>, ::HIR::Compare)>
        ) const;
    ::HIR::Compare check_auto_trait_impl_destructure(
        const Span& sp,
        const ::HIR::SimplePath& trait, const ::HIR::PathParams* params_ptr,
        const ::HIR::TypeRef& type
        ) const;

public:

    void expand_associated_types(const Span& sp, ::HIR::TypeRef& input) const;
    bool expand_associated_types_single(const Span& sp, ::HIR::TypeRef& input) const;

private:
    void expand_associated_types_inner(const Span& sp, ::HIR::TypeRef& input) const;
    bool expand_associated_types__UfcsKnown(const Span& sp, ::HIR::TypeRef& input, bool recurse=true) const;
    bool replace_equalities(::HIR::TypeRef& input) const;

public:
    /// \}

    /// Iterate over in-scope bounds (function then top)
    bool iterate_bounds( ::std::function<bool(const ::HIR::GenericBound&)> cb) const;

    /// Locate a named trait in the provied trait (either itself or as a parent trait)
    bool find_named_trait_in_trait(const Span& sp,
            const ::HIR::SimplePath& des, const ::HIR::PathParams& params,
            const ::HIR::Trait& trait_ptr, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& pp,
            const ::HIR::TypeRef& self_type,
            ::std::function<void(const ::HIR::PathParams&, ::std::map< RcString, ::HIR::TypeRef>)> callback
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
    (Constant, const ::HIR::Constant*),
    (Static, const ::HIR::Static*),
    (Function, const ::HIR::Function*),
    (EnumConstructor, struct { const ::HIR::Enum* e; size_t v; }),
    (EnumValue, struct { const ::HIR::Enum* e; size_t v; }),
    (StructConstructor, struct { const ::HIR::SimplePath* p; const ::HIR::Struct* s; }),
    (StructConstant, struct { const ::HIR::SimplePath* p; const ::HIR::Struct* s; })
    );

    /// `signature_only` - Returns a pointer to an item with the correct signature, not the actual implementation (faster)
    ValuePtr get_value(const Span& sp, const ::HIR::Path& p, MonomorphState& out_params, bool signature_only=false) const;
};

