
#pragma once
#include <hir/generic_params.hpp>
#include <hir/type.hpp>

extern bool monomorphise_type_needed(const ::HIR::TypeRef& tpl);
extern bool monomorphise_pathparams_needed(const ::HIR::PathParams& tpl);
static inline bool monomorphise_genericpath_needed(const ::HIR::GenericPath& tpl) {
    return monomorphise_pathparams_needed(tpl.m_params);
}
extern bool monomorphise_path_needed(const ::HIR::Path& tpl);
extern bool monomorphise_traitpath_needed(const ::HIR::TraitPath& tpl);
extern bool monomorphise_type_needed(const ::HIR::TypeRef& tpl);

class Monomorphiser
{
public:
    virtual ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& g) const = 0;
    virtual ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& g) const = 0;

    virtual ::HIR::TypeRef monomorph_type(const Span& sp, const ::HIR::TypeRef& ty, bool allow_infer=true) const;
    ::HIR::Path monomorph_path(const Span& sp, const ::HIR::Path& tpl, bool allow_infer=true) const;
    ::HIR::TraitPath monomorph_traitpath(const Span& sp, const ::HIR::TraitPath& tpl, bool allow_infer) const;
    ::HIR::PathParams monomorph_path_params(const Span& sp, const ::HIR::PathParams& tpl, bool allow_infer) const;
    ::HIR::GenericPath monomorph_genericpath(const Span& sp, const ::HIR::GenericPath& tpl, bool allow_infer) const;

    ::HIR::ArraySize monomorph_arraysize(const Span& sp, const ::HIR::ArraySize& tpl) const;

    const ::HIR::TypeRef& maybe_monomorph_type(const Span& sp, ::HIR::TypeRef& tmp, const ::HIR::TypeRef& ty, bool allow_infer=true) const {
        if( monomorphise_type_needed(ty) ) {
            return tmp = monomorph_type(sp, ty, allow_infer);
        }
        else {
            return ty;
        }
    }
};

class MonomorphiserPP:
    public Monomorphiser
{
public:
    virtual const ::HIR::TypeRef* get_self_type() const = 0;
    virtual const ::HIR::PathParams* get_impl_params() const = 0;
    virtual const ::HIR::PathParams* get_method_params() const = 0;

    ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& ty) const override;
    ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& val) const override;
};
class MonomorphiserNop:
    public Monomorphiser
{
public:
    ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& ty) const override {
        return HIR::TypeRef(ty);
    }
    ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& val) const override {
        return HIR::ConstGeneric(val);
    }
};

// Wrappers to only monomorphise if required
static inline const ::HIR::TypeRef& monomorphise_type_with_opt(const Span& sp, ::HIR::TypeRef& tmp, const ::HIR::TypeRef& tpl, const Monomorphiser& mono, bool allow_infer=true) {
    return (monomorphise_type_needed(tpl) ? tmp = mono.monomorph_type(sp, tpl, allow_infer) : tpl);
}
static inline const ::HIR::Path& monomorphise_path_with_opt(const Span& sp, ::HIR::Path& tmp, const ::HIR::Path& tpl, const Monomorphiser& mono, bool allow_infer=true) {
    return (monomorphise_path_needed(tpl) ? tmp = mono.monomorph_path(sp, tpl, allow_infer) : tpl);
}
static inline const ::HIR::GenericPath& monomorphise_genericpath_with_opt(const Span& sp, ::HIR::GenericPath& tmp, const ::HIR::GenericPath& tpl, const Monomorphiser& mono, bool allow_infer=true) {
    return (monomorphise_genericpath_needed(tpl) ? tmp = mono.monomorph_genericpath(sp, tpl, allow_infer) : tpl);
}
static inline const ::HIR::TraitPath& monomorphise_traitpath_with_opt(const Span& sp, ::HIR::TraitPath& tmp, const ::HIR::TraitPath& tpl, const Monomorphiser& mono, bool allow_infer=true) {
    return (monomorphise_traitpath_needed(tpl) ? tmp = mono.monomorph_traitpath(sp, tpl, allow_infer) : tpl);
}
static inline const ::HIR::PathParams& monomorphise_pathparams_with_opt(const Span& sp, ::HIR::PathParams& tmp, const ::HIR::PathParams& tpl, const Monomorphiser& mono, bool allow_infer=true) {
    return (monomorphise_pathparams_needed(tpl) ? tmp = mono.monomorph_path_params(sp, tpl, allow_infer) : tpl);
}

// Helper for passing a group of params around
struct MonomorphStatePtr:
    public MonomorphiserPP
{
    const ::HIR::TypeRef* self_ty;
    const ::HIR::PathParams*    pp_impl;
    const ::HIR::PathParams*    pp_method;

    MonomorphStatePtr()
        : self_ty(nullptr)
        , pp_impl(nullptr)
        , pp_method(nullptr)
    {
    }
    MonomorphStatePtr(const ::HIR::TypeRef* self_ty, const ::HIR::PathParams* params_i, const ::HIR::PathParams* params_m, const ::HIR::PathParams* params_p=nullptr):
        self_ty(self_ty),
        pp_impl(params_i),
        pp_method(params_m)
    {
    }
    const ::HIR::TypeRef* get_self_type() const override {
        return self_ty;
    }
    const ::HIR::PathParams* get_impl_params() const override {
        return pp_impl;
    }
    const ::HIR::PathParams* get_method_params() const override {
        return pp_method;
    }
};
//extern ::std::ostream& operator<<(::std::ostream& os, const MonomorphStatePtr& ms);

// Helper for passing a group of params around
struct MonomorphState:
    public MonomorphiserPP
{
    ::HIR::TypeRef    self_ty;
    const ::HIR::PathParams*    pp_impl;
    const ::HIR::PathParams*    pp_method;

    ::HIR::PathParams   pp_impl_data;

    MonomorphState():
        self_ty(),
        pp_impl(nullptr),
        pp_method(nullptr)
    {
    }
    MonomorphState(MonomorphState&& x):
        MonomorphState()
    {
        *this = ::std::move(x);
    }

    MonomorphState& operator=(MonomorphState&& x)
    {
        this->self_ty = ::std::move(x.self_ty);
        this->pp_impl = (x.pp_impl == &x.pp_impl_data ? &this->pp_impl_data : x.pp_impl);
        this->pp_impl_data = ::std::move(x.pp_impl_data);
        this->pp_method = x.pp_method;
        return *this;
    }
    MonomorphState clone() const
    {
        MonomorphState  rv;
        rv.self_ty = this->self_ty.clone();
        rv.pp_impl = (this->pp_impl == &this->pp_impl_data ? &rv.pp_impl_data : this->pp_impl);
        rv.pp_impl_data = this->pp_impl_data.clone();
        rv.pp_method = this->pp_method;
        return rv;
    }


    void set_impl_params(HIR::PathParams pp) {
        pp_impl = &pp_impl_data;
        pp_impl_data = std::move(pp);
    }


    bool has_types() const {
        return pp_method->m_types.size() > 0 || pp_impl->m_types.size() > 0;
    }


    const ::HIR::TypeRef* get_self_type() const override {
        return &self_ty;
    }
    const ::HIR::PathParams* get_impl_params() const override {
        return pp_impl;
    }
    const ::HIR::PathParams* get_method_params() const override {
        return pp_method;
    }
};
extern ::std::ostream& operator<<(::std::ostream& os, const MonomorphState& ms);