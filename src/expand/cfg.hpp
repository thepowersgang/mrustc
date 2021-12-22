/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/cfg.hpp
 * - Handling of `#[cfg]` and `cfg!` conditions
 */

#pragma once

class TokenStream;
namespace AST {
    class Attribute;
}

extern void Cfg_Dump(::std::ostream& os);
extern void Cfg_SetFlag(::std::string name);
extern void Cfg_SetValue(::std::string name, ::std::string val);
extern void Cfg_SetValueCb(::std::string name, ::std::function<bool(const ::std::string&)> cb);
extern bool check_cfg(const Span& sp, const ::AST::Attribute& mi);
/// Check a parenthesised list of cfg rules (treated as `all()`)
extern bool check_cfg_stream(TokenStream& lex);
/// Parse an attribute from a `cfg_attr()` attribute. Returns with an empty name if check failed
extern std::vector<AST::Attribute> check_cfg_attr(const ::AST::Attribute& mi);
