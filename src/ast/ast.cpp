/*
 */
#include "ast.hpp"
#include "../types.hpp"
#include "../common.hpp"
#include <iostream>
#include "../parse/parseerror.hpp"
#include <algorithm>
#include <serialiser_texttree.hpp>

namespace AST {

void MetaItems::push_back(MetaItem i)
{
    m_items.push_back( ::std::move(i) );
}
MetaItem* MetaItems::get(const char *name)
{
    for( auto& i : m_items ) {
        if(i.name() == name) {
            i.mark_used();
            return &i;
        }
    }
    return 0;
}
SERIALISE_TYPE_A(MetaItems::, "AST_MetaItems", {
    s.item(m_items);
})

SERIALISE_TYPE(MetaItem::, "AST_MetaItem", {
    s << m_name;
    s << m_str_val;
    s << m_sub_items;
},{
    s.item(m_name);
    s.item(m_str_val);
    s.item(m_sub_items);
})


Impl Impl::make_concrete(const ::std::vector<TypeRef>& types) const
{
    DEBUG("types={" << types << "}");
    throw ParseError::Todo("Impl::make_concrete");
/*
    INDENT();
    
    assert(m_params.n_params());
    
    Impl    ret( TypeParams(), m_trait, m_type );
    
    auto resolver = [&](const char *name) {
        int idx = m_params.find_name(name);
        assert(idx >= 0);
        return types[idx];
        };
    
    ret.m_trait.resolve_args(resolver);
    ret.m_type.resolve_args(resolver);
    
    for(const auto& fcn : m_functions)
    {
        TypeParams  new_fcn_params = fcn.data.params();
        for( auto& b : new_fcn_params.bounds() )
            b.type().resolve_args(resolver);
        TypeRef new_ret_type = fcn.data.rettype();
        new_ret_type.resolve_args(resolver);
        Function::Arglist  new_args = fcn.data.args();
        for( auto& t : new_args )
            t.second.resolve_args(resolver);
        
        ret.add_function( fcn.is_pub, fcn.name, Function( ::std::move(new_fcn_params), fcn.data.fcn_class(), ::std::move(new_ret_type), ::std::move(new_args), Expr() ) );
    }
   
    UNINDENT();
    return ret;
*/
}

::rust::option<Impl&> Impl::matches(const TypeRef& trait, const TypeRef& type)
{
    DEBUG("this = " << *this);
    if( m_params.ty_params().size() > 0 )
    {
        ::std::vector<TypeRef>  param_types(m_params.ty_params().size());
        try
        {
            auto c = [&](const char* name,const TypeRef& ty){
                    int idx = m_params.find_name(name);
                    assert( idx >= 0 );
                    assert( (unsigned)idx < param_types.size() );
                    param_types[idx].merge_with( ty );
                };
            m_trait.match_args(trait, c);
            m_type.match_args(type, c);
            
            // Check that conditions match
            // - TODO: Requires locating/checking trait implementations on types
            
            // The above two will throw if matching failed, so if we get here, it's a match
            for( auto& i : m_concrete_impls )
            {
                if( i.first == param_types )
                {
                    return ::rust::option<Impl&>(i.second);
                }
            }
            m_concrete_impls.push_back( make_pair(param_types, this->make_concrete(param_types)) );
            return ::rust::option<Impl&>( m_concrete_impls.back().second );
        }
        catch( const ::std::runtime_error& e )
        {
            DEBUG("No match - " << e.what());
        }
    }
    else
    {
        if( m_trait == trait && m_type == type )
        {
            return ::rust::option<Impl&>( *this );
        }
    }
    return ::rust::option<Impl&>();
}

::std::ostream& operator<<(::std::ostream& os, const Impl& impl)
{
    return os << "impl<" << impl.m_params << "> " << impl.m_trait << " for " << impl.m_type << "";
}
SERIALISE_TYPE(Impl::, "AST_Impl", {
    s << m_params;
    s << m_trait;
    s << m_type;
    s << m_functions;
},{
    s.item(m_params);
    s.item(m_trait);
    s.item(m_type);
    s.item(m_functions);
})

Crate::Crate():
    m_root_module(""),
    m_load_std(true)
{
}

static void iterate_module(Module& mod, ::std::function<void(Module& mod)> fcn)
{
    fcn(mod);
    for( auto& sm : mod.submods() )
        iterate_module(sm.first, fcn);
}

void Crate::post_parse()
{
    // Iterate all modules, grabbing pointers to all impl blocks
    iterate_module(m_root_module, [this](Module& mod){
        for( auto& impl : mod.impls() )
        {
            m_impl_index.push_back( &impl );
        }
    });
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
        return it->second.root_module();
    throw ParseError::Generic("crate name unknown");
}

::rust::option<Impl&> Crate::find_impl(const TypeRef& trait, const TypeRef& type)
{
    DEBUG("trait = " << trait << ", type = " << type);
    
    // TODO: Support autoderef here? NO
    if( trait.is_wildcard() && !type.is_path() )
    {
        // You can only have 'impl <type> { }' for user-defined types (i.e. paths)
        // - Return failure
        return ::rust::option<Impl&>();
    }
    
    for( auto implptr : m_impl_index )
    {
        Impl& impl = *implptr;
        // TODO: What if there's two impls that match this combination?
        ::rust::option<Impl&> oimpl = impl.matches(trait, type);
        if( oimpl.is_some() )
        {
            return oimpl.unwrap();
        }
    }
    DEBUG("No impl of " << trait << " for " << type);
    return ::rust::option<Impl&>();
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
    d.item( ret.crate() );
    
    ret.prescan();
    
    m_extern_crates.insert( make_pair(::std::move(name), ::std::move(ret)) );
}
SERIALISE_TYPE_A(Crate::, "AST_Crate", {
    s.item(m_load_std);
    s.item(m_extern_crates);
    s.item(m_root_module);
})

ExternCrate::ExternCrate()
{
}

ExternCrate::ExternCrate(const char *path)
{
    throw ParseError::Todo("Load extern crate from a file");
}

// Fill runtime-generated structures in the crate
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

SERIALISE_TYPE(ExternCrate::, "AST_ExternCrate", {
},{
})

SERIALISE_TYPE_A(Module::, "AST_Module", {
    s.item(m_name);
    s.item(m_attrs);
    
    s.item(m_extern_crates);
    s.item(m_submods);
    
    s.item(m_macros);
    
    s.item(m_imports);
    s.item(m_type_aliases);
    
    s.item(m_traits);
    s.item(m_enums);
    s.item(m_structs);
    s.item(m_statics);
    
    s.item(m_functions);
    s.item(m_impls);
})

void Module::prescan()
{
    TRACE_FUNCTION;
    DEBUG("- '"<<m_name<<"'"); 
    
    for( auto& sm_p : m_submods )
    {
        sm_p.first.prescan();
    }
    
    for( const auto& macro_imp : m_macro_imports )
    {
        resolve_macro_import( *(Crate*)0, macro_imp.first, macro_imp.second );
    }
}

void Module::resolve_macro_import(const Crate& crate, const ::std::string& modname, const ::std::string& macro_name)
{
    DEBUG("Import macros from " << modname << " matching '" << macro_name << "'");
    for( const auto& sm_p : m_submods )
    {
        const AST::Module& sm = sm_p.first;
        if( sm.name() == modname )
        {
            DEBUG("Using module");
            if( macro_name == "" )
            {
                for( const auto& macro_p : sm.m_macro_import_res )
                    m_macro_import_res.push_back( macro_p );
                for( const auto& macro_i : sm.m_macros )
                    m_macro_import_res.push_back( ItemNS<const MacroRules*>( ::std::string(macro_i.name), &macro_i.data, false ) );
                return ;
            }
            else
            {
                for( const auto& macro_p : sm.m_macro_import_res )
                {
                    if( macro_p.name == macro_name ) {
                        m_macro_import_res.push_back( macro_p );
                        return ;
                    }   
                }
                throw ::std::runtime_error("Macro not in module");
            }
        }
    }
    
    for( const auto& cr : m_extern_crates )
    {
        if( cr.name == modname )
        {
            DEBUG("Using crate import " << cr.name << " == '" << cr.data << "'");
            if( macro_name == "" ) {
                for( const auto& macro_p : crate.extern_crates().at(cr.data).crate().m_exported_macros )
                    m_macro_import_res.push_back( ItemNS<const MacroRules*>( ::std::string(macro_p.first), &*macro_p.second, false ) );
                return ;
            }
            else {
                for( const auto& macro_p : crate.extern_crates().at(cr.data).crate().m_exported_macros )
                {
                    DEBUG("Macro " << macro_p.first);
                    if( macro_p.first == macro_name ) {
                        // TODO: Handle #[macro_export] on extern crate
                        m_macro_import_res.push_back( ItemNS<const MacroRules*>( ::std::string(macro_p.first), &*macro_p.second, false ) );
                        return ;
                    }
                }
                throw ::std::runtime_error("Macro not in crate");
            }
        }
    }
    
    throw ::std::runtime_error( FMT("Could not find sub-module '" << modname << "' for macro import") );
}

void Module::add_macro_import(const Crate& crate, ::std::string modname, ::std::string macro_name)
{
    resolve_macro_import(crate, modname, macro_name);
    m_macro_imports.insert( ::std::make_pair( move(modname), move(macro_name) ) );
}

void Module::iterate_functions(fcn_visitor_t *visitor, const Crate& crate)
{
    for( auto& fcn_item : this->m_functions )
    {
        visitor(crate, *this, fcn_item.data);
    }
}

template<typename T>
typename ::std::vector<Item<T> >::const_iterator find_named(const ::std::vector<Item<T> >& vec, const ::std::string& name)
{
    return ::std::find_if(vec.begin(), vec.end(), [&name](const Item<T>& x) {
        //DEBUG("find_named - x.name = " << x.name);
        return x.name == name;
    });
}

Module::ItemRef Module::find_item(const ::std::string& needle, bool allow_leaves, bool ignore_private_wildcard) const
{
    TRACE_FUNCTION_F("needle = " << needle);
    
    // Sub-modules
    {
        auto& sms = submods();
        auto it = ::std::find_if(sms.begin(), sms.end(), [&needle](const ::std::pair<Module,bool>& x) {
                return x.first.name() == needle;
            });
        if( it != sms.end() ) {
            return ItemRef(it->first);
        }
    }
    
    // External crates
    {
        auto& crates = this->extern_crates();
        auto it = find_named(crates, needle);
        if( it != crates.end() ) {
            return ItemRef(it->data);
        }
    }

    // Type Aliases
    {
        auto& items = this->type_aliases();
        auto it = find_named(items, needle);
        if( it != items.end() ) {
            return ItemRef(it->data);
        }
    }

    // Functions
    {
        auto& items = this->functions();
        auto it = find_named(items, needle);
        if( it != items.end() ) {
            if( allow_leaves )
                return ItemRef(it->data);
            else
                DEBUG("Skipping static, leaves not allowed");
        }
    }

    // Traits
    {
        auto& items = this->traits();
        auto it = find_named(items, needle);
        if( it != items.end() ) {
            return ItemRef(it->data);
        }
    }

    // Structs
    {
        auto& items = this->structs();
        auto it = find_named(items, needle);
        if( it != items.end() ) {
            return ItemRef(it->data);
        }
    }

    // Enums
    {
        auto& items = this->enums();
        auto it = find_named(items, needle);
        if( it != items.end() ) {
            return ItemRef(it->data);
        }
    }

    // Statics
    {
        auto& items = this->statics();
        auto it = find_named(items, needle);
        if( it != items.end() ) {
            if( allow_leaves ) {
                return ItemRef(it->data);
            }
            else
                DEBUG("Skipping static, leaves not allowed");
        }
    }
    
    // - Re-exports
    //  > Comes last, as it's a potentially expensive operation
    {
        for( const auto& imp : this->imports() )
        {
            //DEBUG("imp: '" << imp.name << "' = " << imp.data);
            if( !imp.is_pub && ignore_private_wildcard )
            {
                // not public, ignore
                //DEBUG("Private import, '" << imp.name << "' = " << imp.data);
            }
            else if( imp.name == needle )
            {
                DEBUG("Match " << needle);
                return ItemRef(imp);
            }
            else if( imp.name == "" )
            {
                // Loop avoidance, don't check this
                //if( &imp.data == this )
                //    continue ;
                //
                const auto& binding = imp.data.binding();
                if( !binding.is_bound() )
                {
                    // not yet bound, so run resolution (recursion)
                    DEBUG("Recursively resolving pub wildcard use " << imp.data);
                    //imp.data.resolve(root_crate);
                    throw ParseError::Todo("Path::resolve() wildcard re-export call resolve");
                }
                
                switch(binding.type())
                {
                case AST::PathBinding::UNBOUND:
                    throw ParseError::BugCheck("Wildcard import path not bound");
                // - If it's a module, recurse
                case AST::PathBinding::MODULE: {
                    auto rv = binding.bound_module().find_item(needle);
                    if( rv.type() != Module::ItemRef::ITEM_none ) {
                        // Don't return RV, return the import (so caller can rewrite path if need be)
                        return ItemRef(imp);
                        //return rv;
                    }
                    break; }
                // - If it's an enum, search for this name and then pass to resolve
                case AST::PathBinding::ENUM: {
                    auto& vars = binding.bound_enum().variants();
                    // Damnit C++ "let it = vars.find(|a| a.name == needle);"
                    auto it = ::std::find_if(vars.begin(), vars.end(),
                        [&needle](const EnumVariant& ev) { return ev.m_name == needle; });
                    if( it != vars.end() ) {
                        DEBUG("Found enum variant " << it->m_name);
                        return ItemRef(imp);
                        //throw ParseError::Todo("Handle lookup_path_in_module for wildcard imports - enum");
                    }
                    
                    break; }
                // - otherwise, error
                default:
                    DEBUG("ERROR: Import of invalid class : " << imp.data);
                    throw ParseError::Generic("Wildcard import of non-module/enum");
                }
            }
            else
            {
                // Can't match, ignore
            }
        }
        
    }
    
    return Module::ItemRef();
}

SERIALISE_TYPE(TypeAlias::, "AST_TypeAlias", {
    s << m_params;
    s << m_type;
},{
    s.item(m_params);
    s.item(m_type);
})

::Serialiser& operator<<(::Serialiser& s, Static::Class fc)
{
    switch(fc)
    {
    case Static::CONST:  s << "CONST"; break;
    case Static::STATIC: s << "STATIC"; break;
    case Static::MUT:    s << "MUT"; break;
    }
    return s;
}
void operator>>(::Deserialiser& s, Static::Class& fc)
{
    ::std::string   n;
    s.item(n);
         if(n == "CONST")   fc = Static::CONST;
    else if(n == "STATIC")  fc = Static::STATIC;
    else if(n == "MUT")     fc = Static::MUT;
    else
        throw ::std::runtime_error("Deserialise Static::Class");
}
SERIALISE_TYPE(Static::, "AST_Static", {
    s << m_class;
    s << m_type;
    s << m_value;
},{
    s >> m_class;
    s.item(m_type);
    s.item(m_value);
})

::Serialiser& operator<<(::Serialiser& s, Function::Class fc)
{
    switch(fc)
    {
    case Function::CLASS_UNBOUND: s << "UNBOUND"; break;
    case Function::CLASS_REFMETHOD: s << "REFMETHOD"; break;
    case Function::CLASS_MUTMETHOD: s << "MUTMETHOD"; break;
    case Function::CLASS_VALMETHOD: s << "VALMETHOD"; break;
    case Function::CLASS_MUTVALMETHOD: s << "MUTVALMETHOD"; break;
    }
    return s;
}
void operator>>(::Deserialiser& s, Function::Class& fc)
{
    ::std::string   n;
    s.item(n);
         if(n == "UNBOUND")    fc = Function::CLASS_UNBOUND;
    else if(n == "REFMETHOD")  fc = Function::CLASS_REFMETHOD;
    else if(n == "MUTMETHOD")  fc = Function::CLASS_MUTMETHOD;
    else if(n == "VALMETHOD")  fc = Function::CLASS_VALMETHOD;
    else if(n == "MUTVALMETHOD")  fc = Function::CLASS_MUTVALMETHOD;
    else
        throw ::std::runtime_error("Deserialise Function::Class");
}
SERIALISE_TYPE(Function::, "AST_Function", {
    s << m_fcn_class;
    s << m_params;
    s << m_rettype;
    s << m_args;
    s << m_code;
},{
    s >> m_fcn_class;
    s.item(m_params);
    s.item(m_rettype);
    s.item(m_args);
    s.item(m_code);
})

SERIALISE_TYPE(Trait::, "AST_Trait", {
    s << m_params;
    s << m_types;
    s << m_functions;
},{
    s.item(m_params);
    s.item(m_types);
    s.item(m_functions);
})

SERIALISE_TYPE_A(EnumVariant::, "AST_EnumVariant", {
    s.item(m_name);
    s.item(m_sub_types);
    s.item(m_value);
})

SERIALISE_TYPE(Enum::, "AST_Enum", {
    s << m_params;
    s << m_variants;
},{
    s.item(m_params);
    s.item(m_variants);
})

TypeRef Struct::get_field_type(const char *name, const ::std::vector<TypeRef>& args)
{
    if( args.size() != m_params.ty_params().size() )
    {
        throw ::std::runtime_error("Incorrect parameter count for struct");
    }
    // TODO: Should the bounds be checked here? Or is the count sufficient?
    for(const auto& f : m_fields)
    {
        if( f.name == name )
        {
            // Found it!
            if( args.size() )
            {
                TypeRef res = f.data;
                res.resolve_args( [&](const char *argname){
                    for(unsigned int i = 0; i < m_params.ty_params().size(); i ++)
                    {
                        if( m_params.ty_params()[i].name() == argname ) {
                            return args.at(i);
                        }
                    }
                    throw ::std::runtime_error("BUGCHECK - Unknown arg in field type");
                    });
                return res;
            }
            else
            {
                return f.data;
            }
        }
    }
    
    throw ::std::runtime_error(FMT("No such field " << name));
}

SERIALISE_TYPE(Struct::, "AST_Struct", {
    s << m_params;
    s << m_fields;
},{
    s.item(m_params);
    s.item(m_fields);
})

::std::ostream& operator<<(::std::ostream& os, const TypeParam& tp)
{
    //os << "TypeParam(";
    os << tp.m_name;
    os << " = ";
    os << tp.m_default;
    //os << ")";
    return os;
}
SERIALISE_TYPE(TypeParam::, "AST_TypeParam", {
    s << m_name;
    s << m_default;
},{
    s.item(m_name);
    s.item(m_default);
})

::std::ostream& operator<<(::std::ostream& os, const GenericBound& x)
{
    os << x.m_type << ": ";
    if( x.m_lifetime_bound != "" )
        return os << "'" << x.m_lifetime_bound;
    else
        return os << x.m_trait;
}
SERIALISE_TYPE_S(GenericBound, {
    s.item(m_lifetime_test);
    s.item(m_type);
    s.item(m_lifetime_bound);
    s.item(m_trait);
})

int TypeParams::find_name(const char* name) const
{
    for( unsigned int i = 0; i < m_type_params.size(); i ++ )
    {
        if( m_type_params[i].name() == name )
            return i;
    }
    DEBUG("Type param '" << name << "' not in list");
    return -1;
}

bool TypeParams::check_params(Crate& crate, const ::std::vector<TypeRef>& types) const
{
    return check_params( crate, const_cast< ::std::vector<TypeRef>&>(types), false );
}
bool TypeParams::check_params(Crate& crate, ::std::vector<TypeRef>& types, bool allow_infer) const
{
    // Check parameter counts
    if( types.size() > m_type_params.size() )
    {
        throw ::std::runtime_error(FMT("Too many generic params ("<<types.size()<<" passed, expecting "<< m_type_params.size()<<")"));
    }
    else if( types.size() < m_type_params.size() )
    {
        if( allow_infer )
        {
            while( types.size() < m_type_params.size() )
            {
                types.push_back( m_type_params[types.size()].get_default() );
            }
        }
        else
        {
            throw ::std::runtime_error(FMT("Too few generic params, (" << types.size() << " passed, expecting " << m_type_params.size() << ")"));
        }
    }
    else
    {
        // Counts are good, time to validate types
    }
    
    for( unsigned int i = 0; i < types.size(); i ++ )
    {
        auto& type = types[i];
        auto& param = m_type_params[i].name();
        TypeRef test(TypeRef::TagArg(), param);
        if( type.is_wildcard() )
        {
            for( const auto& bound : m_bounds )
            {
                if( bound.is_trait() && bound.test() == test )
                {
                    const auto& trait = bound.bound();
                    const auto& ty_traits = type.traits();
                
                    auto it = ::std::find(ty_traits.begin(), ty_traits.end(), trait);
                    if( it == ty_traits.end() )
                    {
                        throw ::std::runtime_error( FMT("No matching impl of "<<trait<<" for "<<type));
                    }
                }
            }
        }
        else
        {
            // Not a wildcard!
            // Check that the type fits the bounds applied to it
            for( const auto& bound : m_bounds )
            {
                if( bound.is_trait() && bound.test() == test )
                {
                    const auto& trait = bound.bound();
                    // Check if 'type' impls 'trait'
                    if( !crate.find_impl(type, trait).is_some() )
                    {
                        throw ::std::runtime_error( FMT("No matching impl of "<<trait<<" for "<<type));
                    }
                }
            }
        }
    }
    return true;
}

::std::ostream& operator<<(::std::ostream& os, const TypeParams& tps)
{
    return os << "<" << tps.m_lifetime_params << "," << tps.m_type_params << "> where {" << tps.m_bounds << "}";
}
SERIALISE_TYPE_S(TypeParams, {
    s.item(m_type_params);
    s.item(m_lifetime_params);
    s.item(m_bounds);
})

}
