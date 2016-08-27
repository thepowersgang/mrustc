/*
 */
#include "crate.hpp"
#include "ast.hpp"
#include "../parse/parseerror.hpp"
#include "../expand/cfg.hpp"

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
                    TODO(it.data.span, "Load crate '" << name << "' as '" << it.name << "'");
                }
            )
        }
        };
    iterate_module(m_root_module, cb);
}


Module& Crate::get_root_module(const ::std::string& name) {
    return const_cast<Module&>( const_cast<const Crate*>(this)->get_root_module(name) );
}
const Module& Crate::get_root_module(const ::std::string& name) const {
    if( name == "" )
        return m_root_module;
    auto it = m_extern_crates.find(name);
    if( it != m_extern_crates.end() )
        throw ::std::runtime_error("TODO: Get root module for extern crate");
//        return it->second.root_module();
    throw ParseError::Generic("crate name unknown");
}

void Crate::load_extern_crate(::std::string name)
{
    ::std::ifstream is("output/"+name+".ast");
    if( !is.is_open() )
    {
        throw ParseError::Generic("Can't open crate '" + name + "'");
    }
    //Deserialiser_TextTree   ds(is);
    //Deserialiser&   d = ds;
    
    ExternCrate ret;
    
    // TODO: ...
    
    m_extern_crates.insert( make_pair(::std::move(name), ::std::move(ret)) );
}

ExternCrate::ExternCrate()
{
}

ExternCrate::ExternCrate(const char *path)
{
    throw ParseError::Todo( FMT("Load extern crate from a file - '" << path << "'") );
}

const MacroRules* ExternCrate::find_macro_rules(const ::std::string& name)
{
    auto i = m_mr_macros.find(name);
    if(i != m_mr_macros.end())
        return &*i->second;
    return nullptr;
}


}   // namespace AST

