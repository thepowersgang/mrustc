/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/static.cpp
 * - Non-inferred type checking
 */
#pragma once

#include <hir/hir.hpp>
#include "helpers.hpp"

class StaticTraitResolve
{
public:
    const ::HIR::Crate&   m_crate;
    
    ::HIR::GenericParams*   m_impl_generics;
    ::HIR::GenericParams*   m_item_generics;
public:
    StaticTraitResolve(const ::HIR::Crate& crate):
        m_crate(crate),
        m_impl_generics(nullptr),
        m_item_generics(nullptr)
    {}
    
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
        return NullOnDrop< ::HIR::GenericParams>(m_impl_generics);
    }
    NullOnDrop< ::HIR::GenericParams> set_item_generics(::HIR::GenericParams& gps) {
        assert( !m_item_generics );
        m_item_generics = &gps;
        return NullOnDrop< ::HIR::GenericParams>(m_item_generics);
    }
    /// \}
    
    
    struct ImplRef
    {
        TAGGED_UNION(Data, TraitImpl,
        (TraitImpl, struct {
            ::std::vector<const ::HIR::TypeRef*>   params;
            const ::HIR::TraitImpl* impl;
            }),
        (Bounded, struct {
            const ::HIR::TypeRef*   type;
            const ::HIR::PathParams*    trait_args;
            const ::std::map< ::std::string, ::HIR::TypeRef>*   assoc;
            })
        );
        
        Data    m_data;
        
        ImplRef(::std::vector<const ::HIR::TypeRef*> params, const ::HIR::TraitImpl& impl):
            m_data(Data::make_TraitImpl({ mv$(params), &impl }))
    
        {}
        ImplRef(const ::HIR::TypeRef& type, const ::HIR::PathParams& args, const ::std::map< ::std::string, ::HIR::TypeRef>& assoc):
            m_data(Data::make_Bounded({ &type, &args, &assoc }))
        {}
        
        ::HIR::TypeRef get_type(const char* name) const;
        
        friend ::std::ostream& operator<<(::std::ostream& os, const ImplRef& x) {
            TU_MATCH(Data, (x.m_data), (e),
            (TraitImpl,
                os << "impl" << e.impl->m_params.fmt_args() << " SomeTrait" << e.impl->m_trait_args << " for " << e.impl->m_type << e.impl->m_params.fmt_bounds();
                ),
            (Bounded,
                os << "bound";
                )
            )
            return os;
        }
    };
    
    /// \brief Lookups
    /// \{
    typedef ::std::function<bool(const ImplRef&)> t_cb_find_impl;
    
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

    void expand_associated_types(const Span& sp, ::HIR::TypeRef& input) const;
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
};

