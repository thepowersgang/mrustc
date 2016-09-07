
#pragma once

#include "ast.hpp"
#include "types.hpp"
#include <hir/crate_ptr.hpp>

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

    const Module& root_module() const { return m_root_module; }
          Module& root_module()       { return m_root_module; }
 
    /// Load referenced crates
    void load_externs();
    
    void load_extern_crate(Span sp, const ::std::string& name);
};

/// Representation of an imported crate
class ExternCrate
{
public:
    ::std::string   m_name;
    ::HIR::CratePtr m_hir;
    
    ExternCrate(const ::std::string& name, const ::std::string& path);
    
    ExternCrate(ExternCrate&&) = default;
    ExternCrate& operator=(ExternCrate&&) = default;
    ExternCrate(const ExternCrate&) = delete;
    ExternCrate& operator=(const ExternCrate& ) = delete;
    
    void with_all_macros(::std::function<void(const ::std::string& , const MacroRules&)> cb) const;
    const MacroRules* find_macro_rules(const ::std::string& name) const;
};

}   // namespace AST
