/*
 */
#include "crate.hpp"
#include "ast.hpp"
#include "../parse/parseerror.hpp"
#include "../expand/cfg.hpp"
#include <hir/hir.hpp>  // HIR::Crate
#include <hir/main_bindings.hpp>    // HIR_Deserialise
#include <macro_rules/macro_rules.hpp>  // Used to update the crate name

namespace {
    bool check_item_cfg(const ::AST::MetaItems& attrs)
    {
        for(const auto& at : attrs.m_items) {
            if( at.name() == "cfg" && !check_cfg(attrs.m_span, at) ) {
                return false;
            }
        }
        return true;
    }
    void iterate_module(::AST::Module& mod, ::std::function<void(::AST::Module& mod)> fcn)
    {
        fcn(mod);
        for( auto& sm : mod.items() )
        {
            TU_IFLET(::AST::Item, sm.data, Module, e,
                if( check_item_cfg(sm.data.attrs) )
                {
                    iterate_module(e, fcn);
                }
            )
        }
    }
}


namespace AST {

Crate::Crate():
    m_root_module(::AST::Path("",{})),
    m_load_std(LOAD_STD)
{
}

void Crate::load_externs()
{
    auto cb = [this](Module& mod) {
        for( const auto& it : mod.items() )
        {
            TU_IFLET(AST::Item, it.data, Crate, c,
                const auto& name = c.name;
                if( check_item_cfg(it.data.attrs) )
                {
                    load_extern_crate( name );
                }
            )
        }
        };
    iterate_module(m_root_module, cb);
}
void Crate::load_extern_crate(const ::std::string& name)
{
    m_extern_crates.insert(::std::make_pair( name, ExternCrate { name, "output/lib"+name+".hir" } ));
}

ExternCrate::ExternCrate(const ::std::string& name, const ::std::string& path)
{
    m_hir = HIR_Deserialise(path);
    
    // TODO: Do a pass across m_hir that
    // 1. Loads referenced crates
    // 2. Updates all absolute paths with the crate name
    // 3. Updates macros with the crate name
    for(auto& mac : m_hir->m_exported_macros)
    {
        if( mac.second->m_source_crate == "" )
        {
            mac.second->m_source_crate = name;
        }
    }
    // 4. Sets binding pointers where required
}

void ExternCrate::with_all_macros(::std::function<void(const ::std::string& , const MacroRules&)> cb) const
{
    for(const auto& m : m_hir->m_exported_macros)
    {
        cb(m.first, *m.second);
    }
}
const MacroRules* ExternCrate::find_macro_rules(const ::std::string& name) const
{
    auto i = m_hir->m_exported_macros.find(name);
    if(i != m_hir->m_exported_macros.end())
        return &*i->second;
    return nullptr;
}


}   // namespace AST

