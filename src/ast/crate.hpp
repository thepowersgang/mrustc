
#pragma once

#include "ast.hpp"
#include "../types.hpp"

namespace AST {


class ExternCrate;

class Crate:
    public Serialisable
{
public:
    ::std::map< TypeRef, ::std::vector<Impl*> >  m_impl_map;
    ::std::vector<Impl*>    m_impl_index;
    ::std::vector<const ImplDef*> m_neg_impl_index;

    ::AST::MetaItems    m_attrs;
    
    AST::Path   m_lang_item_PhantomFn;
public:
    Module  m_root_module;
    ::std::map< ::std::string, ExternCrate> m_extern_crates;
    // Mapping filled by searching for (?visible) macros with is_pub=true
    ::std::map< ::std::string, const MacroRules*> m_exported_macros;
    
    enum LoadStd {
        LOAD_STD,
        LOAD_CORE,
        LOAD_NONE,
    } m_load_std;
    AST::Path   m_prelude_path;

    Crate();

    Module& root_module() { return m_root_module; }
    Module& get_root_module(const ::std::string& name);
    ::std::map< ::std::string, ExternCrate>& extern_crates() { return m_extern_crates; }   
    
    const Module& root_module() const { return m_root_module; }
    const Module& get_root_module(const ::std::string& name) const;
    const ::std::map< ::std::string, ExternCrate>& extern_crates() const { return m_extern_crates; }   
 
    /// Load referenced crates
    void load_externs();
    
    bool is_trait_implicit(const Path& trait) const;
    

    //::std::vector<ImplRef> find_inherent_impls(const TypeRef& type) const;
    bool find_inherent_impls(const TypeRef& type, ::std::function<bool(const Impl& , ::std::vector<TypeRef> )>) const;
    ::rust::option<ImplRef> find_impl(const Path& trait, const TypeRef& type) const;
    bool find_impl(const Path& trait, const TypeRef& type, Impl** out_impl, ::std::vector<TypeRef>* out_prams=nullptr) const;
    const ::rust::option<Impl&> get_impl(const Path& trait, const TypeRef& type) {
        Impl*   impl_ptr;
        ::std::vector<TypeRef>  params;
        if( find_impl(trait, type, &impl_ptr, &params) ) {
            return ::rust::option<Impl&>( impl_ptr->get_concrete(params) );
        }
        else {
            return ::rust::option<Impl&>();
        }
    }
    Function& lookup_method(const TypeRef& type, const char *name);
    
    void load_extern_crate(::std::string name);
    
    void iterate_functions( fcn_visitor_t* visitor );

    SERIALISABLE_PROTOTYPES();
private:
    bool check_impls_wildcard(const Path& trait, const TypeRef& type) const;
};

/// Representation of an imported crate
/// - Functions are stored as resolved+typechecked ASTs
class ExternCrate:
    public Serialisable
{
    ::std::map< ::std::string, MacroRulesPtr > m_mr_macros;
    
    //::MIR::Module   m_root_module;
    
    //Crate   m_crate;
public:
    ExternCrate();
    ExternCrate(const char *path);
    ExternCrate(const ExternCrate&) = delete;
    ExternCrate(ExternCrate&&) = default;
    
    const MacroRules* find_macro_rules(const ::std::string& name);
    
    //Crate& crate() { return m_crate; }
    //const Crate& crate() const { return m_crate; }
    //Module& root_module() { return m_crate.root_module(); }
    //const Module& root_module() const { return m_crate.root_module(); }
    
    SERIALISABLE_PROTOTYPES();
};

}   // namespace AST
