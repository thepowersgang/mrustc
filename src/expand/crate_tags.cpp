/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/crate_type.cpp
 * - #![crate_type] handling
 */
#include <synext.hpp>
#include <ast/crate.hpp>

class Decorator_CrateType:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate) const override {
        if( crate.m_crate_type != AST::Crate::Type::Unknown ) {
            //ERROR(sp, E0000, "Multiple #![crate_type] attributes");
            return ;
        }
        if( !mi.has_string() ) {
            ERROR(sp, E0000, "#![crate_type] requires a string argument");
        }
        const auto& name = mi.string();
        if( name == "rlib" || name == "lib" ) {
            crate.m_crate_type = AST::Crate::Type::RustLib;
        }
        else if( name == "dylib" || name == "rdylib" ) {
            crate.m_crate_type = AST::Crate::Type::RustDylib;
        }
        else {
            ERROR(sp, E0000, "Unknown crate type '" << name << "'");
        }
    }
};

class Decorator_CrateName:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate) const override {
        if( crate.m_crate_name != "" ) {
            ERROR(sp, E0000, "Multiple #![crate_name] attributes");
        }
        if( !mi.has_string() || mi.string() == "" ) {
            ERROR(sp, E0000, "#![crate_name] requires a non-empty string argument");
        }
        crate.m_crate_name = mi.string();
    }
};

STATIC_DECORATOR("crate_type", Decorator_CrateType)
STATIC_DECORATOR("crate_name", Decorator_CrateName)


