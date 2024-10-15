
#pragma once
#include <hir/generic_params.hpp>
#include <hir/type.hpp>
#include <hir/item_path.hpp>

extern bool monomorphise_pathparams_needed(const ::HIR::PathParams& tpl, bool ignore_lifetimes=false);
static inline bool monomorphise_genericpath_needed(const ::HIR::GenericPath& tpl, bool ignore_lifetimes=false) {
    return monomorphise_pathparams_needed(tpl.m_params, ignore_lifetimes);
}
extern bool monomorphise_path_needed(const ::HIR::Path& tpl, bool ignore_lifetimes=false);
extern bool monomorphise_traitpath_needed(const ::HIR::TraitPath& tpl, bool ignore_lifetimes=false);
extern bool monomorphise_type_needed(const ::HIR::TypeRef& tpl, bool ignore_lifetimes=false);

class Monomorphiser
{
    const HIR::Crate*   consteval_crate;
    HIR::ItemPath consteval_path;
    mutable std::vector<const HIR::GenericParams*>  m_hrb_stack;
public:
    Monomorphiser()
        : consteval_crate(nullptr)
        , consteval_path("")
    {
    }
    virtual ~Monomorphiser() = default;
    
    class PopOnDrop {
        friend class Monomorphiser;
        std::vector<const HIR::GenericParams*>& v;
        PopOnDrop(std::vector<const HIR::GenericParams*>& v): v(v) {
        }
    public:
        ~PopOnDrop() {
            v.pop_back();
        }
    };
    PopOnDrop push_hrb(const HIR::GenericParams& params) const {
        m_hrb_stack.push_back(&params);
        return PopOnDrop(m_hrb_stack);
    }

    void set_consteval_state(const HIR::Crate& crate, HIR::ItemPath ip) {
        this->consteval_crate = &crate;
        this->consteval_path = ip;
    }

    virtual ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& g) const = 0;
    virtual ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& g) const = 0;
    virtual ::HIR::LifetimeRef get_lifetime(const Span& sp, const ::HIR::GenericRef& g) const = 0;

    virtual ::HIR::TypeRef monomorph_type(const Span& sp, const ::HIR::TypeRef& ty, bool allow_infer=true) const;
    virtual ::HIR::LifetimeRef monomorph_lifetime(const Span& sp, const ::HIR::LifetimeRef& tpl) const;
    ::HIR::Path monomorph_path(const Span& sp, const ::HIR::Path& tpl, bool allow_infer=true) const;
    ::HIR::TraitPath monomorph_traitpath(const Span& sp, const ::HIR::TraitPath& tpl, bool allow_infer, bool ignore_hrls=false) const;
    ::HIR::PathParams monomorph_path_params(const Span& sp, const ::HIR::PathParams& tpl, bool allow_infer) const;
    virtual ::HIR::GenericPath monomorph_genericpath(const Span& sp, const ::HIR::GenericPath& tpl, bool allow_infer=true, bool ignore_hrls=false) const;

    ::HIR::ConstGeneric monomorph_constgeneric(const Span& sp, const ::HIR::ConstGeneric& val, bool allow_infer) const;
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
    virtual const ::HIR::PathParams* get_hrb_params() const = 0;

    ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& ty) const override;
    ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& val) const override;
    ::HIR::LifetimeRef get_lifetime(const Span& sp, const ::HIR::GenericRef& lft_ref) const override;
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
    ::HIR::LifetimeRef get_lifetime(const Span& sp, const ::HIR::GenericRef& lft_ref) const override {
        return ::HIR::LifetimeRef(lft_ref.binding);
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
    return (monomorphise_genericpath_needed(tpl) ? tmp = mono.monomorph_genericpath(sp, tpl, allow_infer, false) : tpl);
}
static inline const ::HIR::TraitPath& monomorphise_traitpath_with_opt(const Span& sp, ::HIR::TraitPath& tmp, const ::HIR::TraitPath& tpl, const Monomorphiser& mono, bool allow_infer=true) {
    return (monomorphise_traitpath_needed(tpl) ? tmp = mono.monomorph_traitpath(sp, tpl, allow_infer, false) : tpl);
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
    //const ::HIR::PathParams*    pp_placeholder;
    const ::HIR::PathParams*    pp_hrb;

    MonomorphStatePtr()
        : self_ty(nullptr)
        , pp_impl(nullptr)
        , pp_method(nullptr)
        , pp_hrb(nullptr)
    {
    }
    MonomorphStatePtr(const ::HIR::TypeRef* self_ty, const ::HIR::PathParams* params_i, const ::HIR::PathParams* params_m, const ::HIR::PathParams* params_p=nullptr, const ::HIR::PathParams* params_h=nullptr)
        : self_ty(self_ty)
        , pp_impl(params_i)
        , pp_method(params_m)
        //, pp_placeholder(params_p)
        , pp_hrb(params_h)
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
    const ::HIR::PathParams* get_hrb_params() const override {
        return pp_hrb;
    }
};
//extern ::std::ostream& operator<<(::std::ostream& os, const MonomorphStatePtr& ms);

struct MonomorphHrlsOnly:
    public Monomorphiser
{
    const ::HIR::PathParams*    pp_hrb;
    MonomorphHrlsOnly(const ::HIR::PathParams& params_h)
        : pp_hrb(&params_h)
    {
    }
    ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& ty) const override {
        if( ty.group() == 3 ) {
            ASSERT_BUG(sp, ty.idx() < pp_hrb->m_types.size(), ty << " out of bounds (" << pp_hrb->m_types.size() << ")");
            return pp_hrb->m_types.at(ty.idx()).clone();
        }
        return HIR::TypeRef(ty);
    }
    ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& val) const override {
        if( val.group() == 3 ) {
            ASSERT_BUG(sp, val.idx() < pp_hrb->m_values.size(), val << " out of bounds (" << pp_hrb->m_values.size() << ")");
            return pp_hrb->m_values.at(val.idx()).clone();
        }
        return HIR::ConstGeneric(val);
    }
    ::HIR::LifetimeRef get_lifetime(const Span& sp, const ::HIR::GenericRef& lft_ref) const override {
        if( lft_ref.group() == 3 ) {
            ASSERT_BUG(sp, lft_ref.idx() < pp_hrb->m_lifetimes.size(), lft_ref << " out of bounds (" << pp_hrb->m_lifetimes.size() << ")");
            return pp_hrb->m_lifetimes.at(lft_ref.idx());
        }
        return ::HIR::LifetimeRef(lft_ref.binding);
    }
};

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
    const ::HIR::PathParams* get_hrb_params() const override {
        return nullptr;
    }
};
extern ::std::ostream& operator<<(::std::ostream& os, const MonomorphState& ms);
