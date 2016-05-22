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


void Crate::iterate_functions(fcn_visitor_t* visitor)
{
    m_root_module.iterate_functions(visitor, *this);
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

bool Crate::is_trait_implicit(const Path& trait) const
{
    // 1. Handle lang_item traits (e.g. FhantomFn)
    if( m_lang_item_PhantomFn.is_valid() && trait.equal_no_generic( m_lang_item_PhantomFn ) >= 0 )
    {
        return true;
    }
    return false;
}

/**
 * \brief Checks if a type implements the provided wildcard trait
 * \param trait Trait path
 * \param type  Type in question
 * \note Wildcard trait = A trait for which there exists a 'impl Trait for ..' definition
 *
 * \return True if the trait is implemented (either exlicitly, or implicitly)
 */
bool Crate::check_impls_wildcard(const Path& trait, const TypeRef& type) const
{
    ::std::vector<TypeRef>  _params;
    TRACE_FUNCTION_F("trait="<<trait<<", type="<<type);
    
    // 1. Look for a negative impl for this type
    for( auto implptr : m_neg_impl_index )
    {
        const ImplDef& neg_impl = *implptr;
        
        if( neg_impl.matches(_params, trait, type) )
        {
            return false;
        }
    }
    DEBUG("No negative impl of " << trait << " for " << type);
    
    // 2. Look for a positive impl for this type (i.e. an unsafe impl)
    for( auto implptr : m_impl_index )
    {
        const Impl& impl = *implptr;
        if( impl.def().matches(_params, trait, type) )
        {
            return true;
        }
    }
    DEBUG("No positive impl of " << trait << " for " << type);
    
    // 3. If none found, destructure the type
    return type.impls_wildcard(*this, trait);
}


bool Crate::find_inherent_impls(const TypeRef& type, ::std::function<bool(const Impl& , ::std::vector<TypeRef> )> callback) const
{
    assert( !type.is_type_param() );
    
    for( auto implptr : m_impl_index )
    {
        Impl& impl = *implptr;
        if( impl.def().trait().is_valid() )
        {
            // Trait
        }
        else
        {
            DEBUG("- " << impl.def());
            ::std::vector<TypeRef>  out_params;
            if( impl.def().matches(out_params, AST::Path(), type) )
            {
                if( callback(impl, out_params) ) {
                    return true;
                }
            }
        }
    }
    
    return false;
}

::rust::option<ImplRef> Crate::find_impl(const Path& trait, const TypeRef& type) const
{
    ::std::vector<TypeRef>  params;
    Impl    *out_impl;
    if( find_impl(trait, type, &out_impl, &params) )
    {
        return ::rust::Some( ImplRef(*out_impl, params) );
    }
    else {
        return ::rust::None<ImplRef>();
    }
}

bool Crate::find_impl(const Path& trait, const TypeRef& type, Impl** out_impl, ::std::vector<TypeRef>* out_params) const 
{
    TRACE_FUNCTION_F("trait = " << trait << ", type = " << type);
    
    // If no params output provided, use a dud locaton
    ::std::vector<TypeRef>  dud_params;
    if(out_params)
        *out_params = ::std::vector<TypeRef>();
    else
        out_params = &dud_params;
    
    // Zero output
    if(out_impl)
        *out_impl = nullptr;
    
    if( is_trait_implicit(trait) )
    {
        if(out_impl)    throw CompileError::BugCheck("find_impl - Asking for concrete impl of a marker trait");
        return true;
    }
    
    // 0. Handle generic bounds
    // TODO: Handle more complex bounds like "[T]: Trait"
    if( type.is_type_param() )
    {
        if( trait.is_valid() )
        {
            assert(type.type_params_ptr());
            // Search bounds for type: trait
            for( const auto& bound : type.type_params_ptr()->bounds() )
            {
                DEBUG("bound = " << bound);
                TU_MATCH_DEF(GenericBound, (bound), (ent),
                (),
                (IsTrait,
                    if(ent.type == type && ent.trait == trait) {
                        // If found, success!
                        DEBUG("- Success!");
                        // TODO: What should be returned, kinda need to return a boolean
                        if(out_impl)    throw CompileError::BugCheck("find_impl - Asking for a concrete impl, but generic passed");
                        return true;
                    }
                    )
                )
            }
            // Else, failure
            DEBUG("- No impl :(");
            //if(out_impl)    throw CompileError::BugCheck("find_impl - Asking for a concrete impl, but generic passed");
            return false;
        }
        else
        {
            DEBUG("- No inherent impl for generic params");
            return false;
        }
    }
    
    // TODO: Do a sort to allow a binary search
    // 1. Search for wildcard traits (i.e. ones like "impl Send for ..")
    // - These require special handling, as negatives apply
    for( auto implptr : m_impl_index )
    {
        Impl& impl = *implptr;
        ::std::vector<TypeRef>  _p;
        if( impl.def().matches(_p, trait, TypeRef()) )
        {
            assert(_p.size() == 0);
            // This is a wildcard trait, need to locate either a negative, or check contents
            if( check_impls_wildcard(trait, type) )
            {
                if(out_impl)    *out_impl = &impl;
                return true;
            }
            else {
                return false;
            }
        }
        
    }
    
    // 2. Check real impls
    DEBUG("Not wildcard");
    for( auto implptr : m_impl_index )
    {
        Impl& impl = *implptr;
        // TODO: What if there's two impls that match this combination?
        if( impl.def().matches(*out_params, trait, type) )
        {
            if(out_impl)    *out_impl = &impl;
            return true;
        }
    }
    DEBUG("No impl of " << trait << " for " << type);
    return false;
}

Function& Crate::lookup_method(const TypeRef& type, const char *name)
{
    throw ParseError::Generic( FMT("TODO: Lookup method "<<name<<" for type " <<type));
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

