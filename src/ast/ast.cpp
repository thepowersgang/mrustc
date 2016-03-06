/*
 */
#include "ast.hpp"
#include "crate.hpp"
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

::rust::option<char> ImplRef::find_named_item(const ::std::string& name) const
{
    if( this->impl.has_named_item(name) ) {
        return ::rust::Some(' ');
    }
    else {
        return ::rust::None<char>();
    }
}


SERIALISE_TYPE_A(MacroInvocation::, "AST_MacroInvocation", {
    s.item(m_attrs);
    s.item(m_macro_name);
    s.item(m_ident);
    s.item(m_input);
})

SERIALISE_TYPE_A(Module::, "AST_Module", {
    s.item(m_name);
    
    s.item(m_items);
    
    s.item(m_macros);
    
    s.item(m_imports);
    
    s.item(m_impls);
})

void Module::add_item(bool is_pub, ::std::string name, Item it, MetaItems attrs) {
    m_items.push_back( Named<Item>( mv$(name), mv$(it), is_pub ) );
    m_items.back().data.attrs = mv$(attrs);
}
void Module::add_ext_crate(::std::string ext_name, ::std::string imp_name, MetaItems attrs) {
    // TODO: Extern crates can be public
    this->add_item( false, imp_name, Item::make_Crate({mv$(ext_name)}), mv$(attrs) );
}
void Module::add_alias(bool is_public, Path path, ::std::string name, MetaItems attrs) {
    // TODO: Attributes on aliases / imports
    m_imports.push_back( Named<Path>( move(name), move(path), is_public) );
}
void Module::add_typealias(bool is_public, ::std::string name, TypeAlias alias, MetaItems attrs) {
    this->add_item( is_public, name, Item::make_Type({mv$(alias)}), mv$(attrs) );
}
void Module::add_static(bool is_public, ::std::string name, Static item, MetaItems attrs) {
    this->add_item( is_public, name, Item::make_Static({mv$(item)}), mv$(attrs) );
}
void Module::add_trait(bool is_public, ::std::string name, Trait item, MetaItems attrs) {
    this->add_item( is_public, name, Item::make_Trait({mv$(item)}), mv$(attrs) );
}
void Module::add_struct(bool is_public, ::std::string name, Struct item, MetaItems attrs) {
    this->add_item( is_public, name, Item::make_Struct({mv$(item)}), mv$(attrs) );
}
void Module::add_enum(bool is_public, ::std::string name, Enum item, MetaItems attrs) {
    this->add_item( is_public, name, Item::make_Enum({mv$(item)}), mv$(attrs) );
}
void Module::add_function(bool is_public, ::std::string name, Function item, MetaItems attrs) {
    this->add_item( is_public, name, Item::make_Function({mv$(item)}), mv$(attrs) );
}
void Module::add_submod(bool is_public, Module mod, MetaItems attrs) {
    auto name = mod.m_name;
    DEBUG("mod.m_name = " << name);
    this->add_item( is_public, mv$(name), Item::make_Module({mv$(mod)}), mv$(attrs) );
}

void Module::prescan()
{
    //TRACE_FUNCTION;
    //DEBUG("- '"<<m_name<<"'"); 
    //
    //for( auto& sm_p : m_submods )
    //{
    //    sm_p.first.prescan();
    //}
    //
    ///*
    //for( const auto& macro_imp : m_macro_imports )
    //{
    //    resolve_macro_import( *(Crate*)0, macro_imp.first, macro_imp.second );
    //}
    //*/
}

void Module::resolve_macro_import(const Crate& crate, const ::std::string& modname, const ::std::string& macro_name)
{
    /*
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
    */
}

void Module::iterate_functions(fcn_visitor_t *visitor, const Crate& crate)
{
    for( auto& item : this->m_items )
    {
        TU_MATCH_DEF(::AST::Item, (item.data), (e),
        ( ),
        (Function,
            visitor(crate, *this, e.e);
            )
        )
    }
}

template<typename T>
typename ::std::vector<Named<T> >::const_iterator find_named(const ::std::vector<Named<T> >& vec, const ::std::string& name)
{
    return ::std::find_if(vec.begin(), vec.end(), [&name](const Named<T>& x) {
        DEBUG("find_named - x.name = " << x.name);
        return x.name == name;
    });
}

Module::ItemRef Module::find_item(const ::std::string& needle, bool allow_leaves, bool ignore_private_wildcard) const
{
    TRACE_FUNCTION_F("needle = " << needle);
    
    {
        auto it = find_named(this->items(), needle);
        if( it != this->items().end() ) {
            TU_MATCH(::AST::Item, (it->data), (e),
            (None,
                break;
                ),
            (Module, return ItemRef(e.e); ),
            (Crate, return ItemRef(e.name); ),
            (Type,  return ItemRef(e.e); ),
            (Struct, return ItemRef(e.e); ),
            (Enum,  return ItemRef(e.e); ),
            (Trait, return ItemRef(e.e); ),
            (Function,
                if( allow_leaves )
                    return ItemRef(e.e);
                else
                    DEBUG("Skipping function, leaves not allowed");
                ),
            (Static,
                if( allow_leaves )
                    return ItemRef(e.e);
                else
                    DEBUG("Skipping function, leaves not allowed");
                )
            )
            DEBUG("Item not checked at this level, try re-export list");
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

SERIALISE_TYPE(Item::, "AST_Item", {
    s.item(attrs);
},{
    s.item(attrs);
})

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
