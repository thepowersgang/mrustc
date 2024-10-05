/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/proc_macro.hpp
 * - Support for the `#[proc_macro_derive]` attribute
 */
#pragma once
#include <parse/tokenstream.hpp>

// Derive macros
extern ::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, slice<const AST::Attribute> attrs, const AST::Visibility& vis, const ::std::string& name, const ::AST::Struct& i);
extern ::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, slice<const AST::Attribute> attrs, const AST::Visibility& vis, const ::std::string& name, const ::AST::Enum& i);
extern ::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, slice<const AST::Attribute> attrs, const AST::Visibility& vis, const ::std::string& name, const ::AST::Union& i);

// Attribute macros
extern ::std::unique_ptr<TokenStream> ProcMacro_Invoke(
    const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, const TokenTree& tt,
    slice<const AST::Attribute> attrs, const AST::Visibility& vis, const ::std::string& item_name, const ::AST::Item& i
    );

// Function-like macros
extern ::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<RcString>& mac_path, const TokenTree& tt);

