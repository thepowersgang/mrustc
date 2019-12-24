/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/cfg.hpp
 * - Handling of `#[cfg]` and `cfg!` conditions
 */

#pragma once

#include <ast/attrs.hpp>

extern void Cfg_Dump(::std::ostream& os);
extern void Cfg_SetFlag(::std::string name);
extern void Cfg_SetValue(::std::string name, ::std::string val);
extern void Cfg_SetValueCb(::std::string name, ::std::function<bool(const ::std::string&)> cb);
extern bool check_cfg(const Span& sp, const ::AST::Attribute& mi);
