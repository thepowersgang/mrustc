/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/std_prelude.cpp
 * - Handling of no_std/no_core/no_prelude
 */
#include <synext.hpp>
#include <ast/crate.hpp>

class Decorator_NoStd:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate) const override {
        if( crate.m_load_std != AST::Crate::LOAD_STD && crate.m_load_std != AST::Crate::LOAD_CORE ) {
            WARNING(sp, W0000, "Use of #![no_std] with itself or #![no_core]");
            return ;
        }
        crate.m_load_std = AST::Crate::LOAD_CORE;
    }
};
class Decorator_NoCore:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate) const override {
        if( crate.m_load_std != AST::Crate::LOAD_STD && crate.m_load_std != AST::Crate::LOAD_NONE ) {
            WARNING(sp, W0000, "Use of #![no_core] with itself or #![no_std]");
        }
        crate.m_load_std = AST::Crate::LOAD_NONE;
    }
};
//class Decorator_Prelude:
//    public ExpandDecorator
//{
//public:
//    AttrStage stage() const override { return AttrStage::Pre; }
//};

class Decorator_NoPrelude:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item&i) const override {
        if( i.is_Module() ) {
            i.as_Module().m_insert_prelude = false;
        }
        else {
            ERROR(sp, E0000, "Invalid use of #[no_prelude] on non-module");
        }
    }
};

class Decorator_PreludeImport:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, const AST::Visibility& vis, AST::Item&i) const override {
        if( const auto* e = i.opt_Use() ) {
            if(e->entries.size() != 1)
                ERROR(sp, E0000, "#[prelude_import] should be on a single-entry use");
            ASSERT_BUG(sp, path.nodes.size() > 0, path);
            ASSERT_BUG(sp, path.nodes.back() == "", path);
            if(e->entries.front().name != "")
                ERROR(sp, E0000, "#[prelude_import] should be on a glob");
            const auto& p = e->entries.front().path;
            // TODO: Ensure that this statement is a glob (has a name of "")
            if(p.is_relative())
            {
                crate.m_prelude_path = AST::Path( path );
                crate.m_prelude_path.nodes().pop_back();
                crate.m_prelude_path += p;
            }
            else
            {
                crate.m_prelude_path = AST::Path(p);
            }
        }
        else {
            ERROR(sp, E0000, "Invalid use of #[prelude_import] on non-use");
        }
    }
};

void Expand_init_std_prelude() {
    Register_Synext_Decorator_G<Decorator_NoStd>("no_std");
    Register_Synext_Decorator_G<Decorator_NoCore>("no_core");
    //Register_Synext_Decorator_G<Decorator_Prelude>("prelude");
    Register_Synext_Decorator_G<Decorator_PreludeImport>("prelude_import");
    Register_Synext_Decorator_G<Decorator_NoPrelude>("no_prelude");
}

