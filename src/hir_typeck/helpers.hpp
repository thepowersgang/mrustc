#pragma once

#include <hir/type.hpp>
#include <hir/hir.hpp>

// TODO/NOTE - This is identical to ::HIR::t_cb_resolve_type
typedef ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)>   t_cb_generic;

extern bool monomorphise_type_needed(const ::HIR::TypeRef& tpl);
extern bool monomorphise_pathparams_needed(const ::HIR::PathParams& tpl);
extern bool monomorphise_path_needed(const ::HIR::Path& tpl);
extern bool monomorphise_traitpath_needed(const ::HIR::TraitPath& tpl);
extern bool monomorphise_type_needed(const ::HIR::TypeRef& tpl);
extern ::HIR::PathParams monomorphise_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_generic callback, bool allow_infer);
extern ::HIR::GenericPath monomorphise_genericpath_with(const Span& sp, const ::HIR::GenericPath& tpl, t_cb_generic callback, bool allow_infer);
extern ::HIR::TraitPath monomorphise_traitpath_with(const Span& sp, const ::HIR::TraitPath& tpl, t_cb_generic callback, bool allow_infer);
extern ::HIR::TypeRef monomorphise_type_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_generic callback, bool allow_infer=true);
extern ::HIR::TypeRef monomorphise_type(const Span& sp, const ::HIR::GenericParams& params_def, const ::HIR::PathParams& params,  const ::HIR::TypeRef& tpl);

class HMTypeInferrence
{
public:
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
    
    ::std::vector< IVar>    m_ivars;
    bool    m_has_changed;
    
public:
    HMTypeInferrence():
        m_has_changed(false)
    {}
    
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
    void dump() const;
    bool apply_defaults();
    
    void print_type(::std::ostream& os, const ::HIR::TypeRef& tr) const;
    /// Add (and bind) all '_' types in `type`
    void add_ivars(::HIR::TypeRef& type);
    // (helper) Add ivars to path parameters
    void add_ivars_params(::HIR::PathParams& params);
    
    ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)> callback_resolve_infer() const {
        return [&](const auto& ty)->const auto& {
                if( ty.m_data.is_Infer() ) 
                    return this->get_type(ty);
                else
                    return ty;
            };
    }
    
    unsigned int new_ivar();
    ::HIR::TypeRef new_ivar_tr();
    ::HIR::TypeRef& get_type(::HIR::TypeRef& type);
    const ::HIR::TypeRef& get_type(const ::HIR::TypeRef& type) const;
    
    void set_ivar_to(unsigned int slot, ::HIR::TypeRef type);
    void ivar_unify(unsigned int left_slot, unsigned int right_slot);

private:
    IVar& get_pointed_ivar(unsigned int slot) const;
};

