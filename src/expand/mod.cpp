/*
 */
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>
#include <synext.hpp>
#include <map>
#include "macro_rules.hpp"
#include "../parse/common.hpp"  // For reparse from macros

::std::map< ::std::string, ::std::unique_ptr<ExpandDecorator> >  g_decorators;
::std::map< ::std::string, ::std::unique_ptr<ExpandProcMacro> >  g_macros;

void Register_Synext_Decorator(::std::string name, ::std::unique_ptr<ExpandDecorator> handler) {
    g_decorators[name] = mv$(handler);
}
void Register_Synext_Macro(::std::string name, ::std::unique_ptr<ExpandProcMacro> handler) {
    g_macros[name] = mv$(handler);
}

void Expand_Attrs(const ::AST::MetaItems& attrs, AttrStage stage,  ::AST::Crate& crate, const ::AST::Path& path, ::AST::Module& mod, ::AST::Item& item)
{
    for( auto& a : attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( d.first == a.name() ) {
                DEBUG("#[" << d.first << "] " << (int)d.second->stage() << "-" << (int)stage);
                if( d.second->stage() == stage ) {
                    d.second->handle(a, crate, path, mod, item);
                }
            }
        }
    }
}

::std::unique_ptr<TokenStream> Expand_Macro(bool is_early, LList<const AST::Module*> modstack, ::AST::Module& mod, ::AST::MacroInvocation& mi)
{
    for( const auto& m : g_macros )
    {
        if( mi.name() == m.first && m.second->expand_early() == is_early )
        {
            auto e = m.second->expand(mi.input_ident(), mi.input_tt(), mod);
            mi.clear();
            return e;
        }
    }
    
    
    // Iterate up the module tree, using the first located macro
    for(const auto* ll = &modstack; ll; ll = ll->m_prev)
    {
        const auto& mac_mod = *ll->m_item;
        for( const auto& mr : mac_mod.macros() )
        {
            //DEBUG("- " << mr.name);
            if( mr.name == mi.name() )
            {
                if( mi.input_ident() != "" )
                    ;   // TODO: ERROR - macro_rules! macros can't take an ident
                
                auto e = Macro_Invoke(mr.name.c_str(), mr.data, mi.input_tt(), mod);
                mi.clear();
                return e;
            }
        }
        for( const auto& mri : mac_mod.macro_imports_res() )
        {
            //DEBUG("- " << mri.name);
            if( mri.name == mi.name() )
            {
                if( mi.input_ident() != "" )
                    ;   // TODO: ERROR - macro_rules! macros can't take an ident
                
                auto e = Macro_Invoke(mi.name().c_str(), *mri.data, mi.input_tt(), mod);
                mi.clear();
                return e;
            }
        }
    }
    
    if( ! is_early ) {
        // Error - Unknown macro name
        ERROR(mi.span(), E0000, "Unknown macro '" << mi.name() << "'");
    }
    
    // Leave valid and return an empty expression
    return ::std::unique_ptr<TokenStream>();
}

void Expand_Mod(bool is_early, ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Path modpath, ::AST::Module& mod)
{
    TRACE_FUNCTION_F("is_early = " << is_early << ", modpath = " << modpath);
    
    for( const auto& mi: mod.macro_imports_res() )
        DEBUG("- Imports '" << mi.name << "'");
    
    // TODO: Have the AST representation of a module include the definition order,
    //  mixing macro invocations, general items, use statements, and `impl`s
    
    // 1. Macros first
    //for( auto& mi : mod.macro_invs() )
    for(unsigned int i = 0; i < mod.macro_invs().size(); i ++ )
    {
        auto& mi = mod.macro_invs()[i];
        DEBUG("> Macro invoke '"<<mi.name()<<"'");
        if( mi.name() != "" )
        {
            // Move out of the module to avoid invalidation if a new macro invocation is added
            auto mi_owned = mv$(mi);
            
            auto ttl = Expand_Macro(is_early, modstack, mod, mi_owned);
            
            if( mi_owned.name() != "" )
            {
                mod.macro_invs()[i] = mv$(mi_owned);
            }
            else
            {
                // Re-parse tt
                assert(ttl.get());
                Parse_ModRoot_Items(*ttl, mod, false, "-");
            }
        }
    }
    
    // 2. General items
    DEBUG("Items");
    for( auto& i : mod.items() )
    {
        DEBUG("- " << i.name << " :: " << i.data.attrs);
        ::AST::Path path = modpath + i.name;
        
        Expand_Attrs(i.data.attrs, (is_early ? AttrStage::EarlyPre : AttrStage::LatePre),  crate, path, mod, i.data);
        
        TU_MATCH(::AST::Item, (i.data), (e),
        (None,
            // Skip, nothing
            ),
        (Module,
            LList<const AST::Module*>   sub_modstack(&modstack, &e.e);
            Expand_Mod(is_early, crate, sub_modstack, path, e.e);
            ),
        (Crate,
            // Skip, no recursion
            ),
        
        (Struct,
            // TODO: Struct items
            ),
        (Enum,
            // TODO: Enum variants
            ),
        (Trait,
            // TODO: Trait definition
            ),
        (Type,
            // TODO: Do type aliases require recursion?
            ),
        
        (Function,
            // TODO: Recurse into function code (and types)
            ),
        (Static,
            // TODO: Recurse into static values
            )
        )
        
        Expand_Attrs(i.data.attrs, (is_early ? AttrStage::EarlyPost : AttrStage::LatePost),  crate, path, mod, i.data);
    }
    
    DEBUG("Impls");
    for( auto& i : mod.impls() )
    {
        DEBUG("- " << i);
    }
    
    for( const auto& mi: mod.macro_imports_res() )
        DEBUG("- Imports '" << mi.name << "'");

    // 3. Post-recurse macros (everything else)
}
void Expand(::AST::Crate& crate)
{
    // 1. Crate attributes
    for( auto& a : crate.m_attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( d.first == a.name() && d.second->stage() == AttrStage::EarlyPre ) {
                d.second->handle(a, crate);
            }
        }
    }
    
    // 2. Module attributes
    for( auto& a : crate.m_attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( d.first == a.name() && d.second->stage() == AttrStage::EarlyPre ) {
                //d.second->handle(a, crate, ::AST::Path(), crate.m_root_module, crate.m_root_module);
            }
        }
    }
    
    // 3. Module tree
    Expand_Mod(true , crate, LList<const ::AST::Module*>(nullptr, &crate.m_root_module), ::AST::Path(), crate.m_root_module);
    Expand_Mod(false, crate, LList<const ::AST::Module*>(nullptr, &crate.m_root_module), ::AST::Path(), crate.m_root_module);
    
    // Post-process
    #if 0
    for( auto& a : crate.m_attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( d.first == a.name() && d.second->expand_before_macros() == false ) {
                //d.second->handle(a, crate, ::AST::Path(), crate.m_root_module, crate.m_root_module);
            }
        }
    }
    for( auto& a : crate.m_attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( d.first == a.name() && d.second->expand_before_macros() == false ) {
                d.second->handle(a, crate);
            }
        }
    }
    #endif
}


