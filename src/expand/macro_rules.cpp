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
        TTStream    lex(sp, ParseState(), tt);
        auto mac = Parse_MacroRules(lex);
        DEBUG("macro_rules! " << mod.path() + ident << " " << &*mac);
        mod.add_macro( false, ident, mv$(mac) );

        return ::std::unique_ptr<TokenStream>( new TTStreamO(sp, ParseState(), TokenTree()) );
    }
};

class CMacroUseHandler:
    public ExpandDecorator
{
    AttrStage stage() const override { return AttrStage::Post; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item& i) const override
    {
        TRACE_FUNCTION_F("[CMacroUseHandler] path=" << path);

        std::vector<RcString>   filter;
        if( mi.data().size() > 0 )
        {
            mi.parse_paren_ident_list([&](const Span& sp, RcString ident) {
                filter.push_back(ident);
                });
        }
        std::vector<bool>   filters_used(filter.size());

        auto filter_valid = [&](RcString name)->bool {
            if( filter.empty() ) {
                return true;
            }
            auto it = std::find(filter.begin(), filter.end(), name);
            if(it != filter.end()) {
                auto i = it - filter.begin();
                filters_used[i] = true;
                return true;
            }
            else {
                return false;
            }
            };

        if(i.is_None()) {
            // Just ignore
        }
        else if(const auto* ec_item = i.opt_Crate())
        {
            const auto& ec = crate.m_extern_crates.at(ec_item->name.c_str());

            DEBUG(ec.m_hir->m_exported_macro_names.size() << " exported macros");
            for(const auto& name : ec.m_hir->m_exported_macro_names)
            {
                if( !filter_valid(name) )
                {
                    DEBUG("Skip " << name);
                    continue;
                }
                ASSERT_BUG(sp, ec.m_hir->m_root_module.m_macro_items.count(name) == 1, "Macro `" << name << "` missing from crate " << ec.m_name);
                const auto* e = &*ec.m_hir->m_root_module.m_macro_items.at(name);
                if( !e->publicity.is_global() )
                {
                    DEBUG("Not public: " << name);
                    continue ;
                }

                if( const auto* imp = e->ent.opt_Import() )
                {
                    if( imp->path.m_crate_name == CRATE_BUILTINS ) {
                        DEBUG("Importing builtin (skip): " << name);
                        continue ;
                    }
                    ASSERT_BUG(sp, crate.m_extern_crates.count(imp->path.m_crate_name), "Crate `" << imp->path.m_crate_name << "` not loaded");
                    const ::HIR::Module& mod = crate.m_extern_crates.at(imp->path.m_crate_name).m_hir->get_mod_by_path(sp, imp->path, /*ignore_last_node*/true, /*ignore_crate_name*/true);

                    ASSERT_BUG(sp, mod.m_macro_items.count(imp->path.m_components.back()), "Failed to find final component of " << imp->path);
                    e = &*mod.m_macro_items.at(imp->path.m_components.back());
                    if( const auto& imp2 = e->ent.opt_Import() ) {
                        if( imp2->path.m_crate_name == CRATE_BUILTINS ) {
                            DEBUG("Importing builtin (skip): " << name);
                            continue ;
                        }
                        else {
                            ASSERT_BUG(sp, !e->ent.is_Import(), "Recursive import - " << imp->path << " pointed to " << imp2->path);
                        }
                    }
                    else {
                    }
                }

                TU_MATCH_HDRA( (e->ent), { )
                TU_ARMA(Import, imp) {
                    assert(false);
                    }
                TU_ARMA(MacroRules, mac_ptr) {
                    DEBUG("Imported " << name << "!");

                    auto mi = AST::Module::MacroImport{ false, name, make_vec2(ec_item->name, name), &*mac_ptr };
                    mod.m_macro_imports.push_back(mv$(mi));

                    mod.add_macro_import( sp, name, &*mac_ptr );
                    }
                TU_ARMA(ProcMacro, p) {
                    DEBUG("Imported " << name << "! (proc macro)");
                    auto mi = AST::Module::MacroImport{ false, p.path.m_components.back(), p.path.m_components, nullptr };
                    mi.path.insert(mi.path.begin(), p.path.m_crate_name);
                    mod.m_macro_imports.push_back(mv$(mi));
                    mod.add_macro_import( sp, name, &p );
                    }
                }
            }
        }
        else if( const auto* submod_p = i.opt_Module() )
        {
            const auto& submod = *submod_p;
            for( const auto& mr : submod.macros() )
            {
                if( !filter_valid(mr.name) )
                {
                    continue;
                }
                DEBUG("Imported " << mr.name);
                mod.add_macro_import( sp, mr.name, &*mr.data );
            }
            for( const auto& mri : submod.macro_imports_res() )
            {
                if( !filter_valid(mri.name) )
                {
                    continue;
                }
                DEBUG("Imported " << mri.name << " (propagate)");
                mod.add_macro_import( sp, mri.name, mri.data.clone() );
            }
        }
        else {
            WARNING(sp, W0000, "Use of #[macro_use] on non-module/crate - " << i.tag_str());
            return ;
        }

        for(size_t i = 0; i < filter.size(); i ++)
        {
            if( !filters_used[i] ) {
                ERROR(sp, E0000, "Couldn't find macro " << filter[i]);
            }
        }
    }

};

class CMacroExportHandler:
    public ExpandDecorator
{
    AttrStage stage() const override { return AttrStage::Post; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item& i) const override
    {
        // TODO: Flags on the attribute
        // - `local_inner_macros`: Forces macro lookups within the expansion to search within the source crate
        //   > Strictly speaking, not the same as `macro`-style macros?
        bool local_inner_macros = false;
        if(mi.data().size() > 0)
        {
            mi.parse_paren_ident_list([&](const Span& sp, RcString ident) {
                if( ident == "local_inner_macros" ) {
                    local_inner_macros = true;
                }
                else {
                    ERROR(sp, E0000, "Unknown option for #[macro_export] - " << ident);
                }
                });
        }

        if( i.is_None() ) {
        }
        // If on a `use` it's for a #[rustc_builtin_macro]
        else if( const auto* u = i.opt_Use() )
        {
            if( u->entries.size() == 1
                && u->entries.back().path.is_absolute()
                && u->entries.back().path.m_class.as_Absolute().crate == CRATE_BUILTINS
                && u->entries.back().path.m_class.as_Absolute().nodes.size() == 1
                )
                ;
            else
                ERROR(sp, E0000, "Use of #[macro_export] on non-macro - " << i.tag_str());
            const auto& p = u->entries.back().path.m_class.as_Absolute();
            const auto& name = p.nodes.front().name();
            AST::Module::MacroImport    mi;
            mi.is_pub = true;
            mi.macro_ptr = nullptr;
            mi.name = u->entries.front().name;
            mi.path.push_back(p.crate);
            mi.path.push_back(name);
            crate.m_root_module.m_macro_imports.push_back(mv$(mi));

            crate.m_root_module.add_item(sp, true, name, i.clone(), {});
        }
        else if( i.is_MacroInv() ) {
            const auto& mac = i.as_MacroInv();
            if( !(mac.path().is_trivial() && mac.path().as_trivial() == "macro_rules") ) {
                ERROR(sp, E0000, "#[macro_export] is only valid on macro_rules!");
            }
            const auto& name = mac.input_ident();

            // Tag the macro in the module for crate export
            // AND move it to the root module
            auto it = ::std::find_if( mod.macros().begin(), mod.macros().end(), [&](const auto& x){ return x.name == name; } );
            ASSERT_BUG(sp, it != mod.macros().end(), "Macro '" << name << "' not defined in this module");
            auto e = mv$(*it);
            mod.macros().erase(it);

            if( local_inner_macros ) {
                Ident::ModPath  mp;
                mp.crate = "";
                // Empty node list, will search the crate root
                // TODO: Strictly speaking, this shouldn't apply to non-macro paths
                DEBUG("#[macro_export(local_inner_macros)] mp=" << mp);
                e.data->m_hygiene.set_mod_path(mv$(mp));
            }

            e.data->m_exported = true;
            DEBUG("- Export macro " << name << "!");
            crate.m_root_module.macros().push_back( mv$(e) );
        }
        else if( i.is_Macro() ) {
            const auto& name = path.nodes.back();
            if(i.as_Macro())
            {
                i.as_Macro()->m_exported = true;
                ASSERT_BUG(sp, path.nodes.size() == 1, "");
                DEBUG("- Export macro (item) " << name << "!");
                //crate.m_root_module.macros().push_back( mv$(*i.as_Macro()) );
            }
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
    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item& i) const override
    {
        if( !i.is_Crate() ) {
            ERROR(sp, E0000, "Use of #[macro_reexport] on non-crate - " << i.tag_str());
        }

        const auto& crate_name = i.as_Crate().name;
        auto& ext_crate = *crate.m_extern_crates.at(crate_name.c_str()).m_hir;

        mi.parse_paren_ident_list([&](const Span& sp, RcString name) {
            auto it = ::std::find(ext_crate.m_exported_macro_names.begin(), ext_crate.m_exported_macro_names.end(), name);
            if( it == ext_crate.m_exported_macro_names.end() )
                ERROR(sp, E0000, "Could not find macro " << name << "! in crate " << crate_name);
            // TODO: Do this differently.
            ext_crate.m_root_module.m_macro_items.at(name)->ent.as_MacroRules()->m_exported = true;
            //ext_crate.m_root_module.m_macro_items.at(name)->publicity = AST::Publicity::new_global();
            });
    }
};

class CBuiltinMacroHandler:
    public ExpandDecorator
{
    AttrStage stage() const override { return AttrStage::Pre; }
    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item& i) const override
    {
        RcString    name;
        if(i.is_MacroInv()) {
            const auto& e = i.as_MacroInv();
            if( !(e.path().is_trivial() && e.path().as_trivial() == "macro_rules") ) {
                ERROR(sp, E0000, "Use of #[rustc_builtin_macro] on macro other than macro_rules! - " << i.tag_str());
            }
            name = e.input_ident();
        }
        else if(i.is_Macro()) {
            name = path.nodes.back();
        }
        else {
            ERROR(sp, E0000, "Use of #[rustc_builtin_macro] on non-macro - " << i.tag_str());
        }

        AST::UseItem    ui;
        ui.entries.push_back(AST::UseItem::Ent { });
        ui.entries.back().name = name;
        ui.entries.back().path = AST::Path(CRATE_BUILTINS, { name });
        DEBUG("Convert macro_rules tagged #[rustc_builtin_macro] with use - " << name);
        i = AST::Item::make_Use(mv$(ui));
    }
};

STATIC_MACRO("macro_rules", CMacroRulesExpander);
STATIC_DECORATOR("macro_use", CMacroUseHandler);
STATIC_DECORATOR("macro_export", CMacroExportHandler);
STATIC_DECORATOR("macro_reexport", CMacroReexportHandler);
STATIC_DECORATOR("rustc_builtin_macro", CBuiltinMacroHandler);

