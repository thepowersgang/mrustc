/*
 */
#include "ast.hpp"
#include "../types.hpp"
#include <iostream>
#include "../parse/parseerror.hpp"
#include <algorithm>

namespace AST {

ExternCrate ExternCrate_std();

SERIALISE_TYPE(MetaItem::, "AST_MetaItem", {
    s << m_name;
    s << m_str_val;
    s << m_items;
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


SERIALISE_TYPE(Impl::, "AST_Impl", {
    s << m_params;
    s << m_trait;
    s << m_type;
    s << m_functions;
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
    if( name == "std" )
    {
        // HACK! Load std using a hackjob (included within the compiler)
        m_extern_crates.insert( make_pair( ::std::move(name), ExternCrate_std() ) );
    }
    else
    {
        throw ParseError::Todo("'extern crate' (not hackjob std)");
    }
}
SERIALISE_TYPE(Crate::, "AST_Crate", {
    s << m_load_std;
    s << m_extern_crates;
    s << m_root_module;
})

ExternCrate::ExternCrate()
{
}

ExternCrate::ExternCrate(const char *path)
{
    throw ParseError::Todo("Load extern crate from a file");
}
SERIALISE_TYPE(ExternCrate::, "AST_ExternCrate", {
})

ExternCrate ExternCrate_std()
{
    ExternCrate crate;
    
    Module& std_mod = crate.root_module();
    
    // === Add modules ===
    // - option
    Module  option("option");
    option.add_enum(true, "Option", Enum(
        {
            TypeParam(false, "T"),
        },
        {
            StructItem("None", TypeRef()),
            StructItem("Some", TypeRef(TypeRef::TagArg(), "T")),
        }
        ));
    std_mod.add_submod(true, ::std::move(option));
    // - result
    Module  result("result");
    result.add_enum(true, "Result", Enum(
        {
            TypeParam(false, "R"),
            TypeParam(false, "E"),
        },
        {
            StructItem("Ok", TypeRef(TypeRef::TagArg(), "R")),
            StructItem("Err", TypeRef(TypeRef::TagArg(), "E")),
        }
        ));
    std_mod.add_submod(true, ::std::move(result));
    // - io
    Module  io("io");
    io.add_typealias(true, "IoResult", TypeAlias(
        { TypeParam(false, "T") },
        TypeRef( Path("std", {
            PathNode("result",{}),
            PathNode("Result", {TypeRef("T"),TypeRef(Path("std", {PathNode("io"), PathNode("IoError")}))})
            }) )
        ));
    std_mod.add_submod(true, ::std::move(io));
    // - iter
    {
        Module  iter("iter");
        #if 0
        {
            Trait   iterator;
            iterator.add_type("Item", TypeRef());
            //iterator.add_function("next", Function({}, Function::CLASS_REFMETHOD, "Option<<Self as Iterator>::Item>", {}, Expr());
            iter.add_trait(true, "Iterator", ::std::move(iterator));
        }
        #endif
        std_mod.add_submod(true, ::std::move(iter));
    }
    
    // - prelude
    Module  prelude("prelude");
    // Re-exports
    #define USE(mod, name, ...)    do{ Path p("std", {__VA_ARGS__}); mod.add_alias(true, ::std::move(p), name); } while(0)
    USE(prelude, "Option",  PathNode("option", {}), PathNode("Option",{}) );
    USE(prelude, "Some",  PathNode("option", {}), PathNode("Option",{}), PathNode("Some",{}) );
    USE(prelude, "None",  PathNode("option", {}), PathNode("Option",{}), PathNode("None",{}) );
    USE(prelude, "Result", PathNode("result", {}), PathNode("Result",{}) );
    USE(prelude, "Ok",  PathNode("result", {}), PathNode("Result",{}), PathNode("Ok",{}) );
    USE(prelude, "Err", PathNode("result", {}), PathNode("Result",{}), PathNode("Err",{}) );
    std_mod.add_submod(true, prelude);
    
    return crate;
}

SERIALISE_TYPE(Module::, "AST_Module", {
    s << m_name;
    s << m_attrs;
    s << m_extern_crates;
    s << m_submods;
    s << m_enums;
    s << m_structs;
    s << m_functions;
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
SERIALISE_TYPE(Static::, "AST_Static", {
    s << m_class;
    s << m_type;
    //s << m_value;
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
SERIALISE_TYPE(Function::, "AST_Function", {
    s << m_fcn_class;
    s << m_generic_params;
    s << m_rettype;
    s << m_args;
    //s << m_code;
})

SERIALISE_TYPE(Trait::, "AST_Trait", {
    s << m_params;
    s << m_types;
    s << m_functions;
})

SERIALISE_TYPE(Enum::, "AST_Enum", {
    s << m_params;
    s << m_variants;
})

SERIALISE_TYPE(Struct::, "AST_Struct", {
    s << m_params;
    s << m_fields;
})

void TypeParam::addLifetimeBound(::std::string name)
{

}
void TypeParam::addTypeBound(TypeRef type)
{

}
::std::ostream& operator<<(::std::ostream& os, const TypeParam& tp)
{
    os << "TypeParam(";
    switch(tp.m_class)
    {
    case TypeParam::LIFETIME:  os << "'";  break;
    case TypeParam::TYPE:      os << "";   break;
    }
    os << tp.m_name;
    if( tp.m_trait_bounds.size() )
    {
        os << ": [" << tp.m_trait_bounds << "]";
    }
    os << ")";
    return os;
}
SERIALISE_TYPE(TypeParam::, "AST_TypeParam", {
    // TODO: TypeParam
})

}
