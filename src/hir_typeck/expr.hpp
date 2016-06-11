/*
 */
#pragma once
#include <hir/expr.hpp>
#include <hir/hir.hpp>
#include <hir/visitor.hpp>

namespace typeck {

// TODO/NOTE - This is identical to ::HIR::t_cb_resolve_type
typedef ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)>   t_cb_generic;

extern bool monomorphise_type_needed(const ::HIR::TypeRef& tpl);
extern bool monomorphise_pathparams_needed(const ::HIR::PathParams& tpl);
extern bool monomorphise_path_needed(const ::HIR::Path& tpl);

extern ::HIR::PathParams monomorphise_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_generic callback, bool allow_infer);
extern ::HIR::TraitPath monomorphise_traitpath_with(const Span& sp, const ::HIR::TraitPath& tpl, t_cb_generic callback, bool allow_infer);
extern ::HIR::TypeRef monomorphise_type_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_generic callback, bool allow_infer=true);
extern ::HIR::TypeRef monomorphise_type(const Span& sp, const ::HIR::GenericParams& params_def, const ::HIR::PathParams& params,  const ::HIR::TypeRef& tpl);

extern void check_type_class_primitive(const Span& sp, const ::HIR::TypeRef& type, ::HIR::InferClass ic, ::HIR::CoreType ct);

class TypecheckContext
{
    struct IVar
    {
        unsigned int alias; // If not ~0, this points to another ivar
        ::std::unique_ptr< ::HIR::TypeRef> type;    // Type (only nullptr if alias!=0)
        
        IVar():
            alias(~0u),
            type(new ::HIR::TypeRef())
        {}
        bool is_alias() const { return alias != ~0u; }
    };
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
    ::std::vector< IVar>    m_ivars;
    bool    m_has_changed;
    
    const ::HIR::GenericParams* m_impl_params;
    const ::HIR::GenericParams* m_item_params;
    
public:
    TypecheckContext(const ::HIR::Crate& crate, const ::HIR::GenericParams* impl_params, const ::HIR::GenericParams* item_params):
        m_crate(crate),
        m_has_changed(false),
        m_impl_params( impl_params ),
        m_item_params( item_params )
    {
    }
    
    void dump() const;
    
    bool take_changed() {
        bool rv = m_has_changed;
        m_has_changed = false;
        return rv;
    }
    void mark_change() {
        DEBUG("- CHANGE");
        m_has_changed = true;
    }
    
    void compact_ivars();
    /// Apply defaults (i32 or f64), returns true if a default was applied
    bool apply_defaults();
    
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
    
    /// Searches for a trait impl that matches the provided trait name and type
    bool find_trait_impls(const ::HIR::SimplePath& trait, const ::HIR::TypeRef& type,  ::std::function<bool(const ::HIR::PathParams&)> callback) const;
    
    typedef ::std::function<bool(const ::HIR::PathParams&, const ::std::map< ::std::string,::HIR::TypeRef>&)> t_cb_trait_impl;
    
    /// Locate a named trait in the provied trait (either itself or as a parent trait)
    bool find_named_trait_in_trait(const Span& sp,
            const ::HIR::SimplePath& des,
            const ::HIR::Trait& trait_ptr, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& pp,
            const ::HIR::TypeRef& self_type,
            t_cb_trait_impl callback
            ) const;
    /// Search for a trait implementation in current bounds
    bool find_trait_impls_bound(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::TypeRef& type,  t_cb_trait_impl callback) const;
    /// Search for a trait implementation in the crate
    bool find_trait_impls_crate(const ::HIR::SimplePath& trait, const ::HIR::TypeRef& type,  ::std::function<bool(const ::HIR::PathParams&)> callback) const;
    
    /// Locates a named method in a trait, and returns the path of the trait that contains it (with fixed parameters)
    bool trait_contains_method(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::std::string& name,  ::HIR::GenericPath& out_path) const;
    
    /// Locate the named method by applying auto-dereferencing.
    /// \return Number of times deref was applied (or ~0 if _ was hit)
    unsigned int autoderef_find_method(const Span& sp, const ::HIR::TypeRef& top_ty, const ::std::string& method_name,  /* Out -> */::HIR::Path& fcn_path) const;
    bool find_method(const Span& sp, const ::HIR::TypeRef& ty, const ::std::string& method_name,  /* Out -> */::HIR::Path& fcn_path) const;
    
    unsigned int autoderef_find_field(const Span& sp, const ::HIR::TypeRef& top_ty, const ::std::string& name,  /* Out -> */::HIR::TypeRef& field_type) const;
    bool find_field(const Span& sp, const ::HIR::TypeRef& ty, const ::std::string& name,  /* Out -> */::HIR::TypeRef& field_type) const;
    
public:
    ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)> callback_resolve_infer() {
        return [&](const auto& ty)->const auto& {
                if( ty.m_data.is_Infer() ) 
                    return this->get_type(ty);
                else
                    return ty;
            };
    }
    
    unsigned int new_ivar()
    {
        m_ivars.push_back( IVar() );
        m_ivars.back().type->m_data.as_Infer().index = m_ivars.size() - 1;
        return m_ivars.size() - 1;
    }
    ::HIR::TypeRef new_ivar_tr() {
        ::HIR::TypeRef rv;
        rv.m_data.as_Infer().index = this->new_ivar();
        return rv;
    }
    
    ::HIR::TypeRef& get_type(::HIR::TypeRef& type)
    {
        TU_IFLET(::HIR::TypeRef::Data, type.m_data, Infer, e,
            assert(e.index != ~0u);
            return *get_pointed_ivar(e.index).type;
        )
        else {
            return type;
        }
    }
    const ::HIR::TypeRef& get_type(const ::HIR::TypeRef& type) const
    {
        TU_IFLET(::HIR::TypeRef::Data, type.m_data, Infer, e,
            assert(e.index != ~0u);
            return *get_pointed_ivar(e.index).type;
        )
        else {
            return type;
        }
    }

private:
    void set_ivar_to(unsigned int slot, ::HIR::TypeRef type);
    void ivar_unify(unsigned int left_slot, unsigned int right_slot);
    IVar& get_pointed_ivar(unsigned int slot) const;
};

}   // namespace typeck

