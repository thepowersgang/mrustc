/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/macro_rules.cpp
 * - Top-level handling of macro_rules! macros
 *  > macro_rules! dispatch handler
 *  > #[macro_use]
 *  > #[macro_export]
 *  > #[macro_reexport]
 */
#include <synext.hpp>
#include "../ast/expr.hpp"
#include "../ast/ast.hpp"
#include "../parse/common.hpp"
#include "../parse/ttstream.hpp"
#include <ast/crate.hpp>
#include <macro_rules/macro_rules.hpp>
#include <hir/hir.hpp>  // for HIR::Crate

class CMacroRulesExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        ERROR(sp, E0000, "macro_rules! requires an identifier" );
    }
    ::std::unique_ptr<TokenStream> expand_ident(const Span& sp, const ::AST::Crate& crate, const RcString& ident, const TokenTree& tt, AST::Module& mod) override
    {
        DEBUG("Parsing macro_rules! " << ident);
        TTStream    lex(sp, ParseState(crate.m_edition), tt);
        auto mac = Parse_MacroRules(lex);
        mod.add_macro( false, ident, mv$(mac) );

        return ::std::unique_ptr<TokenStream>( new TTStreamO(sp, ParseState(crate.m_edition), TokenTree()) );
    }
};

class CMacroUseHandler:
    public ExpandDecorator
{
    AttrStage stage() const override { return AttrStage::Post; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item& i) const override
    {
        TRACE_FUNCTION_F("path=" << path);

        TU_IFLET( ::AST::Item, i, None, e,
            // Just ignore
        )
        else if(const auto* ec_item = i.opt_Crate())
        {
            const auto& ec = crate.m_extern_crates.at(ec_item->name.c_str());
            if( mi.has_sub_items() )
            {
                TODO(sp, "Named import from extern crate");
            }
            else
            {
                ec.with_all_macros([&](const auto& name, const auto& mac) {
                    DEBUG("Imported " << name << "!");
                    mod.add_macro_import( name, mac );
                    });
                for(const auto& p : ec.m_hir->m_proc_macros)
                {
                    mod.m_macro_imports.push_back(AST::Module::MacroImport{ false, p.path.m_components.back(), p.path.m_components, nullptr });
                    mod.m_macro_imports.back().path.insert( mod.m_macro_imports.back().path.begin(), p.path.m_crate_name );
                }
                for(const auto& p : ec.m_hir->m_proc_macro_reexports)
                {
                    mod.m_macro_imports.push_back(AST::Module::MacroImport{ /*is_pub=*/ false, p.first, p.second.path.m_components, nullptr });
                    mod.m_macro_imports.back().path.insert( mod.m_macro_imports.back().path.begin(), p.second.path.m_crate_name );
                }
            }
        }
        else if( const auto* submod_p = i.opt_Module() )
        {
            const auto& submod = *submod_p;
            if( mi.has_sub_items() )
            {
                for( const auto& si : mi.items() )
                {
                    const auto& name = si.name().as_trivial();
                    for( const auto& mr : submod.macros() )
                    {
                        if( mr.name == name ) {
                            DEBUG("Imported " << mr.name);
                            mod.add_macro_import( mr.name, *mr.data );
                            goto _good;
                        }
                    }
                    for( const auto& mri : submod.macro_imports_res() )
                    {
                        if( mri.name == name ) {
                            DEBUG("Imported " << mri.name << " (propagate)");
                            mod.add_macro_import( mri.name, *mri.data );
                            goto _good;
                        }
                    }
                    ERROR(sp, E0000, "Couldn't find macro " << name);
                _good:
                    (void)0;
                }
            }
            else
            {
                for( const auto& mr : submod.macros() )
                {
                    DEBUG("Imported " << mr.name);
                    mod.add_macro_import( mr.name, *mr.data );
                }
                for( const auto& mri : submod.macro_imports_res() )
                {
                    DEBUG("Imported " << mri.name << " (propagate)");
                    mod.add_macro_import( mri.name, *mri.data );
                }
            }
        }
        else {
            ERROR(sp, E0000, "Use of #[macro_use] on non-module/crate - " << i.tag_str());
        }
    }

};

class CMacroExportHandler:
    public ExpandDecorator
{
    AttrStage stage() const override { return AttrStage::Post; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item& i) const override
    {
        if( i.is_None() ) {
        }
        else if( i.is_MacroInv() ) {
            const auto& mac = i.as_MacroInv();
            if( !(mac.path().is_trivial() && mac.path().as_trivial() == "macro_rules") ) {
                ERROR(sp, E0000, "#[macro_export] is only valid on macro_rules!");
            }
            const auto& name = mac.input_ident();

            // Tag the macro in the module for crate export
            auto it = ::std::find_if( mod.macros().begin(), mod.macros().end(), [&](const auto& x){ return x.name == name; } );
            ASSERT_BUG(sp, it != mod.macros().end(), "Macro '" << name << "' not defined in this module");
            it->data->m_exported = true;
            DEBUG("- Export macro " << name << "!");
        }
        else {
            ERROR(sp, E0000, "Use of #[macro_export] on non-macro - " << i.tag_str());
        }
    }
};

class CMacroReexportHandler:
    public ExpandDecorator
{
    AttrStage stage() const override { return AttrStage::Post; }
    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item& i) const override
    {
        if( !i.is_Crate() ) {
            ERROR(sp, E0000, "Use of #[macro_reexport] on non-crate - " << i.tag_str());
        }

        const auto& crate_name = i.as_Crate().name;
        auto& ext_crate = *crate.m_extern_crates.at(crate_name.c_str()).m_hir;

        if( mi.has_sub_items() )
        {
            for( const auto& si : mi.items() )
            {
                if( !si.name().is_trivial() )
                    ERROR(sp, E0000, "macro_reexport of non-trivial name - " << si.name());
                const auto& name = si.name().as_trivial();
                auto it = ext_crate.m_exported_macros.find(name);
                if( it == ext_crate.m_exported_macros.end() )
                    ERROR(sp, E0000, "Could not find macro " << name << "! in crate " << crate_name);
                it->second->m_exported = true;
            }
        }
        else
        {
            ERROR(sp, E0000, "#[macro_reexport] requires a list of macros");
        }
    }
};


STATIC_MACRO("macro_rules", CMacroRulesExpander);
STATIC_DECORATOR("macro_use", CMacroUseHandler);
STATIC_DECORATOR("macro_export", CMacroExportHandler);
STATIC_DECORATOR("macro_reexport", CMacroReexportHandler);

