/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/resolve_common.hpp
 * - Helper class for logic shared between "static" and "dynamic" type checking/resolution
 */
#pragma once

#include <map>
#include <memory>
#include <hir/hir.hpp>
#include <hir/type_ref.hpp>
#include <hir/path.hpp>
#include <hir/generic_params.hpp>
#include <range_vec_map.hpp>

struct TraitResolveCommon
{
    const ::HIR::Crate&   m_crate;

    const ::HIR::GenericParams*   m_impl_generics;
    const ::HIR::GenericParams*   m_item_generics;

    ::std::map< ::HIR::TypeRef, ::HIR::TypeRef> m_type_equalities;
    // A pre-calculated list of trait bounds
    struct CachedBound {
        const HIR::Trait*   trait_ptr;
        HIR::TraitPath::assoc_list_t    assoc;
    };
    struct CachedBoundCmp {
        typedef std::pair< ::HIR::TypeRef, ::HIR::GenericPath>  key_t;
        typedef std::pair< const ::HIR::TypeRef&, const ::HIR::GenericPath&>  ref_t;
        typedef std::pair< const ::HIR::TypeRef&, const ::HIR::SimplePath&>  ref_sp_t;
        Ordering ord(const key_t& a, const key_t& b) const {
            return ::ord(a,b);
        }
        bool operator()(const key_t& a, const key_t& b) const {
            return ord(a,b) == OrdLess;
        }
        Ordering ord(const key_t& a, const ref_t& b) const {
            ORD(a.first, b.first);
            ORD(a.second, b.second);
            return OrdEqual;
        }
        bool operator()(const key_t& a, const ref_t& b) const {
            return ord(a,b) == OrdLess;
        }
        bool operator()(const ref_t& a, const key_t& b) const {
            return ord(b,a) == OrdGreater;
        }
        Ordering ord(const key_t& a, const ref_sp_t& b) const {
            ORD(a.first, b.first);
            ORD(a.second.m_path, b.second);
            return OrdEqual;
        }
        bool operator()(const key_t& a, const ref_sp_t& b) const {
            return ord(a,b) == OrdLess;
        }
        bool operator()(const ref_sp_t& a, const key_t& b) const {
            return ord(b,a) == OrdGreater;
        }
    };
    typedef RangeVecMap< std::pair< ::HIR::TypeRef, ::HIR::GenericPath>, CachedBound, CachedBoundCmp> cached_bounds_t;
    cached_bounds_t m_trait_bounds;

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
    ::HIR::SimplePath   m_lang_Generator;   // 1.39
    ::HIR::SimplePath   m_lang_DiscriminantKind;    // 1.54
    ::HIR::SimplePath   m_lang_Pointee;    // 1.54
    ::HIR::SimplePath   m_lang_DynMetadata;    // 1.54

    TraitResolveCommon(const ::HIR::Crate& crate):
        m_crate(crate),
        m_impl_generics(nullptr),
        m_item_generics(nullptr)
    {
        m_lang_Copy = m_crate.get_lang_item_path_opt("copy");
        if( TARGETVER_LEAST_1_29 )
            m_lang_Clone = m_crate.get_lang_item_path_opt("clone");
        m_lang_Drop = m_crate.get_lang_item_path_opt("drop");
        m_lang_Sized = m_crate.get_lang_item_path_opt("sized");
        m_lang_Unsize = m_crate.get_lang_item_path_opt("unsize");
        m_lang_Fn = m_crate.get_lang_item_path_opt("fn");
        m_lang_FnMut = m_crate.get_lang_item_path_opt("fn_mut");
        m_lang_FnOnce = m_crate.get_lang_item_path_opt("fn_once");
        m_lang_Box = m_crate.get_lang_item_path_opt("owned_box");
        m_lang_PhantomData = m_crate.get_lang_item_path_opt("phantom_data");
        m_lang_Generator = m_crate.get_lang_item_path_opt("generator");
        m_lang_DiscriminantKind = m_crate.get_lang_item_path_opt("discriminant_kind");
        m_lang_Pointee = m_crate.get_lang_item_path_opt("pointee_trait");
        m_lang_DynMetadata = m_crate.get_lang_item_path_opt("dyn_metadata");
    }

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

    /// <summary>
    /// Obtain the type for a given constant parameter
    /// </summary>
    const ::HIR::TypeRef& get_const_param_type(const Span& sp, unsigned binding) const;


    void prep_indexes(const Span& sp);

private:
    void prep_indexes__add_equality(const Span& sp, ::HIR::TypeRef long_ty, ::HIR::TypeRef short_ty);
    void prep_indexes__add_trait_bound(const Span& sp, ::HIR::TypeRef type, ::HIR::TraitPath trait_path, bool add_parents=true);


    /// Iterate over in-scope bounds (function then type)
    bool iterate_bounds( ::std::function<bool(const ::HIR::GenericBound&)> cb) const {
        const ::HIR::GenericParams* v[2] = { m_item_generics, m_impl_generics };
        for(auto p : v) {
            if( !p )    continue ;
            for(const auto& b : p->m_bounds)
                if(cb(b))   return true;
        }
        return false;
    }
};
