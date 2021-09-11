/*
* MRustC - Rust Compiler
* - By John Hodge (Mutabah/thePowersGang)
*
* expand/stability.cpp
* - Item stability
*/
#include <synext.hpp>
#include <ast/generics.hpp>


class CMultiHandler_Stability:
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
};
class CHandler_Stable: public CMultiHandler_Stability {};
STATIC_DECORATOR("stable", CHandler_Stable);
class CHandler_Unstable: public CMultiHandler_Stability {};
STATIC_DECORATOR("unstable", CHandler_Unstable);
class CHandler_RustcDeprecated: public CMultiHandler_Stability {};
STATIC_DECORATOR("rustc_deprecated", CHandler_RustcDeprecated);
// #[rustc_const_unstable] - Unstable in const context
class CHandler_RustcConstUnstable: public CMultiHandler_Stability {};
STATIC_DECORATOR("rustc_const_unstable", CHandler_RustcConstUnstable);

class CHandler_AllowInternalUnstable:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::Module& mod, AST::ImplDef& impl) const override {
    }
};
STATIC_DECORATOR("allow_internal_unstable", CHandler_AllowInternalUnstable);