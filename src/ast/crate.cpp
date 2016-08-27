/*
 */
#include "crate.hpp"
#include "ast.hpp"
#include "../parse/parseerror.hpp"
#include "../expand/cfg.hpp"
#include <hir/hir.hpp>  // HIR Crate

#include <serialiser_texttree.hpp>

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
    m_extern_crates.insert(::std::make_pair( name, ExternCrate { "output/lib"+name+".hir" } ));
}

ExternCrate::ExternCrate(const ::std::string& path)
{
    throw ParseError::Todo( FMT("Load extern crate from a file - '" << path << "'") );
}

const MacroRules* ExternCrate::find_macro_rules(const ::std::string& name)
{
    auto i = m_hir->m_exported_macros.find(name);
    if(i != m_hir->m_exported_macros.end())
        return &i->second;
    return nullptr;
}


}   // namespace AST

