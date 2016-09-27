/*
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
    virtual ::std::unique_ptr<TokenStream>  expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) = 0;
};

#define STATIC_MACRO(ident, _handler_class) \
    struct register_##_handler_class##_c {\
        register_##_handler_class##_c() {\
            Register_Synext_Macro( ident, ::std::unique_ptr<ExpandProcMacro>(new _handler_class()) ); \
        } \
    } s_register_##_handler_class;

extern void Register_Synext_Macro(::std::string name, ::std::unique_ptr<ExpandProcMacro> handler);

#endif

