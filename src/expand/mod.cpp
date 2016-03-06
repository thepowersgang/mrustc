/*
 */
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>
#include <synext.hpp>
#include <map>

::std::map< ::std::string, ::std::unique_ptr<ExpandDecorator> >  g_decorators;
::std::map< ::std::string, ::std::unique_ptr<ExpandProcMacro> >  g_macros;

void init() __attribute__((constructor(101)));
void init()
{
}

void Register_Synext_Decorator(::std::string name, ::std::unique_ptr<ExpandDecorator> handler) {
    g_decorators[name] = mv$(handler);
}

void Expand_Decorators_Mod(::AST::Crate& crate, bool is_before_macros, ::AST::Path modpath, ::AST::Module& mod)
{
    for( auto& i : mod.items() )
    {
        ::AST::Path path = modpath + i.name;
        for( auto& a : i.data.attrs.m_items )
        {
            for( auto& d : g_decorators ) {
                if( d.first == a.name() && d.second->expand_before_macros() == is_before_macros ) {
                    d.second->handle(a, crate, path, mod, i.data);
                }
            }
        }
        
        TU_MATCH(::AST::Item, (i.data), (e),
        (None,
            // Skip, nothing
            ),
        (Module,
            Expand_Decorators_Mod(crate, is_before_macros, path, e.e);
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
    }
}
void Expand_Decorators(::AST::Crate& crate, bool is_before_macros)
{
    // 1. Crate attributes
    for( auto& a : crate.m_attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( d.first == a.name() && d.second->expand_before_macros() == is_before_macros ) {
                d.second->handle(a, crate);
            }
        }
    }
    
    // 2. Module attributes
    for( auto& a : crate.m_attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( d.first == a.name() && d.second->expand_before_macros() == is_before_macros ) {
                //d.second->handle(a, crate, ::AST::Path(), crate.m_root_module, crate.m_root_module);
            }
        }
    }
    
    // 3. Module tree
    Expand_Decorators_Mod(crate, is_before_macros, ::AST::Path(), crate.m_root_module);
}

/// Expand decorators that apply before macros are expanded
/// - E.g. #[cfg] #![no_std] ...
void Expand_Decorators_Pre(::AST::Crate& crate)
{
    Expand_Decorators(crate, true);
}

/// Expand macros
void Expand_Macros(::AST::Crate& crate)
{
}

/// Expand decorators that apply _after_ macros
/// - E.g. #[derive]
void Expand_Decorators_Post(::AST::Crate& crate)
{
    Expand_Decorators(crate, false);
}

/// Expand syntax sugar (e.g. for loops)
void Expand_Sugar(::AST::Crate& crate)
{
}

