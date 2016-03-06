/*
 */
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>
#include <synext.hpp>
#include <map>

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
            if( d.first == a.name() && d.second->stage() == stage ) {
                d.second->handle(a, crate, path, mod, item);
            }
        }
    }
}

void Expand_Mod(bool is_early, ::AST::Crate& crate, ::AST::Path modpath, ::AST::Module& mod)
{
    TRACE_FUNCTION_F("is_early = " << is_early << ", modpath = " << modpath);
    
    // 1. Macros first
    for( auto& mi : mod.macro_invs() )
    {
        if( mi.name() == "" )
            continue ;
        for( auto& m : g_macros ) {
            if( mi.name() == m.first && m.second->expand_early() == is_early ) {
                m.second->expand(mi.input_ident(), mi.input_tt(), mod, MacroPosition::Item);
                
                mi.clear();
                break;
            }
        }
        
        if( ! is_early && mi.name() != "" ) {
            // TODO: Error - Unknown macro name
            throw ::std::runtime_error( FMT("Unknown macro '" << mi.name() << "'") );
        }
    }
    
    // 2. General items
    DEBUG("Items");
    for( auto& i : mod.items() )
    {
        ::AST::Path path = modpath + i.name;
        
        Expand_Attrs(i.data.attrs, (is_early ? AttrStage::EarlyPre : AttrStage::LatePre),  crate, path, mod, i.data);
        
        TU_MATCH(::AST::Item, (i.data), (e),
        (None,
            // Skip, nothing
            ),
        (Module,
            Expand_Mod(is_early, crate, path, e.e);
            ),
        (Crate,
            // Skip, no recursion
            ),
        
        (Struct,
            // TODO: Struct items
            ),
        (Enum,
            ),
        (Trait,
            ),
        (Type,
            // TODO: Do type aliases require recursion?
            ),
        
        (Function,
            // TODO:
            ),
        (Static,
            // TODO: 
            )
        )
        
        Expand_Attrs(i.data.attrs, (is_early ? AttrStage::EarlyPost : AttrStage::LatePost),  crate, path, mod, i.data);
    }

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
    Expand_Mod(true , crate, ::AST::Path(), crate.m_root_module);
    Expand_Mod(false, crate, ::AST::Path(), crate.m_root_module);
    
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


