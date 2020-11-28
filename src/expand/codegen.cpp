/*
* MRustC - Rust Compiler
* - By John Hodge (Mutabah/thePowersGang)
*
* expand/codegen.cpp
* - Attributes that influence codegen and layouts
*/
#include <synext.hpp>
#include <ast/generics.hpp>
#include <ast/ast.hpp>


class CHandler_Inline:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        if( i.is_Function() ) {
        }
        else {
            // TODO: Error
        }
    }
};
STATIC_DECORATOR("inline", CHandler_Inline);
class CHandler_Cold:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        if( i.is_Function() ) {
        }
        else {
            // TODO: Error
        }
    }
};
STATIC_DECORATOR("cold", CHandler_Cold);

class CHandler_Repr:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Types only
    }
};
STATIC_DECORATOR("repr", CHandler_Repr);

class CHandler_RustcNonnullOptimizationGuarantees:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Types only
    }
};
STATIC_DECORATOR("rustc_nonnull_optimization_guaranteed", CHandler_RustcNonnullOptimizationGuarantees);

class CHandler_RustcLayoutScalarValidRangeStart:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Types only
    }
};
STATIC_DECORATOR("rustc_layout_scalar_valid_range_start", CHandler_RustcLayoutScalarValidRangeStart);

class CHandler_LinkName:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        if( i.is_Function() ) {
        }
        else if( i.is_Static() ) {
        }
        else {
            // TODO: Error
        }
    }
};
STATIC_DECORATOR("link_name", CHandler_LinkName);

class CHandler_TargetFeature:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Functions only?
    }
};
STATIC_DECORATOR("target_feature", CHandler_TargetFeature);
