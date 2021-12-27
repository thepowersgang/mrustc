/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/crate.hpp
 * - AST::Crate type, and other top-level AST definitions
 */
#pragma once

#include "ast.hpp"
#include "types.hpp"
#include <hir/crate_ptr.hpp>
#include <ast/edition.hpp>

namespace AST {

class ExternCrate;

class TestDesc
{
public:
    ::AST::AbsolutePath path;
    ::std::string   name;
    bool    ignore = false;
    bool    is_benchmark = false;

    enum class ShouldPanic {
        No,
        Yes,
        YesWithMessage,
    } panic_type = ShouldPanic::No;

    ::std::string   expected_panic_message;
};

class ProcMacroDef
{
public:
    RcString    name;
    ::AST::AbsolutePath path;
    ::std::vector<::std::string>    attributes;
};

class Crate
{
public:
    ::AST::AttributeList    m_attrs;

    ::std::map< ::std::string, ::AST::AbsolutePath> m_lang_items;
public:
    Module  m_root_module;

    /// Loaded crates in load order
    ::std::vector<RcString> m_extern_crates_ord;
    ::std::map< RcString, ExternCrate> m_extern_crates;
    // Mapping filled by searching for (?visible) macros with is_pub=true
    ::std::map< RcString, const MacroRules*> m_exported_macros;

    RcString    m_ext_cratename_core;
    RcString    m_ext_cratename_std;
    RcString    m_ext_cratename_procmacro;
    RcString    m_ext_cratename_test;

    // List of tests (populated in expand if --test is passed)
    bool    m_test_harness = false;
    ::std::vector<TestDesc>   m_tests;

    /// Files loaded using things like include! and include_str!
    mutable ::std::vector<::std::string>    m_extra_files;

    // Procedural macros!
    ::std::vector<ProcMacroDef> m_proc_macros;

    AST::Edition    m_edition;
    enum class Type {
        Unknown,
        RustLib,
        RustDylib,
        CDylib,
        Executable,
        ProcMacro,   // Procedural macro
    } m_crate_type = Type::Unknown;
    enum LoadStd {
        LOAD_STD,
        LOAD_CORE,
        LOAD_NONE,
    } m_load_std = LOAD_STD;
    ::std::string   m_crate_name_suffix;    // Suffix (from command-line)
    ::std::string   m_crate_name_set;   // Crate name as set by the user (or auto-detected)
    RcString    m_crate_name_real;  // user name '-' suffix
    AST::Path   m_prelude_path;

    Crate();

    const Module& root_module() const { return m_root_module; }
          Module& root_module()       { return m_root_module; }
    
    void set_crate_name(std::string name) {
        m_crate_name_set = name;
        if( m_crate_type == Type::Executable ) {
            m_crate_name_real = "";
        }
        else {
            m_crate_name_real = m_crate_name_suffix != ""
                ? RcString::new_interned(name + "-" + m_crate_name_suffix)
                : RcString::new_interned(name);
        }
    }

    /// Load referenced crates
    void load_externs();

    /// Load the named crate and returns the crate's unique name
    /// If the parameter `file` is non-empty, only that particular filename will be loaded (from any of the search paths)
    RcString load_extern_crate(Span sp, const RcString& name, const ::std::string& file="");
};

/// Representation of an imported crate
class ExternCrate
{
public:
    RcString    m_name;
    RcString    m_short_name;
    ::std::string   m_filename;
    ::HIR::CratePtr m_hir;

    ExternCrate(const RcString& name, const ::std::string& path);

    ExternCrate(ExternCrate&&) = default;
    ExternCrate& operator=(ExternCrate&&) = default;
    ExternCrate(const ExternCrate&) = delete;
    ExternCrate& operator=(const ExternCrate& ) = delete;
};

extern ::std::vector<::std::string>    g_crate_load_dirs;
extern ::std::map<::std::string, ::std::string>    g_crate_overrides;
extern ::std::map<RcString, RcString>    g_implicit_crates;

}   // namespace AST
