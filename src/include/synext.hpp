/*
 */
#pragma once
#ifndef _SYNEXT_HPP_
#define _SYNEXT_HPP_

#include "../ast/item.hpp"
#include <span.hpp>

class TypeRef;
namespace AST {
    class Crate;
    class MetaItem;
    class Path;

    class StructItem;
    class TupleItem;
    class EnumVariant;
    
    class Module;
    class Item;
    
    class Expr;
    class ExprNode;
    class ExprNode_Match_Arm;
    
    class MacroInvocation;
    
    class ImplDef;
}
class TokenTree;
class TokenStream;


#include "../common.hpp"   // for mv$ and other things
#include <string>
#include <memory>

enum class AttrStage
{
    EarlyPre,
    EarlyPost,
    LatePre,
    LatePost,
};

class ExpandDecorator
{
    void unexpected(const AST::MetaItem& mi, const char* loc_str) const;
public:
    virtual AttrStage   stage() const = 0;
    
    virtual void    handle(const AST::MetaItem& mi, AST::Crate& crate) const { unexpected(mi, "crate"); }
    virtual void    handle(const AST::MetaItem& mi, AST::Crate& crate, AST::MacroInvocation& mac) const { unexpected(mi, "macro invocation"); }
    virtual void    handle(const AST::MetaItem& mi, AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item&i) const { unexpected(mi, "item"); }
    // NOTE: To delete, set the type to `_`
    virtual void    handle(const AST::MetaItem& mi, AST::Crate& crate, const AST::Module& mod, AST::ImplDef& impl) const { unexpected(mi, "impl"); }
    // NOTE: To delete, clear the name
    virtual void    handle(const AST::MetaItem& mi, AST::Crate& crate, ::AST::StructItem& si) const { unexpected(mi, "struct item"); }
    // NOTE: To delete, make the type invalid
    virtual void    handle(const AST::MetaItem& mi, AST::Crate& crate, ::AST::TupleItem& si) const { unexpected(mi, "tuple item"); }
    // NOTE: To delete, clear the name
    virtual void    handle(const AST::MetaItem& mi, AST::Crate& crate, ::AST::EnumVariant& ev) const { unexpected(mi, "enum variant"); }
    
    virtual void    handle(const AST::MetaItem& mi, AST::Crate& crate, ::std::unique_ptr<AST::ExprNode>& expr) const { unexpected(mi, "expression"); }
    // NOTE: To delete, clear the patterns vector
    virtual void    handle(const AST::MetaItem& mi, AST::Crate& crate, ::AST::ExprNode_Match_Arm& expr) const { unexpected(mi, "match arm"); }
};

class ExpandProcMacro
{
public:
    virtual bool    expand_early() const = 0;
    
    virtual ::std::unique_ptr<TokenStream>  expand(Span sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) = 0;
};

#define STATIC_DECORATOR(ident, _handler_class) \
    struct register_##_handler_class##_c {\
        register_##_handler_class##_c() {\
            Register_Synext_Decorator( ident, ::std::unique_ptr<ExpandDecorator>(new _handler_class()) ); \
        } \
    } s_register_##_handler_class;
#define STATIC_MACRO(ident, _handler_class) \
    struct register_##_handler_class##_c {\
        register_##_handler_class##_c() {\
            Register_Synext_Macro( ident, ::std::unique_ptr<ExpandProcMacro>(new _handler_class()) ); \
        } \
    } s_register_##_handler_class;

extern void Register_Synext_Decorator(::std::string name, ::std::unique_ptr<ExpandDecorator> handler);
extern void Register_Synext_Macro(::std::string name, ::std::unique_ptr<ExpandProcMacro> handler);


extern void Expand_Expr(bool is_early, ::AST::Crate& crate, LList<const AST::Module*> modstack, ::std::unique_ptr<AST::ExprNode>& node);

#endif

