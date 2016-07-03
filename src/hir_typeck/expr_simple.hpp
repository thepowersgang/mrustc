/*
 */
#pragma once
#include <hir/expr.hpp>
#include <hir/hir.hpp>
#include <hir/visitor.hpp>

#define IDENT_CR  ([](const auto& v)->const auto&{return v;})

#include "helpers.hpp"

namespace typeck {

extern void check_type_class_primitive(const Span& sp, const ::HIR::TypeRef& type, ::HIR::InferClass ic, ::HIR::CoreType ct);

class TypecheckContext
{
    struct Variable
    {
        ::std::string   name;
        ::HIR::TypeRef  type;
        
        Variable()
        {}
        Variable(const ::std::string& name, ::HIR::TypeRef type):
            name( name ),
            type( mv$(type) )
        {}
        Variable(Variable&&) = default;
        
        Variable& operator=(Variable&&) = default;
    };
public:
    const ::HIR::Crate& m_crate;
    ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   m_traits;
private:
    ::std::vector< Variable>    m_locals;
    HMTypeInferrence    m_ivars;
    
    const ::HIR::GenericParams* m_impl_params;
    const ::HIR::GenericParams* m_item_params;
    
public:
    TypecheckContext(const ::HIR::Crate& crate, const ::HIR::GenericParams* impl_params, const ::HIR::GenericParams* item_params):
        m_crate(crate),
        m_impl_params( impl_params ),
        m_item_params( item_params )
    {
    }
    
    void dump() const;
    
    bool take_changed() {
        return m_ivars.take_changed();
    }
    void mark_change() {
        m_ivars.mark_change();
    }
    
    void push_traits(const ::std::vector<::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >& list);
    void pop_traits(const ::std::vector<::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >& list);

    void compact_ivars();
    /// Apply defaults (i32 or f64), returns true if a default was applied
    bool apply_defaults();
    
    bool pathparams_contain_ivars(const ::HIR::PathParams& pps) const;
    bool type_contains_ivars(const ::HIR::TypeRef& ty) const;
    bool pathparams_equal(const ::HIR::PathParams& pps_l, const ::HIR::PathParams& pps_r) const;
    bool types_equal(const ::HIR::TypeRef& l, const ::HIR::TypeRef& r) const;
    
    void print_type(::std::ostream& os, const ::HIR::TypeRef& tr) const;
    
    /// Adds a local variable binding (type is mutable so it can be inferred if required)
    void add_local(unsigned int index, const ::std::string& name, ::HIR::TypeRef type);

    /// Get the type associated with a variable
    const ::HIR::TypeRef& get_var_type(const Span& sp, unsigned int index);

    /// Add (and bind) all '_' types in `type`
    void add_ivars(::HIR::TypeRef& type);
    // (helper) Add ivars to path parameters
    void add_ivars_params(::HIR::PathParams& params);
    
    // (helper) Add a new local based on the pattern binding
    void add_pattern_binding(const ::HIR::PatternBinding& pb, ::HIR::TypeRef type);
    // (first pass) Add locals using the pssed pattern
    void add_binding(const Span& sp, ::HIR::Pattern& pat, ::HIR::TypeRef& type);
    
    /// Run inferrence using a pattern
    void apply_pattern(const ::HIR::Pattern& pat, ::HIR::TypeRef& type);
    
    /// (wrapper)
    void apply_equality(const Span& sp, const ::HIR::TypeRef& left, const ::HIR::TypeRef& right, ::HIR::ExprNodeP* node_ptr_ptr = nullptr) {
        apply_equality(sp, left, [](const auto& x)->const auto&{return x;}, right, [](const auto& x)->const auto&{return x;}, node_ptr_ptr);
    }
    
    /// (helper) Expands a top-level associated type into `tmp_t`, returning either `t` or `tmp_t`
    const ::HIR::TypeRef& expand_associated_types_to(const Span& sp, const ::HIR::TypeRef& t, ::HIR::TypeRef& tmp_t) const;
    
    /// Equates the two types, checking that they are equal (and binding ivars)
    /// \note !! The ordering DOES matter, as the righthand side will get unsizing/deref coercions applied if possible (using node_ptr_ptr)
    /// \param sp   Span for reporting errors
    /// \param left     Lefthand type (destination for coercions)
    /// \param right    Righthand type (source for coercions)
    /// \param node_ptr Pointer to ExprNodeP, updated with new nodes for coercions
    void apply_equality(const Span& sp, const ::HIR::TypeRef& left, t_cb_generic cb_left, const ::HIR::TypeRef& right, t_cb_generic cb_right, ::HIR::ExprNodeP* node_ptr_ptr);
    
    /// Check if a trait bound applies, using the passed function to expand Generic/Infer types
    bool check_trait_bound(const Span& sp, const ::HIR::TypeRef& type, const ::HIR::GenericPath& trait, t_cb_generic placeholder) const;
    
    /// Expand any located associated types in the input, operating in-place and returning the result
    ::HIR::TypeRef expand_associated_types(const Span& sp, ::HIR::TypeRef input) const;
    
    /// Iterate over in-scope bounds (function then top)
    bool iterate_bounds( ::std::function<bool(const ::HIR::GenericBound&)> cb) const;
    
    typedef ::std::function<bool(const ::HIR::PathParams&, const ::std::map< ::std::string,::HIR::TypeRef>&)> t_cb_trait_impl;

    /// Searches for a trait impl that matches the provided trait name and type
    bool find_trait_impls(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams& params, const ::HIR::TypeRef& type,  t_cb_trait_impl callback) const;
    
    /// Locate a named trait in the provied trait (either itself or as a parent trait)
    bool find_named_trait_in_trait(const Span& sp,
            const ::HIR::SimplePath& des, const ::HIR::PathParams& params,
            const ::HIR::Trait& trait_ptr, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& pp,
            const ::HIR::TypeRef& self_type,
            t_cb_trait_impl callback
            ) const;
    /// Search for a trait implementation in current bounds
    bool find_trait_impls_bound(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams& params, const ::HIR::TypeRef& type,  t_cb_trait_impl callback) const;
    /// Search for a trait implementation in the crate
    bool find_trait_impls_crate(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams& params, const ::HIR::TypeRef& type,  t_cb_trait_impl callback) const;
    
    /// Locates a named method in a trait, and returns the path of the trait that contains it (with fixed parameters)
    bool trait_contains_method(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::std::string& name,  ::HIR::GenericPath& out_path) const;
    bool trait_contains_type(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::std::string& name,  ::HIR::GenericPath& out_path) const;
    
    const ::HIR::TypeRef* autoderef(const Span& sp, const ::HIR::TypeRef& ty,  ::HIR::TypeRef& tmp_type) const;
    /// Locate the named method by applying auto-dereferencing.
    /// \return Number of times deref was applied (or ~0 if _ was hit)
    unsigned int autoderef_find_method(const Span& sp, const ::HIR::TypeRef& top_ty, const ::std::string& method_name,  /* Out -> */::HIR::Path& fcn_path) const;
    bool find_method(const Span& sp, const ::HIR::TypeRef& ty, const ::std::string& method_name,  /* Out -> */::HIR::Path& fcn_path) const;
    
    unsigned int autoderef_find_field(const Span& sp, const ::HIR::TypeRef& top_ty, const ::std::string& name,  /* Out -> */::HIR::TypeRef& field_type) const;
    bool find_field(const Span& sp, const ::HIR::TypeRef& ty, const ::std::string& name,  /* Out -> */::HIR::TypeRef& field_type) const;
    
public:
    ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)> callback_resolve_infer() const {
        return [&](const auto& ty)->const auto& {
                if( ty.m_data.is_Infer() ) 
                    return this->get_type(ty);
                else
                    return ty;
            };
    }
    
    unsigned int new_ivar() {
        return m_ivars.new_ivar();
    }
    ::HIR::TypeRef new_ivar_tr() {
        return m_ivars.new_ivar_tr();
    }
    
    ::HIR::TypeRef& get_type(::HIR::TypeRef& type) {
        return m_ivars.get_type(type);
    }
    const ::HIR::TypeRef& get_type(const ::HIR::TypeRef& type) const {
        return m_ivars.get_type(type);
    }

private:
    void set_ivar_to(unsigned int slot, ::HIR::TypeRef type) {
        m_ivars.set_ivar_to(slot, mv$(type));
    }
    void ivar_unify(unsigned int left_slot, unsigned int right_slot) {
        m_ivars.ivar_unify(left_slot, right_slot);
    }
};

}   // namespace typeck

