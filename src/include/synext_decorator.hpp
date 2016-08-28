/*
 */
#pragma once
#ifndef _SYNEXT_DECORATOR_HPP_
#define _SYNEXT_DECORATOR_HPP_

#include <string>
#include <memory>
#include <span.hpp>
#include "../ast/item.hpp"

class TypeRef;
namespace AST {
    class Crate;
    class MetaItem;
    class Path;

    struct StructItem;
    struct TupleItem;
    struct EnumVariant;
    
    class Module;
    class Item;
    class UseStmt;
    
    class Expr;
    class ExprNode;
    struct ExprNode_Match_Arm;
    
    class MacroInvocation;
    
    class ImplDef;
}

enum class AttrStage
{
    EarlyPre,
    EarlyPost,
    LatePre,
    LatePost,
};

class ExpandDecorator
{
    void unexpected(const Span& sp, const AST::MetaItem& mi, const char* loc_str) const;
public:
    virtual AttrStage   stage() const = 0;
    
    virtual void    handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate) const { unexpected(sp, mi, "crate"); }
    virtual void    handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate, AST::MacroInvocation& mac) const { unexpected(sp, mi, "macro invocation"); }
    virtual void    handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate, AST::UseStmt& use) const { unexpected(sp, mi, "use statement"); }
    virtual void    handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item&i) const { unexpected(sp, mi, "item"); }
    // NOTE: To delete, set the type to `_`
    virtual void    handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate, const AST::Module& mod, AST::ImplDef& impl) const { unexpected(sp, mi, "impl"); }
    // NOTE: To delete, clear the name
    virtual void    handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate, ::AST::StructItem& si) const { unexpected(sp, mi, "struct item"); }
    // NOTE: To delete, make the type invalid
    virtual void    handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate, ::AST::TupleItem& si) const { unexpected(sp, mi, "tuple item"); }
    // NOTE: To delete, clear the name
    virtual void    handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate, ::AST::EnumVariant& ev) const { unexpected(sp, mi, "enum variant"); }
    
    virtual void    handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate, ::std::unique_ptr<AST::ExprNode>& expr) const { unexpected(sp, mi, "expression"); }
    // NOTE: To delete, clear the patterns vector
    virtual void    handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate, ::AST::ExprNode_Match_Arm& expr) const { unexpected(sp, mi, "match arm"); }
};

#define STATIC_DECORATOR(ident, _handler_class) \
    struct register_##_handler_class##_c {\
        register_##_handler_class##_c() {\
            Register_Synext_Decorator( ident, ::std::unique_ptr<ExpandDecorator>(new _handler_class()) ); \
        } \
    } s_register_##_handler_class;

extern void Register_Synext_Decorator(::std::string name, ::std::unique_ptr<ExpandDecorator> handler);

#endif

