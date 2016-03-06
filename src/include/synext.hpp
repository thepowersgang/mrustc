/*
 */
#pragma once
#ifndef _SYNEXT_HPP_
#define _SYNEXT_HPP_

#include "../ast/item.hpp"

namespace AST {
    class Crate;
    class MetaItem;
    class Path;
    
    class Module;
    class Item;
    
    class ExprNode;
    class Expr;
    
    class MacroInvocation;
}
class TokenTree;


#include "../common.hpp"   // for mv$ and other things
#include <string>
#include <memory>


class ExpandDecorator
{
public:
    virtual bool    expand_before_macros() const = 0;
    
    virtual void    handle(const AST::MetaItem& mi, AST::Crate& crate) const {}
    virtual void    handle(const AST::MetaItem& mi, AST::Crate& crate, AST::MacroInvocation& mac) const {}
    virtual void    handle(const AST::MetaItem& mi, AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item&i) const {}
    virtual void    handle(const AST::MetaItem& mi, AST::ExprNode& expr) const {};
};


class ExpandProcMacro
{
public:
    virtual AST::Expr   expand(const ::std::string& ident, const TokenTree& tt, AST::Module& mod) = 0;
};

#define STATIC_DECORATOR(ident, _handler_class) \
    struct register_##_handler_class##_c {\
        register_##_handler_class##_c() {\
            Register_Synext_Decorator( ident, ::std::unique_ptr<ExpandDecorator>(new _handler_class()) ); \
        } \
    } s_register_##_handler_class;

extern void Register_Synext_Decorator(::std::string name, ::std::unique_ptr<ExpandDecorator> handler);

#endif

