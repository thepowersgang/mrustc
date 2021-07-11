/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/synext_decorator.hpp
 * - Decorator syntax extensions (#[foo])
 */
#pragma once
#ifndef _SYNEXT_DECORATOR_HPP_
#define _SYNEXT_DECORATOR_HPP_

#include <string>
#include <memory>
#include <span.hpp>
#include <slice.hpp>
#include "../ast/item.hpp"
#include "../ast/expr.hpp"

class TypeRef;
namespace AST {
    class Crate;
    class Attribute;
    class Path;

    struct StructItem;
    struct TupleItem;
    struct EnumVariant;

    class Module;
    class Item;

    class Expr;
    class ExprNode;
    struct ExprNode_Match_Arm;

    class ImplDef;
    class Impl;
}

enum class AttrStage
{
    Pre,
    Post,
};

class ExpandDecorator
{
    void unexpected(const Span& sp, const AST::Attribute& mi, const char* loc_str) const;
public:
    virtual ~ExpandDecorator() = default;
    virtual AttrStage   stage() const = 0;

    virtual void    handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate) const { unexpected(sp, mi, "crate"); }
    virtual void    handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const { unexpected(sp, mi, "item"); }
    virtual void    handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, AST::Impl& impl, const RcString& name, slice<const AST::Attribute> attrs, AST::Item&i) const { unexpected(sp, mi, "associated item"); }
    virtual void    handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::AbsolutePath& path, AST::Trait& trait, slice<const AST::Attribute> attrs, AST::Item&i) const { unexpected(sp, mi, "trait item"); }

    // NOTE: To delete, set the type to `_`
    virtual void    handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::Module& mod, AST::ImplDef& impl) const { unexpected(sp, mi, "impl"); }
    // NOTE: To delete, clear the name
    virtual void    handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::StructItem& si) const { unexpected(sp, mi, "struct item"); }
    // NOTE: To delete, make the type invalid
    virtual void    handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::TupleItem& si) const { unexpected(sp, mi, "tuple item"); }
    // NOTE: To delete, clear the name
    virtual void    handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::EnumVariant& ev) const { unexpected(sp, mi, "enum variant"); }

    virtual void    handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::ExprNodeP& expr) const { unexpected(sp, mi, "expression"); }
    // NOTE: To delete, clear the patterns vector
    virtual void    handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::ExprNode_Match_Arm& expr) const { unexpected(sp, mi, "match arm"); }
    // NOTE: To delete, clear the value
    virtual void    handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::ExprNode_StructLiteral::Ent& expr) const { unexpected(sp, mi, "struct literal ent"); }
};

struct DecoratorDef;
extern void Register_Synext_Decorator(::std::string name, ::std::unique_ptr<ExpandDecorator> handler);
extern void Register_Synext_Decorator_Static(DecoratorDef* def);
template<typename T> void Register_Synext_Decorator_G(::std::string name) {
    Register_Synext_Decorator( mv$(name), ::std::unique_ptr<ExpandDecorator>(new T()) ); 
}

struct DecoratorDef
{
    DecoratorDef*   prev;
    ::std::string   name;
    ::std::unique_ptr<ExpandDecorator>  def;
    DecoratorDef(::std::string name, ::std::unique_ptr<ExpandDecorator> def):
        prev(nullptr),
        name(::std::move(name)),
        def(::std::move(def))
    {
        Register_Synext_Decorator_Static(this);
    }
};

#define STATIC_DECORATOR(ident, _handler_class) static DecoratorDef s_register_##_handler_class ( ident, ::std::unique_ptr<ExpandDecorator>(new _handler_class()) );

#endif

