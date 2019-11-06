/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/synext_macro.hpp
 * - Macro-style syntax extensions ( `foo!()` )
 */
#pragma once
#ifndef _SYNEXT_MACRO_HPP_
#define _SYNEXT_MACRO_HPP_

//#include "../common.hpp"   // for mv$ and other things
#include <string>
#include <memory>
#include <span.hpp>

class TypeRef;
namespace AST {
    class Crate;
    class Module;
}
class TokenTree;
class TokenStream;



class ExpandProcMacro
{
public:
    virtual ~ExpandProcMacro() = default;
    virtual ::std::unique_ptr<TokenStream>  expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) = 0;
    virtual ::std::unique_ptr<TokenStream>  expand_ident(const Span& sp, const AST::Crate& crate, const RcString& ident, const TokenTree& tt, AST::Module& mod) {
        ERROR(sp, E0000, "macro doesn't take an identifier");
    }
};

struct MacroDef;
extern void Register_Synext_Macro(::std::string name, ::std::unique_ptr<ExpandProcMacro> handler);
extern void Register_Synext_Macro_Static(MacroDef* def);

struct MacroDef
{
    MacroDef*   prev;
    ::std::string   name;
    ::std::unique_ptr<ExpandProcMacro>  def;
    MacroDef(::std::string name, ::std::unique_ptr<ExpandProcMacro> def) :
        prev(nullptr),
        name(::std::move(name)),
        def(::std::move(def))
    {
        Register_Synext_Macro_Static(this);
    }
};

#define STATIC_MACRO(ident, _handler_class) static MacroDef s_register_##_handler_class(ident, ::std::unique_ptr<ExpandProcMacro>(new _handler_class()));

#endif

