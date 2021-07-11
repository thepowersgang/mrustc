/*
* MRustC - Rust Compiler
* - By John Hodge (Mutabah/thePowersGang)
*
* expand/doc.cpp
* - Documentation comments
*/
#include <synext.hpp>
#include <ast/generics.hpp>


class CDocHandler:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate) const override {
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

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::ExprNode_Match_Arm& expr) const override {
    }
};

STATIC_DECORATOR("doc", CDocHandler);
