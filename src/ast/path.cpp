/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/path.cpp
 * - AST::Path and friends
 */
#include "path.hpp"
#include "ast.hpp"
#include "types.hpp"
#include <iostream>
#include "../parse/parseerror.hpp"
#include <algorithm>
#include "expr.hpp"

#define PRETTY_PATH_PRINT   1

namespace AST {

// --- AST::PathBinding
::std::ostream& operator<<(::std::ostream& os, const PathBinding_Type& x) {
    TU_MATCHA( (x), (i),
    (Unbound, os << "_";   ),
    (Crate ,  os << "Crate";    ),
    (Module,  os << "Module";    ),
    (Trait,     os << "Trait";   ),
    (TraitAlias, os << "TraitAlias"; ),
    (Struct,    os << "Struct";  ),
    (Enum,      os << "Enum";    ),
    (Union,     os << "Union";   ),
    (EnumVar,   os << "EnumVar(" << i.idx << ")"; ),
    (TypeAlias, os << "TypeAlias";),
    (TypeParameter, os << "TyParam(" << i.slot << ")"; )
    )
    return os;
}
PathBinding_Type PathBinding_Type::clone() const
{
    TU_MATCHA( (*this), (e),
    (Unbound  , return PathBinding_Type::make_Unbound({}); ),
    (Module   , return PathBinding_Type::make_Module(e);   ),
    (Crate    , return PathBinding_Type(e); ),
    (Trait    , return PathBinding_Type(e); ),
    (TraitAlias, return PathBinding_Type(e); ),
    (Struct   , return PathBinding_Type(e); ),
    (Enum     , return PathBinding_Type(e); ),
    (Union    , return PathBinding_Type(e); ),
    (TypeAlias, return PathBinding_Type::make_TypeAlias(e); ),
    (EnumVar  , return PathBinding_Type::make_EnumVar(e);  ),

    (TypeParameter, return PathBinding_Type::make_TypeParameter(e); )
    )
    throw "BUG: Fell off the end of PathBinding_Type::clone";
}
::std::ostream& operator<<(::std::ostream& os, const PathBinding_Value& x) {
    TU_MATCHA( (x), (i),
    (Unbound, os << "_";   ),
    (Struct,    os << "Struct";  ),
    (Static,    os << "Static";  ),
    (Function,  os << "Function";),
    (EnumVar,  os << "EnumVar(" << i.idx << ")"; ),
    (Generic, os << "Param(" << i.index << ")"; ),
    (Variable, os << "Var(" << i.slot << ")"; )
    )
    return os;
}
PathBinding_Value PathBinding_Value::clone() const
{
    TU_MATCHA( (*this), (e),
    (Unbound , return PathBinding_Value::make_Unbound({}); ),
    (Struct  , return PathBinding_Value(e); ),
    (Static  , return PathBinding_Value(e); ),
    (Function, return PathBinding_Value(e); ),
    (EnumVar , return PathBinding_Value::make_EnumVar(e);  ),
    (Generic, return PathBinding_Value::make_Generic(e); ),
    (Variable, return PathBinding_Value::make_Variable(e); )
    )
    throw "BUG: Fell off the end of PathBinding_Value::clone";
}
::std::ostream& operator<<(::std::ostream& os, const PathBinding_Macro& x) {
    TU_MATCHA( (x), (i),
    (Unbound, os << "_";   ),
    (ProcMacroDerive,
        os << "ProcMacroDerive(? " << i.mac_name << ")";
        ),
    (ProcMacroAttribute,
        os << "ProcMacroAttribute(? " << i.mac_name << ")";
        ),
    (ProcMacro,
        os << "ProcMacro(? " << i.mac_name << ")";
        ),
    (MacroRules,
        os << "MacroRules(? ?)";
        )
    )
    return os;
}
PathBinding_Macro PathBinding_Macro::clone() const
{
    TU_MATCHA( (*this), (e),
    (Unbound , return PathBinding_Macro::make_Unbound({}); ),
    (ProcMacroDerive, return PathBinding_Macro(e); ),
    (ProcMacroAttribute, return PathBinding_Macro(e); ),
    (ProcMacro, return PathBinding_Macro(e); ),
    (MacroRules, return PathBinding_Macro(e); )
    )
    throw "BUG: Fell off the end of PathBinding_Macro::clone";
}

::std::ostream& operator<<(::std::ostream& os, const PathParams& x)
{
    bool needs_comma = false;
    os << "<";
    for(const auto& e : x.m_entries)
    {
        if(e.is_Null())
            continue;
        if(needs_comma) os << ", ";
        needs_comma = true;

        e.fmt(os);
    }
    os << ">";
    return os;
}
PathParams::PathParams(const PathParams& x)
{
    m_entries.reserve( x.m_entries.size() );
    for(const auto& e : x.m_entries)
        m_entries.push_back(e.clone());
}
Ordering PathParams::ord(const PathParams& x) const
{
    return ::ord(m_entries, x.m_entries);
}

PathParamEnt PathParamEnt::clone() const
{
    TU_MATCH_HDRA( (*this), {)
    TU_ARMA(Null, v) {
        return v;
        }
    TU_ARMA(Lifetime, v) {
        return v;
        }
    TU_ARMA(Type, v) {
        return v.clone();
        }
    TU_ARMA(Value, v) {
        return v->clone();
        }
    TU_ARMA(AssociatedTyEqual, v) {
        return ::std::make_pair(v.first, v.second.clone());
        }
    TU_ARMA(AssociatedTyBound, v) {
        return ::std::make_pair(v.first, v.second);
        }
    }
    throw "";
}
Ordering PathParamEnt::ord(const PathParamEnt& x) const
{
    if(this->tag() != x.tag())
        return ::ord( static_cast<int>(this->tag()), static_cast<int>(x.tag()) );
    
    TU_MATCH_HDRA( (*this, x), {)
    TU_ARMA(Null, v1, v2) {
        return ::OrdEqual;
        }
    TU_ARMA(Lifetime, v1, v2) {
        return ::ord(v1, v2);
        }
    TU_ARMA(Type, v1, v2) {
        return ::ord(v1, v2);
        }
    TU_ARMA(Value, v1, v2) {
        return ::ord( (uintptr_t)v1.get(), (uintptr_t)v2.get() );
        }
    TU_ARMA(AssociatedTyEqual, v1, v2) {
        return ::ord(v1, v2);
        }
    TU_ARMA(AssociatedTyBound, v1, v2) {
        return ::ord(v1, v2);
        }
    }
    throw "";
}
void PathParamEnt::fmt(::std::ostream& os) const
{
    TU_MATCH_HDRA( (*this), {)
    TU_ARMA(Null, _) {
        os << "/*removed*/";
        }
    TU_ARMA(Lifetime, v) {
        os << v;
        }
    TU_ARMA(Type, v) {
        os << v;
        }
    TU_ARMA(Value, v) {
        v->print(os);
        }
    TU_ARMA(AssociatedTyEqual, v) {
        os << v.first << "=" << v.second;
        }
    TU_ARMA(AssociatedTyBound, v) {
        os << v.first << ": " << v.second;
        }
    }
}

// --- AST::PathNode
PathNode::PathNode(RcString name, PathParams args):
    m_name( mv$(name) ),
    m_params( mv$(args) )
{
}
Ordering PathNode::ord(const PathNode& x) const
{
    Ordering    rv;
    rv = ::ord(m_name, x.m_name);
    if(rv != OrdEqual)  return rv;
    rv = m_params.ord(x.m_params);
    if(rv != OrdEqual)  return rv;
    return OrdEqual;
}
void PathNode::print_pretty(::std::ostream& os, bool is_type_context) const
{
    os << m_name;
    if( ! m_params.is_empty() )
    {
        if( ! is_type_context )
            os << "::";
        os << m_params;
    }
}
::std::ostream& operator<<(::std::ostream& os, const PathNode& pn) {
    pn.print_pretty(os, false);
    return os;
}

/// Return an iterator to the named item
template<typename T>
typename ::std::vector<Named<T> >::const_iterator find_named(const ::std::vector<Named<T> >& vec, const ::std::string& name)
{
    return ::std::find_if(vec.begin(), vec.end(), [&name](const Named<T>& x) {
        return x.name == name;
    });
}

// --- AST::Path
AST::Path::~Path()
{
}
AST::Path AST::Path::new_ufcs_ty(TypeRef type, ::std::vector<AST::PathNode> nodes)
{
    return AST::Path( AST::Path::Class::make_UFCS({box$(type), nullptr, nodes}) );
}
AST::Path AST::Path::new_ufcs_trait(TypeRef type, Path trait, ::std::vector<AST::PathNode> nodes)
{
    return AST::Path( AST::Path::Class::make_UFCS({box$(type), box$(trait), nodes}) );
}
AST::Path::Path(const Path& x):
    m_class()
    ,m_bindings(x.m_bindings.clone())
{
    TU_MATCH(Class, (x.m_class), (ent),
    (Invalid,
        m_class = Class::make_Invalid({});
        ),
    (Local,
        m_class = Class::make_Local({ent.name});
        ),
    (Relative,
        m_class = Class::make_Relative({ent.hygiene, ent.nodes});
        ),
    (Self,
        m_class = Class::make_Self({ent.nodes});
        ),
    (Super,
        m_class = Class::make_Super({ent.count, ent.nodes});
        ),
    (Absolute,
        m_class = Class::make_Absolute({ent.crate, ent.nodes});
        ),
    (UFCS,
        if( ent.trait )
            m_class = Class::make_UFCS({ box$(ent.type->clone()), ::std::unique_ptr<Path>(new Path(*ent.trait)), ent.nodes });
        else
            m_class = Class::make_UFCS({ box$(ent.type->clone()), nullptr, ent.nodes });
        )
    )
}

bool Path::is_parent_of(const Path& x) const
{
    if( !this->m_class.is_Absolute() || !x.m_class.is_Absolute() )
        return false;
    const auto& te = this->m_class.as_Absolute();
    const auto& xe = x.m_class.as_Absolute();

    if( te.crate != xe.crate )
        return false;

    if( te.nodes.size() > xe.nodes.size() )
        return false;

    for(size_t i = 0; i < te.nodes.size(); i ++)
    {
        if( te.nodes[i].name() != xe.nodes[i].name() )
            return false;
    }

    return true;
}

void Path::bind_variable(unsigned int slot)
{
    m_bindings.value.set(AST::AbsolutePath(), PathBinding_Value::make_Variable({slot}));
}
#if 0
void Path::bind_enum_var(const Enum& ent, const RcString& name)
{
    auto it = ::std::find_if(ent.variants().begin(), ent.variants().end(), [&](const auto& x) { return x.m_name == name; });
    if( it == ent.variants().end() )
    {
        throw ParseError::Generic("Enum variant not found");
    }
    unsigned int idx = it - ent.variants().begin();

    DEBUG("Bound to enum variant '" << name << "' (#" << idx << ")");
    m_bindings.type = PathBinding_Type::make_EnumVar({ &ent, idx });
    m_bindings.value = PathBinding_Value::make_EnumVar({ &ent, idx });
}
#endif

Path& Path::operator+=(const Path& other)
{
    for(auto& node : other.nodes())
        append(node);
    // If the path is modified, clear the binding
    m_bindings = Bindings();
    return *this;
}

Ordering Path::ord(const Path& x) const
{
    Ordering rv;

    rv = ::ord( (unsigned)m_class.tag(), (unsigned)x.m_class.tag() );
    if( rv != OrdEqual )    return rv;

    TU_MATCH(Path::Class, (m_class, x.m_class), (ent, x_ent),
    (Invalid,
        return OrdEqual;
        ),
    (Local,
        return ::ord(ent.name, x_ent.name);
        ),
    (Relative,
        return ::ord(ent.nodes, x_ent.nodes);
        ),
    (Self,
        return ::ord(ent.nodes, x_ent.nodes);
        ),
    (Super,
        return ::ord(ent.nodes, x_ent.nodes);
        ),
    (Absolute,
        rv = ::ord( ent.crate, x_ent.crate );
        if( rv != OrdEqual )    return rv;
        return ::ord(ent.nodes, x_ent.nodes);
        ),
    (UFCS,
        rv = ent.type->ord( *x_ent.type );
        if( rv != OrdEqual )    return rv;
        rv = ent.trait->ord( *x_ent.trait );
        if( rv != OrdEqual )    return rv;
        return ::ord(ent.nodes, x_ent.nodes);
        )
    )

    return OrdEqual;
}

void Path::print_pretty(::std::ostream& os, bool is_type_context, bool is_debug) const
{
    TU_MATCH_HDRA( (m_class), {)
    TU_ARMA(Invalid, ent) {
        os << "/*inv*/";
        // NOTE: Don't print the binding for invalid paths
        return ;
        }
    TU_ARMA(Local, ent) {
        // Only print comment if there's no binding
        if( m_bindings.value.is_Unbound() && m_bindings.type.is_Unbound() )
        {
            if( is_debug )
                os << "/*var*/";
        }
        else
        {
            assert(
                m_bindings.value.binding.is_Variable() || m_bindings.value.binding.is_Generic()
                || m_bindings.type.binding.is_TypeParameter()
                );
        }
        os << ent.name;
        }
    TU_ARMA(Relative, ent) {
        if( is_debug )
            os << ent.hygiene;
        for(const auto& n : ent.nodes)
        {
            if( &n != &ent.nodes[0] ) {
                os << "::";
            }
            n.print_pretty(os, is_type_context);
        }
        }
    TU_ARMA(Self, ent) {
        os << "self";
        for(const auto& n : ent.nodes)
        {
            os << "::";
            n.print_pretty(os, is_type_context);
        }
        }
    TU_ARMA(Super, ent) {
        os << "super";
        for(const auto& n : ent.nodes)
        {
            os << "::";
            n.print_pretty(os, is_type_context);
        }
        }
    TU_ARMA(Absolute, ent) {
        if( ent.crate != "" )
            os << "::\"" << ent.crate << "\"";
        else
            os << "crate";
        for(const auto& n : ent.nodes)
        {
            os << "::";
            n.print_pretty(os, is_type_context);
        }
        }
    TU_ARMA(UFCS, ent) {
        //os << "/*ufcs*/";
        if( ent.trait ) {
            os << "<" << *ent.type << " as ";
            if( ent.trait->m_class.is_Invalid() ) {
                os << "_";
            }
            else {
                os << *ent.trait;
            }
            os << ">";
        }
        else {
            os << "<" << *ent.type << ">";
        }
        for(const auto& n : ent.nodes) {
            os << "::";
            n.print_pretty(os, is_type_context);
        }
        }
    }
    if( is_debug ) {
        os << "/*";
        bool printed = false;
        if( !m_bindings.value.is_Unbound() ) {
            if(printed) os << ",";
            os << "v:" << m_bindings.value;
            printed = true;
        }
        if( !m_bindings.type.is_Unbound() ) {
            if(printed) os << ",";
            os << "t:" << m_bindings.type;
            printed = true;
        }
        if( !m_bindings.macro.is_Unbound() ) {
            if(printed) os << ",";
            os << "m:" << m_bindings.macro;
            printed = true;
        }
        if( !printed ) {
            os << "?";
        }
        os << "*/";
    }
}

::std::ostream& operator<<(::std::ostream& os, const Path& path)
{
    path.print_pretty(os, false, true);
    return os;
}

}
