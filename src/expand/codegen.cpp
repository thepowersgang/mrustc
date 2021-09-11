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

namespace {
    class Common_Function:
        public ExpandDecorator
    {
    public:
        virtual void handle(AST::Function& fcn) const = 0;

        AttrStage   stage() const override { return AttrStage::Pre; }

        void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
            if( i.is_Function() ) {
                this->handle(i.as_Function());
            }
            else {
                // TODO: Error
            }
        }
        void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, AST::Impl& impl, const RcString& name, slice<const AST::Attribute> attrs, AST::Item&i) const override {
            if( i.is_Function() ) {
                this->handle(i.as_Function());
            }
            else {
                // TODO: Error
            }
        }
        void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, const AST::AbsolutePath& path, AST::Trait& trait, slice<const AST::Attribute> attrs, AST::Item&i) const override {
            if( i.is_Function() ) {
                this->handle(i.as_Function());
            }
            else {
                // TODO: Error
            }
        }
    };
}

class CHandler_Inline:
    public Common_Function
{
public:
    void handle(AST::Function& fcn) const override {
    }
};
STATIC_DECORATOR("inline", CHandler_Inline);
class CHandler_Cold:
    public Common_Function
{
public:
    void handle(AST::Function& fcn) const override {
    }
};
STATIC_DECORATOR("cold", CHandler_Cold);

class CHandler_Repr:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Types only
    }
};
STATIC_DECORATOR("repr", CHandler_Repr);

class CHandler_RustcNonnullOptimizationGuaranteed:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Types only
        if( i.is_Struct() ) {
        }
        else {
        }
    }
};
STATIC_DECORATOR("rustc_nonnull_optimization_guaranteed", CHandler_RustcNonnullOptimizationGuaranteed);

// 1.39
class CHandler_RustcLayoutScalarValidRangeStart:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Types only
        if( auto* s = i.opt_Struct() ) {
            s->m_markings.scalar_valid_start_set = true;
            s->m_markings.scalar_valid_start = std::stoull( mi.data().as_List().sub_items.at(0).string() );
        }
        else {
            TODO(sp, "#[rustc_layout_scalar_valid_range_start] on " << i.tag_str());
        }
    }
};
STATIC_DECORATOR("rustc_layout_scalar_valid_range_start", CHandler_RustcLayoutScalarValidRangeStart);
class CHandler_RustcLayoutScalarValidRangeEnd:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Types only
        if( auto* s = i.opt_Struct() ) {
            const auto& arg = mi.data().as_List().sub_items.at(0).string();
            s->m_markings.scalar_valid_end_set = true;
            try {
                s->m_markings.scalar_valid_end = std::stoull( arg );
            }
            catch(const std::invalid_argument& e) {
                TODO(sp, "Error for stoull failure: " << e.what());
            }
            catch(const std::out_of_range& e) {
                TODO(sp, "Error for stoull failure: " << e.what());
            }
            DEBUG(path << " #[rustc_layout_scalar_valid_range_end]: " << std::hex << s->m_markings.scalar_valid_end);
        }
        else {
            TODO(sp, "#[rustc_layout_scalar_valid_range_end] on " << i.tag_str());
        }
    }
};
STATIC_DECORATOR("rustc_layout_scalar_valid_range_end", CHandler_RustcLayoutScalarValidRangeEnd);

class CHandler_LinkName:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
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

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        // TODO: Functions only?
    }
};
STATIC_DECORATOR("target_feature", CHandler_TargetFeature);
