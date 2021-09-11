/*
* MRustC - Rust Compiler
* - By John Hodge (Mutabah/thePowersGang)
*
* expand/misc_attrs.cpp
* - Miscenaleous attributes that rustc doesn't use (or doesn't use in expand)
*/
#include <synext.hpp>
#include <ast/generics.hpp>
#include <ast/ast.hpp>


// #[must_use] - Marks a type needing to be consumed
class CHandler_MustUse:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: only allowed on types
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, AST::Impl& impl, const RcString& name, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: only allowed on associated types
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::AbsolutePath& path, AST::Trait& trait, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: only allowed on associated types
    }
};
STATIC_DECORATOR("must_use", CHandler_MustUse);

// #[path] - Already used by this stage
class CHandler_Path:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: only allowed on modules
    }
};
STATIC_DECORATOR("path", CHandler_Path);

// #[rustc_promotable] - ?
class CHandler_RustcPromotable:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, AST::Impl& impl, const RcString& name, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: only allowed on functions?
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::AbsolutePath& path, AST::Trait& trait, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: only allowed on functions?
    }
};
STATIC_DECORATOR("rustc_promotable", CHandler_RustcPromotable);


// #[rustc_inherit_overflow_checks]
class CHandler_RustcInheritOverflowChecks:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, AST::Impl& impl, const RcString& name, slice<const AST::Attribute> attrs, AST::Item&i) const override {
    }
    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::AbsolutePath& path, AST::Trait& trait, slice<const AST::Attribute> attrs, AST::Item&i) const override {
    }

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::ExprNodeP& expr) const {
    }
};
STATIC_DECORATOR("rustc_inherit_overflow_checks", CHandler_RustcInheritOverflowChecks);

// #[rustc_on_unimplemented]
class CHandler_RustcOnUnimiplemented:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // Trait only.
    }
};
STATIC_DECORATOR("rustc_on_unimplemented", CHandler_RustcOnUnimiplemented);
