/*
 */
#include "ast.hpp"
#include "../types.hpp"
#include <iostream>
#include "../parse/parseerror.hpp"
#include <algorithm>
#include <serialiser_texttree.hpp>

namespace AST {

SERIALISE_TYPE(MetaItem::, "AST_MetaItem", {
    s << m_name;
    s << m_str_val;
    s << m_items;
},{
    s.item(m_name);
    s.item(m_str_val);
    s.item(m_items);
})

::std::ostream& operator<<(::std::ostream& os, const Pattern& pat)
{
    switch(pat.m_class)
    {
    case Pattern::ANY:
        os << "Pattern(TagWildcard, '" << pat.m_binding << "' @ _)";
        break;
    case Pattern::MAYBE_BIND:
        os << "Pattern(TagMaybeBind, '" << pat.m_binding << "')";
        break;
    case Pattern::VALUE:
        //os << "Pattern(TagValue, " << *pat.m_node << ")";
        os << "Pattern(TagValue, '" << pat.m_binding << "' @ TODO:ExprNode)";
        break;
    case Pattern::TUPLE:
        os << "Pattern(TagTuple, '" << pat.m_binding << "' @ [" << pat.m_sub_patterns << "])";
        break;
    case Pattern::TUPLE_STRUCT:
        os << "Pattern(TagEnumVariant, '" << pat.m_binding << "' @ " << pat.m_path << ", [" << pat.m_sub_patterns << "])";
        break;
    }
    return os;
}
void operator%(Serialiser& s, Pattern::BindType c) {
    switch(c)
    {
    case Pattern::ANY:          s << "ANY";         return;
    case Pattern::MAYBE_BIND:   s << "MAYBE_BIND";  return;
    case Pattern::VALUE:        s << "VALUE";  return;
    case Pattern::TUPLE:        s << "TUPLE";  return;
    case Pattern::TUPLE_STRUCT: s << "TUPLE_STRUCT";  return;
    }
}
void operator%(::Deserialiser& s, Pattern::BindType& c) {
    ::std::string   n;
    s.item(n);
         if(n == "ANY")         c = Pattern::ANY;
    else if(n == "MAYBE_BIND")  c = Pattern::MAYBE_BIND;
    else
        throw ::std::runtime_error("");
}
SERIALISE_TYPE_S(Pattern, {
    s % m_class;
    s.item(m_binding);
    s.item(m_sub_patterns);
    s.item(m_path);
});


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
    
    m_extern_crates.insert( make_pair(::std::move(name), ::std::move(ret)) );
}
SERIALISE_TYPE(Crate::, "AST_Crate", {
    s << m_load_std;
    s << m_extern_crates;
    s << m_root_module;
},{
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
SERIALISE_TYPE(ExternCrate::, "AST_ExternCrate", {
},{
})

SERIALISE_TYPE(Module::, "AST_Module", {
    s << m_name;
    s << m_attrs;
    
    s << m_extern_crates;
    s << m_submods;
    
    s << m_imports;
    s << m_type_aliases;
    
    s << m_traits;
    s << m_enums;
    s << m_structs;
    s << m_statics;
    
    s << m_functions;
    s << m_impls;
},{
    s.item(m_name);
    s.item(m_attrs);
    
    s.item(m_extern_crates);
    s.item(m_submods);
    
    s.item(m_imports);
    s.item(m_type_aliases);
    
    s.item(m_traits);
    s.item(m_enums);
    s.item(m_structs);
    s.item(m_statics);
    
    s.item(m_functions);
    s.item(m_impls);
})
void Module::add_ext_crate(::std::string ext_name, ::std::string int_name)
{
    DEBUG("add_ext_crate(\"" << ext_name << "\" as " << int_name << ")");
    m_extern_crates.push_back( Item< ::std::string>( ::std::move(int_name), ::std::move(ext_name), false ) );
}
void Module::iterate_functions(fcn_visitor_t *visitor, const Crate& crate)
{
    for( auto fcn_item : this->m_functions )
    {
        visitor(crate, *this, fcn_item.data);
    }
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

SERIALISE_TYPE(Enum::, "AST_Enum", {
    s << m_params;
    s << m_variants;
},{
    s.item(m_params);
    s.item(m_variants);
})

SERIALISE_TYPE(Struct::, "AST_Struct", {
    s << m_params;
    s << m_fields;
},{
    s.item(m_params);
    s.item(m_fields);
})

::std::ostream& operator<<(::std::ostream& os, const TypeParam& tp)
{
    os << "TypeParam(";
    switch(tp.m_class)
    {
    case TypeParam::LIFETIME:  os << "'";  break;
    case TypeParam::TYPE:      os << "";   break;
    }
    os << tp.m_name;
    os << " = ";
    os << tp.m_default;
    os << ")";
    return os;
}
SERIALISE_TYPE(TypeParam::, "AST_TypeParam", {
    const char *classstr = "-";
    switch(m_class)
    {
    case TypeParam::LIFETIME: classstr = "Lifetime";    break;
    case TypeParam::TYPE:       classstr = "Type";    break;
    }
    s << classstr;
    s << m_name;
    s << m_default;
},{
    {
        ::std::string   n;
        s.item(n);
             if(n == "Lifetime") m_class = TypeParam::LIFETIME;
        else if(n == "Type")     m_class = TypeParam::TYPE;
        else    throw ::std::runtime_error("");
    }
    s.item(m_name);
    s.item(m_default);
})

::std::ostream& operator<<(::std::ostream& os, const GenericBound& x)
{
    return os << "GenericBound(" << x.m_argname << "," << x.m_lifetime << "," << x.m_trait << ")";
}
SERIALISE_TYPE_S(GenericBound, {
    s.item(m_argname);
    s.item(m_lifetime);
    s.item(m_trait);
})

::std::ostream& operator<<(::std::ostream& os, const TypeParams& tps)
{
    return os << "TypeParams({" << tps.m_params << "}, {" << tps.m_bounds << "})";
}
SERIALISE_TYPE_S(TypeParams, {
    s.item(m_params);
    s.item(m_bounds);
})

}
