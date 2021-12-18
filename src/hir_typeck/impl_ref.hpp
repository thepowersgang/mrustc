/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/impl_ref.hpp
 * - Reference to a specific impl block (or the concept of one)
 */
#pragma once

#include <hir/type.hpp>
#include <hir/hir.hpp>
#include <hir_typeck/monomorph.hpp>


namespace HIR {
    class TraitImpl;
}


struct ImplRef
{
    TAGGED_UNION(Data, TraitImpl,
    (TraitImpl, struct {
        HIR::PathParams impl_params;
        const ::HIR::Trait* trait_ptr;
        const ::HIR::SimplePath*    trait_path;
        const ::HIR::TraitImpl* impl;
        mutable ::HIR::TypeRef  self_cache;
        }),
    (BoundedPtr, struct {
        const ::HIR::TypeRef*    type;
        const ::HIR::PathParams* trait_args;
        const ::HIR::TraitPath::assoc_list_t*    assoc;
        }),
    (Bounded, struct {
        ::HIR::TypeRef    type;
        ::HIR::PathParams trait_args;
        ::HIR::TraitPath::assoc_list_t    assoc;
        })
    );

    Data    m_data;

    ImplRef():
        m_data(Data::make_TraitImpl({ {}, nullptr, nullptr, nullptr }))
    {}
    ImplRef(HIR::PathParams impl_params, const HIR::Trait& trait_ref, const ::HIR::SimplePath& trait, const ::HIR::TraitImpl& impl):
        m_data(Data::make_TraitImpl({ mv$(impl_params), &trait_ref, &trait, &impl }))

    {}
    ImplRef(const ::HIR::TypeRef* type, const ::HIR::PathParams* args, const ::HIR::TraitPath::assoc_list_t* assoc):
        m_data(Data::make_BoundedPtr({ type, args, assoc }))
    {}
    ImplRef(::HIR::TypeRef type, ::HIR::PathParams args, ::HIR::TraitPath::assoc_list_t assoc):
        m_data(Data::make_Bounded({ mv$(type), mv$(args), mv$(assoc) }))
    {}

    bool is_valid() const {
        return !(m_data.is_TraitImpl() && m_data.as_TraitImpl().impl == nullptr);
    }

    bool more_specific_than(const ImplRef& other) const;
    bool overlaps_with(const ::HIR::Crate& crate, const ImplRef& other) const;

    bool has_magic_params() const;

    /// HELPER: Returns callback to monomorphise a type using parameters from Data::TraitImpl
    class Monomorph:
        public Monomorphiser
    {
        friend struct ImplRef;
        const ImplRef::Data::Data_TraitImpl& ti;

        Monomorph(const ImplRef::Data::Data_TraitImpl& ti):
            ti(ti)
        {
        }

        ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& ty) const override;
        ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& val) const override;
    };
    Monomorph get_cb_monomorph_traitimpl(const Span& sp) const;

    ::HIR::TypeRef get_impl_type() const;
    ::HIR::PathParams get_trait_params() const;

    ::HIR::TypeRef get_trait_ty_param(unsigned int) const;

    bool type_is_specialisable(const char* name) const;
    ::HIR::TypeRef get_type(const char* name) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const ImplRef& x);
};
