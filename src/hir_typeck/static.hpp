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

class StaticTraitResolve
{
public:
    const ::HIR::Crate&   m_crate;
    
    ::HIR::GenericParams*   m_impl_generics;
    ::HIR::GenericParams*   m_item_generics;
    
    
    ::std::map< ::HIR::TypeRef, ::HIR::TypeRef> m_type_equalities;
private:
    ::HIR::SimplePath   m_lang_Copy;
    
public:
    StaticTraitResolve(const ::HIR::Crate& crate):
        m_crate(crate),
        m_impl_generics(nullptr),
        m_item_generics(nullptr)
    {
        m_lang_Copy = m_crate.get_lang_item_path(Span(), "copy");
        prep_indexes();
    }

private:
    void prep_indexes();
    
public:
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
    template<typename T>
    class NullOnDrop {
        T*& ptr;
    public:
        NullOnDrop(T*& ptr):
            ptr(ptr)
        {}
        ~NullOnDrop() {
            ptr = nullptr;
        }
    };
    NullOnDrop< ::HIR::GenericParams> set_impl_generics(::HIR::GenericParams& gps) {
        assert( !m_impl_generics );
        m_impl_generics = &gps;
        m_type_equalities.clear();
        prep_indexes();
        return NullOnDrop< ::HIR::GenericParams>(m_impl_generics);
    }
    NullOnDrop< ::HIR::GenericParams> set_item_generics(::HIR::GenericParams& gps) {
        assert( !m_item_generics );
        m_item_generics = &gps;
        m_type_equalities.clear();
        prep_indexes();
        return NullOnDrop< ::HIR::GenericParams>(m_item_generics);
    }
    /// \}
    
    /// \brief Lookups
    /// \{
    typedef ::std::function<bool(ImplRef)> t_cb_find_impl;
    
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
        t_cb_find_impl found_cb
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

public:

    void expand_associated_types(const Span& sp, ::HIR::TypeRef& input) const;

private:
    void expand_associated_types__UfcsKnown(const Span& sp, ::HIR::TypeRef& input) const;
    void replace_equalities(::HIR::TypeRef& input) const;

public:
    /// \}
    
    /// Iterate over in-scope bounds (function then top)
    bool iterate_bounds( ::std::function<bool(const ::HIR::GenericBound&)> cb) const;

    /// Locate a named trait in the provied trait (either itself or as a parent trait)
    bool find_named_trait_in_trait(const Span& sp,
            const ::HIR::SimplePath& des, const ::HIR::PathParams& params,
            const ::HIR::Trait& trait_ptr, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& pp,
            const ::HIR::TypeRef& self_type,
            ::std::function<void(const ::HIR::PathParams&, ::std::map< ::std::string, ::HIR::TypeRef>)> callback
            ) const;
    /// 
    bool trait_contains_type(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::std::string& name,  ::HIR::GenericPath& out_path) const;
    
    
    // --------------
    // Common bounds
    // -------------
    bool type_is_copy(const Span& sp, const ::HIR::TypeRef& ty) const;
};

