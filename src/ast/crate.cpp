/*
 */
#include "crate.hpp"
#include "ast.hpp"
#include "../parse/parseerror.hpp"
#include "../expand/cfg.hpp"
#include <hir/hir.hpp>  // HIR::Crate
#include <hir/main_bindings.hpp>    // HIR_Deserialise

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
        // TODO: What about if an anon mod has been #[cfg]-d out?
        for(const auto& anon : mod.anon_mods() ) {
            iterate_module(*anon, fcn);
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
                    load_extern_crate( it.data.span, name );
                }
            )
        }
        };
    iterate_module(m_root_module, cb);
    
    // Check for no_std or no_core, and load libstd/libcore
    // - Duplicates some of the logic in "Expand", but also helps keep crate loading separate to most of expand
    // NOTE: Not all crates are loaded here, any crates loaded by macro invocations will be done during expand.
    bool no_std  = false;
    bool no_core = false;
    
    for( const auto& a : this->m_attrs.m_items )
    {
        if( a.name() == "no_std" )
            no_std = true;
        if( a.name() == "no_core" )
            no_core = true;
        if( a.name() == "cfg_attr" && a.items().size() == 2 ) {
            if( check_cfg(this->m_attrs.m_span, a.items().at(0)) )
            {
                const auto& a2 = a.items().at(1);
                if( a2.name() == "no_std" )
                    no_std = true;
                if( a2.name() == "no_core" )
                    no_core = true;
            }
        }
    }

    if( no_core ) {
        // Don't load anything
    }
    else if( no_std ) {
        this->load_extern_crate(Span(), "core");
    }
    else {
        this->load_extern_crate(Span(), "std");
    }
}
void Crate::load_extern_crate(Span sp, const ::std::string& name)
{
    DEBUG("Loading crate '" << name << "'");
    // TODO: Search a list of load paths for the crate

    ::std::vector< ::std::string> paths { "output/", "output/test_deps/" };
    ::std::string   path;
    for(const auto& p : paths){
        path = p + "lib" + name + ".hir";
        
        if( ::std::ifstream(path).good() ) {
            break ;
        }
    }
    if( !::std::ifstream(path).good() ) {
        ERROR(sp, E0000, "Unable to locate crate '" << name << "'");
    }
    
    auto res = m_extern_crates.insert(::std::make_pair( name, ExternCrate { name, path } ));
    auto crate_ext_list = mv$( res.first->second.m_hir->m_ext_crates );
    
    // Load referenced crates
    for( const auto& ext : crate_ext_list )
    {
        if( m_extern_crates.count(ext.first) == 0 )
        {
            this->load_extern_crate(sp, ext.first);
        }
    }
}

ExternCrate::ExternCrate(const ::std::string& name, const ::std::string& path):
    m_name(name)
{
    TRACE_FUNCTION_F("name=" << name << ", path='" << path << "'");
    m_hir = HIR_Deserialise(path, name);
    
    m_hir->post_load_update(name);
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

