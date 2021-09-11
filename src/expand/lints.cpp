/*
* MRustC - Rust Compiler
* - By John Hodge (Mutabah/thePowersGang)
*
* expand/lints.cpp
* - Lint attributes
*/
#include <synext.hpp>
#include <ast/generics.hpp>
#include <ast/ast.hpp>


class CMultiHandler_Lint:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate) const {
    }
    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, AST::Impl& impl, const RcString& name, slice<const AST::Attribute> attrs, AST::Item&i) const override {
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::AbsolutePath& path, AST::Trait& trait, slice<const AST::Attribute> attrs, AST::Item&i) const override {
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::Module& mod, AST::ImplDef& impl) const override {
    }

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::StructItem& si) const override {
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::TupleItem& si) const override {
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::EnumVariant& ev) const override {
    }

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::ExprNodeP& expr) const override {
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::ExprNode_Match_Arm& expr) const override { 
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::ExprNode_StructLiteral::Ent& expr) const override { 
    }
};
class CHandler_Allow: public CMultiHandler_Lint {};
STATIC_DECORATOR("allow", CHandler_Allow);
class CHandler_Warn: public CMultiHandler_Lint {};
STATIC_DECORATOR("warn", CHandler_Warn);
class CHandler_Deny: public CMultiHandler_Lint {};
STATIC_DECORATOR("deny", CHandler_Deny);
class CHandler_Forbid: public CMultiHandler_Lint {};
STATIC_DECORATOR("forbid", CHandler_Forbid);
