
#include <synext.hpp>
#include "../ast/expr.hpp"
#include "../ast/ast.hpp"
#include "../parse/common.hpp"
#include "macro_rules.hpp"

class CMacroRulesExpander:
    public ExpandProcMacro
{
    bool    expand_early() const override { return true; }
    
    AST::Expr expand(const ::std::string& ident, const TokenTree& tt, AST::Module& mod, MacroPosition position)
    {
        if( ident == "" ) {
            throw ::std::runtime_error( "ERROR: macro_rules! requires an identifier" );
        }
        
        TTStream    lex(tt);
        auto mac = Parse_MacroRules(lex);
        // TODO: Place into current module using `ident` as the name
        mod.add_macro( false, ident, mac );
        
        return AST::Expr();
    }
};

class CMacroUseHandler:
    public ExpandDecorator
{
    AttrStage stage() const override { return AttrStage::EarlyPost; }
    
    void handle(const AST::MetaItem& mi, AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item& i) const override
    {
        TRACE_FUNCTION_F("path=" << path);
        if( !i.is_Module() )
            throw ::std::runtime_error("ERROR: Use of #[macro_use] on non-module");
        
        const auto& submod = i.as_Module().e;
        
        if( mi.has_sub_items() )
        {
            throw ::std::runtime_error("TODO: #[macro_use]");
        }
        else
        {
            for( const auto& mr : submod.macros() )
            {
                DEBUG("Imported " << mr.name);
                mod.add_macro_import( mr.name, mr.data );
            }
            for( const auto& mri : submod.macro_imports_res() )
            {
                DEBUG("Imported " << mri.name << " (propagate)");
                mod.add_macro_import( mri.name, *mri.data );
            }
        }
    }
    
};

AST::Expr Macro_Invoke(const char* name, const MacroRules& rules, const TokenTree& tt, AST::Module& mod, MacroPosition position)
{
    auto out_tt = Macro_InvokeRules(name, rules, tt);
    TokenStream& lex = *out_tt;
    // TODO: Expand out_tt
    switch(position)
    {
    case MacroPosition::Item: {
        LList<AST::Module*> l(nullptr, &mod);
        Parse_ModRoot_Items(lex, mod, l, false, "-");
        return AST::Expr();
        }
    default:
        throw ::std::runtime_error("TODO: Macro in non-item position");
    //case MacroPosition::Stmt: {
    //    break; }
    }
}


STATIC_MACRO("macro_rules", CMacroRulesExpander);
STATIC_DECORATOR("macro_use", CMacroUseHandler);

