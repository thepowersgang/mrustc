
#include <synext.hpp>
#include "../ast/expr.hpp"
#include "../ast/ast.hpp"
#include "../parse/common.hpp"
#include <ast/crate.hpp>
#include "macro_rules.hpp"
#include <macro_rules/macro_rules.hpp>

class CMacroRulesExpander:
    public ExpandProcMacro
{
    bool    expand_early() const override { return true; }
    
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        if( ident == "" )
            ERROR(sp, E0000, "macro_rules! requires an identifier" );
        
        DEBUG("Parsing macro_rules! " << ident);
        TTStream    lex(tt);
        auto mac = Parse_MacroRules(lex);
        mod.add_macro( false, ident, mv$(mac) );
        
        return ::std::unique_ptr<TokenStream>( new TTStreamO(TokenTree()) );
    }
};

class CMacroUseHandler:
    public ExpandDecorator
{
    AttrStage stage() const override { return AttrStage::EarlyPost; }
    
    void handle(const Span& sp, const AST::MetaItem& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item& i) const override
    {
        TRACE_FUNCTION_F("path=" << path);
        
        TU_IFLET( ::AST::Item, i, None, e,
            // Just ignore
        )
        else TU_IFLET( ::AST::Item, i, Crate, ec_name,
            const auto& ec = crate.m_extern_crates.at(ec_name.name);
            if( mi.has_sub_items() )
            {
                TODO(sp, "Named import from extern crate");
            }
            else
            {
                ec.with_all_macros([&](const auto& name, const auto& mac) {
                    mod.add_macro_import( name, mac );
                    });
            }
        )
        else TU_IFLET( ::AST::Item, i, Module, submod,
            if( mi.has_sub_items() )
            {
                for( const auto& si : mi.items() )
                {
                    const auto& name = si.name();
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
        )
        else {
            ERROR(sp, E0000, "Use of #[macro_use] on non-module/crate - " << i.tag_str());
        }
    }
    
};

::std::unique_ptr<TokenStream>  Macro_Invoke(const char* name, const MacroRules& rules, const TokenTree& tt, AST::Module& mod)
{
    return Macro_InvokeRules(name, rules, tt);
}


STATIC_MACRO("macro_rules", CMacroRulesExpander);
STATIC_DECORATOR("macro_use", CMacroUseHandler);

