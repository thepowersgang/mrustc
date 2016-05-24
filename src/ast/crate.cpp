/*
 */
#include "crate.hpp"
#include "ast.hpp"
#include "../parse/parseerror.hpp"

#include <serialiser_texttree.hpp>

namespace {
    void iterate_module(::AST::Module& mod, ::std::function<void(::AST::Module& mod)> fcn)
    {
        fcn(mod);
        for( auto& sm : mod.items() )
        {
            TU_MATCH_DEF(::AST::Item, (sm.data), (e),
            ( ),
            (Module,
                iterate_module(e, fcn);
                )
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
            if( it.data.is_Crate() ) {
                const auto& name = it.data.as_Crate().name;
                throw ::std::runtime_error( FMT("TODO: Load crate '" << name << "' as '" << it.name << "'") );
            }
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
    Deserialiser_TextTree   ds(is);
    Deserialiser&   d = ds;
    
    ExternCrate ret;
    ret.deserialise( d );
    
    m_extern_crates.insert( make_pair(::std::move(name), ::std::move(ret)) );
}
SERIALISE_TYPE(Crate::, "AST_Crate", {
    unsigned ls = m_load_std;
    s.item(ls);
    s.item(m_extern_crates);
    s.item(m_root_module);
},{
    unsigned ls = m_load_std;
    s.item(ls);
    m_load_std = (::AST::Crate::LoadStd)ls;
    s.item(m_extern_crates);
    s.item(m_root_module);
})

ExternCrate::ExternCrate()
{
}

ExternCrate::ExternCrate(const char *path)
{
    throw ParseError::Todo( FMT("Load extern crate from a file - '" << path << "'") );
}

// Fill runtime-generated structures in the crate
#if 0
void ExternCrate::prescan()
{
    TRACE_FUNCTION;
    
    Crate& cr = m_crate;

    cr.m_root_module.prescan();
    
    for( const auto& mi : cr.m_root_module.macro_imports_res() )
    {
        DEBUG("Macro (I) '"<<mi.name<<"' is_pub="<<mi.is_pub);
        if( mi.is_pub )
        {
            m_crate.m_exported_macros.insert( ::std::make_pair(mi.name, mi.data) );
        }
    }
    for( const auto& mi : cr.m_root_module.macros() )
    {
        DEBUG("Macro '"<<mi.name<<"' is_pub="<<mi.is_pub);
        if( mi.is_pub )
        {
            m_crate.m_exported_macros.insert( ::std::make_pair(mi.name, &mi.data) );
        }
    }
}
#endif

const MacroRules* ExternCrate::find_macro_rules(const ::std::string& name)
{
    auto i = m_mr_macros.find(name);
    if(i != m_mr_macros.end())
        return &*i->second;
    return nullptr;
}

SERIALISE_TYPE(ExternCrate::, "AST_ExternCrate", {
    (void)s;
},{
    (void)s;
})


}   // namespace AST

