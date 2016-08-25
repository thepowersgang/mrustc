
#pragma once

#include "ast.hpp"
#include "types.hpp"

namespace AST {


class ExternCrate;

class Crate
{
public:
    ::std::map< TypeRef, ::std::vector<Impl*> >  m_impl_map;
    ::std::vector<Impl*>    m_impl_index;
    ::std::vector<const ImplDef*> m_neg_impl_index;

    ::AST::MetaItems    m_attrs;
    
    ::std::map< ::std::string, ::AST::Path> m_lang_items;
public:
    Module  m_root_module;
    ::std::map< ::std::string, ExternCrate> m_extern_crates;
    // Mapping filled by searching for (?visible) macros with is_pub=true
    ::std::map< ::std::string, const MacroRules*> m_exported_macros;
    
    enum class Type {
        Unknown,
        RustLib,
        RustDylib,
        CDylib,
        Executable,
    } m_crate_type = Type::Unknown;
    enum LoadStd {
        LOAD_STD,
        LOAD_CORE,
        LOAD_NONE,
    } m_load_std = LOAD_STD;
    ::std::string   m_crate_name;
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
    
    void load_extern_crate(::std::string name);
};

/// Representation of an imported crate
class ExternCrate
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
};

}   // namespace AST
