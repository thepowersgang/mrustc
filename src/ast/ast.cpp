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

bool ImplDef::matches(::std::vector<TypeRef>& out_types, const Path& trait, const TypeRef& type) const
{
    // 1. Check the type/trait counting parameters as wildcards (but flagging if one was seen)
    //  > If that fails, just early return
    int trait_match = m_trait.equal_no_generic(trait);
    if( trait_match < 0 )
        return false;
    int type_match = m_type.equal_no_generic(type);
    if( type_match < 0 )
        return false;
    DEBUG("Match Tr:" <<trait_match << ", Ty:" << type_match << " for Trait " << trait << ", Type " << type);
    
    // 2. If a parameter was seen, do the more expensive generic checks
    //  > Involves checking that parameters are valid
    if( m_params.ty_params().size() )
    {
        if( trait_match == 0 && type_match == 0 )
            throw CompileError::Generic( "Unbound generic in impl" );
    } 
    
    // If there was a fuzzy match, then make it less fuzzy.
    if( !(trait_match == 0 && type_match == 0) )
    {
        out_types.clear();
        out_types.resize(m_params.ty_params().size());
        try
        {
            auto c = [&](const char* name,const TypeRef& ty) {
                    if( strcmp(name, "Self") == 0 ) {
                        if( ty != type )
                            throw CompileError::Generic(FMT("Self mismatch : " << ty));
                        return ;
                    }
                    int idx = m_params.find_name(name);
                    assert( idx >= 0 );
                    assert( (unsigned)idx < out_types.size() );
                    out_types[idx].merge_with( ty );
                };
            m_trait.match_args(trait, c);
            m_type.match_args(type, c);
        }
        catch(const CompileError::Base& e)
        {
            DEBUG("No match - " << e.what());
            return false;
        }
        
        // TODO: Validate params against bounds?
    }
    
    // Perfect match
    return true;
}
::std::ostream& operator<<(::std::ostream& os, const ImplDef& impl)
{
    return os << "impl<" << impl.m_params << "> " << impl.m_trait << " for " << impl.m_type << "";
}
SERIALISE_TYPE(ImplDef::, "AST_ImplDef", {
    s << m_params;
    s << m_trait;
    s << m_type;
},{
    s.item(m_params);
    s.item(m_trait);
    s.item(m_type);
})

bool Impl::has_named_item(const ::std::string& name) const
{
    for( const auto& it : this->functions() )
    {
        if( it.name == name ) {
            return true;
        }
    }
    return false;
}

Impl& Impl::get_concrete(const ::std::vector<TypeRef>& param_types)
{
    if( param_types.size() > 0 )
    { 
        for( auto& i : m_concrete_impls )
        {
            if( i.first == param_types )
            {
                return i.second;
            }
        }
        
        m_concrete_impls.push_back( make_pair(param_types, this->make_concrete(param_types)) );
        return m_concrete_impls.back().second;
    }
    else
    {
        return *this;
    }
}

Impl Impl::make_concrete(const ::std::vector<TypeRef>& types) const
{
    TRACE_FUNCTION_F("*this = " << *this << ", types={" << types << "}");
    assert(m_def.params().ty_params().size());
    
    GenericResolveClosure   resolver(m_def.params(), types);
    
    Impl    ret( MetaItems(), GenericParams(), m_def.type(), m_def.trait() );
    ret.m_def.trait().resolve_args( resolver );
    ret.m_def.type().resolve_args( resolver );
    
    throw ParseError::Todo("Impl::make_concrete");
/*
    for(const auto& fcn : m_functions)
    {
        GenericParams  new_fcn_params = fcn.data.params();
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

::std::ostream& operator<<(::std::ostream& os, const Impl& impl)
{
    return os << impl.m_def;
}
SERIALISE_TYPE(Impl::, "AST_Impl", {
    s << m_def;
    s << m_functions;
},{
    s.item(m_def);
    s.item(m_functions);
})

Crate::Crate():
    m_root_module(MetaItems(), ""),
    m_load_std(true)
{
}

::rust::option<char> ImplRef::find_named_item(const ::std::string& name) const
{
    if( this->impl.has_named_item(name) ) {
        return ::rust::Some(' ');
    }
    else {
        return ::rust::None<char>();
    }
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
    auto cb = [this](Module& mod){
        for( auto& impl : mod.impls() )
            m_impl_index.push_back( &impl );
        for( auto& impl : mod.neg_impls() )
            m_neg_impl_index.push_back( &impl );
        };
    iterate_module(m_root_module, cb);
    iterate_module(g_compiler_module, cb);

    // Create a map of inherent impls
    for( const auto& impl : m_impl_index )
    {
        if( impl->def().trait().is_valid() == false )
        {
            auto& ent = m_impl_map[impl->def().type()];
            ent.push_back( impl );
        }
    }
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
                DEBUG("Skipping function, leaves not allowed");
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
                DEBUG("Match " << needle << " = " << imp.data);
                return ItemRef(imp);
            }
            else if( imp.name == "" )
            {
                // Loop avoidance, don't check this
                //if( &imp.data == this )
                //    continue ;
                //
                const auto& binding = imp.data.binding();
                if( binding.is_Unbound() )
                {
                    // not yet bound, so run resolution (recursion)
                    DEBUG("Recursively resolving pub wildcard use " << imp.data);
                    //imp.data.resolve(root_crate);
                    throw ParseError::Todo("AST::Module::find_item() - Wildcard `use` not bound, call resolve here?");
                }
                
                TU_MATCH_DEF(AST::PathBinding, (binding), (info),
                // - any other type - error
                (
                    DEBUG("ERROR: Import of invalid class : " << imp.data);
                    throw ParseError::Generic("Wildcard import of non-module/enum");
                    ),
                (Unbound,
                    throw ParseError::BugCheck("Wildcard import path not bound");
                    ),
                // - If it's a module, recurse
                (Module,
                    auto rv = info.module_->find_item(needle);
                    if( !rv.is_None() ) {
                        // Don't return RV, return the import (so caller can rewrite path if need be)
                        return ItemRef(imp);
                        //return rv;
                    }
                    ),
                // - If it's an enum, search for this name and then pass to resolve
                (Enum,
                    auto& vars = info.enum_->variants();
                    // Damnit C++ "let it = vars.find(|a| a.name == needle);"
                    auto it = ::std::find_if(vars.begin(), vars.end(),
                        [&needle](const EnumVariant& ev) { return ev.m_name == needle; });
                    if( it != vars.end() ) {
                        DEBUG("Found enum variant " << it->m_name);
                        return ItemRef(imp);
                        //throw ParseError::Todo("Handle lookup_path_in_module for wildcard imports - enum");
                    }
                    )
                )
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

SERIALISE_TYPE(Function::, "AST_Function", {
    s << m_params;
    s << m_rettype;
    s << m_args;
    s << m_code;
},{
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
                res.resolve_args( GenericResolveClosure(m_params, args) );
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
    TU_MATCH(GenericBound, (x), (ent),
    (Lifetime,
        os << "'" << ent.test << ": '" << ent.bound;
        ),
    (TypeLifetime,
        os << ent.type << ": '" << ent.bound;
        ),
    (IsTrait,
        if( ! ent.hrls.empty() )
        {
            os << "for<";
            for(const auto& l : ent.hrls)
                os << "'" << l;
            os << ">";
        }
        os << ent.type << ":  " << ent.trait;
        ),
    (MaybeTrait,
        os << ent.type << ": ?" << ent.trait;
        ),
    (NotTrait,
        os << ent.type << ": !" << ent.trait;
        ),
    (Equality,
        os << ent.type << " = " << ent.replacement;
        )
    )
    return os;
}


#define SERIALISE_TU_ARM(CLASS, NAME, TAG, ...)    case CLASS::TAG_##TAG: { *this = CLASS::make_null_##TAG(); auto& NAME = this->as_##TAG(); (void)&NAME; __VA_ARGS__ } break;
#define SERIALISE_TU_ARMS(CLASS, NAME, ...)    TU_GMA(__VA_ARGS__)(SERIALISE_TU_ARM, (CLASS, NAME), __VA_ARGS__)
#define SERIALISE_TU(PATH, TAG, NAME, ...) \
    void operator%(::Serialiser& s, PATH::Tag c) { s << PATH::tag_to_str(c); } \
    void operator%(::Deserialiser& s, PATH::Tag& c) { ::std::string n; s.item(n); c = PATH::tag_from_str(n); }\
    SERIALISE_TYPE(PATH::, TAG, {\
        s % this->tag(); TU_MATCH(PATH, ((*this)), (NAME), __VA_ARGS__)\
    }, {\
        PATH::Tag tag; s % tag; switch(tag) { SERIALISE_TU_ARMS(PATH, NAME, __VA_ARGS__) } \
    })

SERIALISE_TU(GenericBound, "GenericBound", ent,
    (Lifetime,
        s.item(ent.test);
        s.item(ent.bound);
        ),
    (TypeLifetime,
        s.item(ent.type);
        s.item(ent.bound);
        ),
    (IsTrait,
        s.item(ent.type);
        s.item(ent.hrls);
        s.item(ent.trait);
        ),
    (MaybeTrait,
        s.item(ent.type);
        s.item(ent.trait);
        ),
    (NotTrait,
        s.item(ent.type);
        s.item(ent.trait);
        ),
    (Equality,
        s.item(ent.type);
        s.item(ent.replacement);
        )
)

int GenericParams::find_name(const char* name) const
{
    for( unsigned int i = 0; i < m_type_params.size(); i ++ )
    {
        if( m_type_params[i].name() == name )
            return i;
    }
    DEBUG("Type param '" << name << "' not in list");
    return -1;
}

bool GenericParams::check_params(Crate& crate, const ::std::vector<TypeRef>& types) const
{
    return check_params( crate, const_cast< ::std::vector<TypeRef>&>(types), false );
}
bool GenericParams::check_params(Crate& crate, ::std::vector<TypeRef>& types, bool allow_infer) const
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
                if( bound.is_IsTrait() && bound.as_IsTrait().type == test )
                {
                    const auto& trait = bound.as_IsTrait().trait;
                    //const auto& ty_traits = type.traits();
                
                    //auto it = ::std::find(ty_traits.begin(), ty_traits.end(), trait);
                    //if( it == ty_traits.end() )
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
                if( bound.is_IsTrait() && bound.as_IsTrait().type == test )
                {
                    const auto& trait = bound.as_IsTrait().trait;
                    // Check if 'type' impls 'trait'
                    if( !crate.find_impl(trait, trait, nullptr, nullptr) )
                    {
                        throw ::std::runtime_error( FMT("No matching impl of "<<trait<<" for "<<type));
                    }
                }
            }
        }
    }
    return true;
}

::std::ostream& operator<<(::std::ostream& os, const GenericParams& tps)
{
    return os << "<" << tps.m_lifetime_params << "," << tps.m_type_params << "> where {" << tps.m_bounds << "}";
}
SERIALISE_TYPE_S(GenericParams, {
    s.item(m_type_params);
    s.item(m_lifetime_params);
    s.item(m_bounds);
})

}
