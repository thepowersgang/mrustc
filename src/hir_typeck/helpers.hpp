/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/helpers.hpp
 * - Typecheck dynamic checker
 */
#pragma once

#include <hir/hir.hpp>
#include <hir/expr.hpp> // t_trait_list

#include "common.hpp"
#include "resolve_common.hpp"

static inline bool type_is_unbounded_infer(const ::HIR::TypeRef& ty)
{
    if( const auto* te = ty.data().opt_Infer() ) {
        switch( te->ty_class )
        {
        case ::HIR::InferClass::Integer:    return false;
        case ::HIR::InferClass::Float:      return false;
        case ::HIR::InferClass::None:   return true;
        }
    }
    return false;
}

class HMTypeInferrence
{
public:
    struct FmtType {
        const HMTypeInferrence& ctxt;
        const ::HIR::TypeRef& ty;
        FmtType(const HMTypeInferrence& ctxt, const ::HIR::TypeRef& ty):
            ctxt(ctxt),
            ty(ty)
        {}
        friend ::std::ostream& operator<<(::std::ostream& os, const FmtType& x) {
            x.ctxt.print_type(os, x.ty);
            return os;
        }
    };
    struct FmtPP {
        const HMTypeInferrence& ctxt;
        const ::HIR::PathParams& pps;
        FmtPP(const HMTypeInferrence& ctxt, const ::HIR::PathParams& pps):
            ctxt(ctxt),
            pps(pps)
        {}
        friend ::std::ostream& operator<<(::std::ostream& os, const FmtPP& x) {
            x.ctxt.print_pathparams(os, x.pps);
            return os;
        }
    };

public: // ?? - Needed once, anymore?
    struct IVar
    {
        //bool could_be_diverge;
        unsigned int alias; // If not ~0, this points to another ivar
        ::std::unique_ptr< ::HIR::TypeRef> type;    // Type (only nullptr if alias!=0)

        IVar():
            alias(~0u),
            type(new ::HIR::TypeRef())
        {}
        bool is_alias() const { return alias != ~0u; }
    };

    ::std::vector< IVar>    m_ivars;
    struct IVarValue {
        unsigned int alias;
        ::std::unique_ptr< ::HIR::ConstGeneric> val;
        IVarValue():
            alias(~0u),
            val(new ::HIR::ConstGeneric())
        {}
        bool is_alias() const { return alias != ~0u; }
    };
    ::std::vector< IVarValue>    m_values;

    bool    m_has_changed;

public:
    HMTypeInferrence():
        m_has_changed(false)
    {}

    bool peek_changed() const {
        return m_has_changed;
    }
    bool take_changed() {
        bool rv = m_has_changed;
        m_has_changed = false;
        return rv;
    }
    void mark_change() {
        if( !m_has_changed ) {
            DEBUG("- CHANGE");
            m_has_changed = true;
        }
    }

    void compact_ivars();
    bool apply_defaults();

    void dump() const;

    void print_type(::std::ostream& os, const ::HIR::TypeRef& tr, LList<const ::HIR::TypeRef*> stack = {}) const;
    void print_pathparams(::std::ostream& os, const ::HIR::PathParams& pps, LList<const ::HIR::TypeRef*> stack = {}) const;

    FmtType fmt_type(const ::HIR::TypeRef& tr) const {
        return FmtType(*this, tr);
    }
    FmtPP fmt(const ::HIR::PathParams& v) const {
        return FmtPP(*this, v);
    }

    /// Add (and bind) all '_' types in `type`
    void add_ivars(::HIR::TypeRef& type);
    void add_ivars(::HIR::ConstGeneric& val);
    // (helper) Add ivars to path parameters
    void add_ivars_params(::HIR::PathParams& params);

    ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)> callback_resolve_infer() const {
        return [&](const auto& ty)->const auto& {
                if( ty.data().is_Infer() )
                    return this->get_type(ty);
                else
                    return ty;
            };
    }

    // Mutation
    unsigned int new_ivar(HIR::InferClass ic = HIR::InferClass::None);
    ::HIR::TypeRef new_ivar_tr(HIR::InferClass ic = HIR::InferClass::None);
    void set_ivar_to(unsigned int slot, ::HIR::TypeRef type);
    void ivar_unify(unsigned int left_slot, unsigned int right_slot);

    // 
    unsigned int new_ivar_val();
    void set_ivar_val_to(unsigned int slot, ::HIR::ConstGeneric val);
    void ivar_val_unify(unsigned int left_slot, unsigned int right_slot);

    ::HIR::ConstGeneric new_val_ivar();
    void set_val_ivar_to(unsigned int slot, ::HIR::ConstGeneric val);

    // Lookup
    //::HIR::TypeRef& get_type(::HIR::TypeRef& type);
    const ::HIR::TypeRef& get_type(const ::HIR::TypeRef& type) const;
          ::HIR::TypeRef& get_type(unsigned idx);
    const ::HIR::TypeRef& get_type(unsigned idx) const;

    const ::HIR::ConstGeneric& get_value(const ::HIR::ConstGeneric& val) const;
    const ::HIR::ConstGeneric& get_value(unsigned idx) const;

    void check_for_loops();
    void expand_ivars(::HIR::TypeRef& type);
    void expand_ivars_params(::HIR::PathParams& params);

    // Helpers
    bool pathparams_contain_ivars(const ::HIR::PathParams& pps) const;
    bool type_contains_ivars(const ::HIR::TypeRef& ty) const;
    bool pathparams_equal(const ::HIR::PathParams& pps_l, const ::HIR::PathParams& pps_r) const;
    bool types_equal(const ::HIR::TypeRef& l, const ::HIR::TypeRef& r) const;
private:
    IVar& get_pointed_ivar(unsigned int slot) const;
};

class TraitResolution:
    public TraitResolveCommon
{
    const HIR::SimplePath&  m_lang_Deref;
    const HMTypeInferrence& m_ivars;

    const ::HIR::SimplePath&    m_vis_path;
    const ::HIR::GenericPath*   m_current_trait_path;
    const ::HIR::Trait* m_current_trait_ptr;

    mutable ::std::vector<std::unique_ptr<::HIR::TypeRef>>  m_eat_active_stack;
public:
    TraitResolution(const HMTypeInferrence& ivars, const ::HIR::Crate& crate, const ::HIR::GenericParams* impl_params, const ::HIR::GenericParams* item_params, const ::HIR::SimplePath& vis_path,  const ::HIR::GenericPath* current_trait):
        TraitResolveCommon(crate)
        ,m_lang_Deref(crate.get_lang_item_path_opt("deref"))
        ,m_ivars(ivars)
        ,m_vis_path(vis_path)
        ,m_current_trait_path(current_trait)
        ,m_current_trait_ptr(current_trait ? &crate.get_trait_by_path(Span(), current_trait->m_path) : nullptr)
    {
        m_impl_generics = impl_params;
        m_item_generics = item_params;
        prep_indexes(Span());
    }

    ::HIR::Compare compare_pp(const Span& sp, const ::HIR::PathParams& left, const ::HIR::PathParams& right) const;

    void compact_ivars(HMTypeInferrence& m_ivars);

    bool has_associated_type(const ::HIR::TypeRef& ty) const;
    /// Expand any located associated types in the input, operating in-place and returning the result
    ::HIR::TypeRef expand_associated_types(const Span& sp, ::HIR::TypeRef input) const {
        expand_associated_types_inplace(sp, input, LList<const ::HIR::TypeRef*>());
        return input;
    }

    const ::HIR::TypeRef& expand_associated_types(const Span& sp, const ::HIR::TypeRef& input, ::HIR::TypeRef& tmp) const {
        if( this->has_associated_type(input) ) {
            return (tmp = this->expand_associated_types(sp, input.clone()));
        }
        else {
            return input;
        }
    }

    typedef ::std::function<bool(HIR::Compare cmp, const ::HIR::TypeRef&, const ::HIR::GenericPath& trait_path, const CachedBound& info)> t_cb_bound;
    bool iterate_bounds_traits(const Span& sp, const HIR::TypeRef& type, const HIR::SimplePath& trait, t_cb_bound cb) const;
    bool iterate_bounds_traits(const Span& sp, const HIR::TypeRef& type, t_cb_bound cb) const;
    bool iterate_bounds_traits(const Span& sp, t_cb_bound cb) const;
    bool iterate_aty_bounds(const Span& sp, const ::HIR::Path::Data::Data_UfcsKnown& pe, ::std::function<bool(const ::HIR::TraitPath&)> cb) const;

    typedef ::std::function<bool(const ::HIR::TypeRef&, const ::HIR::PathParams&, const ::HIR::TraitPath::assoc_list_t&)> t_cb_trait_impl;
    typedef ::std::function<bool(ImplRef, ::HIR::Compare)> t_cb_trait_impl_r;

    /// Searches for a trait impl that matches the provided trait name and type
    bool find_trait_impls(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams& params, const ::HIR::TypeRef& type,  t_cb_trait_impl_r callback, bool magic_trait_impls=true) const;

    /// Locate a named trait in the provied trait (either itself or as a parent trait)
    bool find_named_trait_in_trait(const Span& sp,
            const ::HIR::SimplePath& des, const ::HIR::PathParams& params,
            const ::HIR::Trait& trait_ptr, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& pp,
            const ::HIR::TypeRef& self_type,
            t_cb_trait_impl callback
            ) const;
    /// Search for a trait implementation in current bounds
    bool find_trait_impls_bound(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams& params, const ::HIR::TypeRef& type,  t_cb_trait_impl_r callback) const;
    /// Search for a trait implementation in the crate
    bool find_trait_impls_crate(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams& params, const ::HIR::TypeRef& type,  t_cb_trait_impl_r callback) const {
        return find_trait_impls_crate(sp, trait, &params, type, callback);
    }
    /// Search for a trait implementation in the crate (allows nullptr to ignore params)
    bool find_trait_impls_crate(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams* params, const ::HIR::TypeRef& type,  t_cb_trait_impl_r callback) const;
    /// Check for magic (automatically determined) trait implementations
    bool find_trait_impls_magic(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams& params, const ::HIR::TypeRef& type,  t_cb_trait_impl_r callback) const;

    struct RecursionDetected {};

private:
    ::HIR::Compare check_auto_trait_impl_destructure(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams* params_ptr, const ::HIR::TypeRef& type) const;
    ::HIR::Compare ftic_check_params(const Span& sp, const ::HIR::SimplePath& trait,
        const ::HIR::PathParams* params, const ::HIR::TypeRef& type,
        const ::HIR::GenericParams& impl_params_def, const ::HIR::PathParams& impl_trait_args, const ::HIR::TypeRef& impl_ty,
        /*Out->*/ HIR::PathParams& out_impl_params
        ) const ;
public:

    enum class AutoderefBorrow {
        None,
        Shared,
        Unique,
        Owned,
    };
    friend ::std::ostream& operator<<(::std::ostream& os, const AutoderefBorrow& x);
    /// Locate the named method by applying auto-dereferencing.
    /// \return Number of times deref was applied (or ~0 if _ was hit)
    unsigned int autoderef_find_method(const Span& sp,
            const HIR::t_trait_list& traits, const ::std::vector<unsigned>& ivars, const ::HIR::TypeRef& top_ty, const char* method_name,
            /* Out -> */::std::vector<::std::pair<AutoderefBorrow,::HIR::Path>>& possibilities
            ) const;
    /// Locate the named field by applying auto-dereferencing.
    /// \return Number of times deref was applied (or ~0 if _ was hit)
    unsigned int autoderef_find_field(const Span& sp, const ::HIR::TypeRef& top_ty, const char* name,  /* Out -> */::HIR::TypeRef& field_type) const;

    /// Apply an automatic dereference
    const ::HIR::TypeRef* autoderef(const Span& sp, const ::HIR::TypeRef& ty,  ::HIR::TypeRef& tmp_type) const;

    bool find_field(const Span& sp, const ::HIR::TypeRef& ty, const char* name,  /* Out -> */::HIR::TypeRef& field_type) const;

    enum class MethodAccess {
        Shared,
        Unique,
        Move,
    };
private:
    const ::HIR::TypeRef* check_method_receiver(const Span& sp, const ::HIR::Function& fcn, const ::HIR::TypeRef& ty, TraitResolution::MethodAccess access) const;
public:
    enum class AllowedReceivers {
        All,
        AnyBorrow,
        SharedBorrow,
        Value,
        Box,
    };
    friend ::std::ostream& operator<<(::std::ostream& os, const AllowedReceivers& x);
    bool find_method(const Span& sp,
            const HIR::t_trait_list& traits, const ::std::vector<unsigned>& ivars, const ::HIR::TypeRef& ty, const char* method_name, MethodAccess access,
            AutoderefBorrow borrow_type, /* Out -> */::std::vector<::std::pair<AutoderefBorrow,::HIR::Path>>& possibilities
            ) const;

    /// Locates a named method in a trait, and returns the path of the trait that contains it (with fixed parameters)
    const ::HIR::Function* trait_contains_method(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::HIR::TypeRef& self, const char* name,  ::HIR::GenericPath& out_path) const;
    bool trait_contains_type(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const char* name,  ::HIR::GenericPath& out_path) const;

    ::HIR::Compare type_is_sized(const Span& sp, const ::HIR::TypeRef& ty) const;
    ::HIR::Compare type_is_copy(const Span& sp, const ::HIR::TypeRef& ty) const;
    ::HIR::Compare type_is_clone(const Span& sp, const ::HIR::TypeRef& ty) const;

    // If `new_type_callback` is populated, it will be called with the actual/possible dst_type
    // If `infer_callback` is populated, it will be called when either side is an ivar
    ::HIR::Compare can_unsize(const Span& sp, const ::HIR::TypeRef& dst_ty, const ::HIR::TypeRef& src_ty, ::std::function<void(::HIR::TypeRef new_dst)> new_type_callback) const {
        return can_unsize(sp, dst_ty, src_ty, &new_type_callback);
    }
    ::HIR::Compare can_unsize(const Span& sp, const ::HIR::TypeRef& dst_ty, const ::HIR::TypeRef& src_ty, ::std::function<void(::HIR::TypeRef new_dst)>* new_type_callback, ::std::function<void(const ::HIR::TypeRef& dst, const ::HIR::TypeRef& src)>* infer_callback=nullptr) const;

    const ::HIR::TypeRef* type_is_owned_box(const Span& sp, const ::HIR::TypeRef& ty) const;

private:
    void expand_associated_types_inplace(const Span& sp, ::HIR::TypeRef& input, LList<const ::HIR::TypeRef*> stack) const;
    void expand_associated_types_inplace__UfcsKnown(const Span& sp, ::HIR::TypeRef& input, LList<const ::HIR::TypeRef*> stack) const;
};

