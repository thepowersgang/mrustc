/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/proc_macro.hpp
 * - Support for the `#[proc_macro_derive]` attribute
 */
#pragma once
#include <parse/tokenstream.hpp>

extern ::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<::std::string>& mac_path, const ::std::string& name, const ::AST::Struct& i);
extern ::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<::std::string>& mac_path, const ::std::string& name, const ::AST::Enum& i);
extern ::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<::std::string>& mac_path, const ::std::string& name, const ::AST::Union& i);
//extern ::std::unique_ptr<TokenStream> ProcMacro_Invoke(const Span& sp, const ::AST::Crate& crate, const ::std::vector<::std::string>& mac_path, const TokenStream& tt);

