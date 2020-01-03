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

#define PRETTY_PATH_PRINT   1

namespace AST {

// --- AST::PathBinding
::std::ostream& operator<<(::std::ostream& os, const PathBinding_Type& x) {
    TU_MATCHA( (x), (i),
    (Unbound, os << "_";   ),
    (Crate ,  os << "Crate";    ),
    (Module,  os << "Module";    ),
    (Trait,     os << "Trait";   ),
    (Struct,    os << "Struct";  ),
    (Enum,      os << "Enum";    ),
    (Union,     os << "Union";   ),
    (EnumVar,   os << "EnumVar(" << i.idx << ")"; ),
    (TypeAlias, os << "TypeAlias";),
    (TypeParameter, os << "TyParam(" << i.level << " # " << i.idx << ")"; )
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
    for(const auto& v : x.m_lifetimes) {
        if(needs_comma) os << ", ";
        needs_comma = true;
        os << v;
    }
    for(const auto& v : x.m_types) {
        if(needs_comma) os << ", ";
        needs_comma = true;
        os << v;
    }
    for(const auto& v : x.m_assoc_equal) {
        if(needs_comma) os << ", ";
        needs_comma = true;
        os << v.first << "=" << v.second;
    }
    for(const auto& v : x.m_assoc_bound) {
        if(needs_comma) os << ", ";
        needs_comma = true;
        os << v.first << ": " << v.second;
    }
    os << ">";
    return os;
}
PathParams::PathParams(const PathParams& x):
    m_lifetimes( x.m_lifetimes )
{
    m_types.reserve( x.m_types.size() );
    for(const auto& t : x.m_types)
        m_types.push_back(t.clone());

    m_assoc_equal.reserve( x.m_assoc_equal.size() );
    for(const auto& t : x.m_assoc_equal)
        m_assoc_equal.push_back( ::std::make_pair(t.first, t.second.clone()) );

    m_assoc_bound.reserve( x.m_assoc_bound.size() );
    for(const auto& t : x.m_assoc_bound)
        m_assoc_bound.push_back( ::std::make_pair(t.first, AST::Path(t.second)) );
}
Ordering PathParams::ord(const PathParams& x) const
{
    Ordering rv;
    rv = ::ord(m_lifetimes, x.m_lifetimes);
    if(rv != OrdEqual)  return rv;
    rv = ::ord(m_types, x.m_types);
    if(rv != OrdEqual)  return rv;
    rv = ::ord(m_assoc_equal, x.m_assoc_equal);
    if(rv != OrdEqual)  return rv;
    rv = ::ord(m_assoc_bound, x.m_assoc_bound);
    if(rv != OrdEqual)  return rv;
    return rv;
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
AST::Path::Path(TagUfcs, TypeRef type, ::std::vector<AST::PathNode> nodes):
    m_class( AST::Path::Class::make_UFCS({box$(type), nullptr, nodes}) )
{
}
AST::Path::Path(TagUfcs, TypeRef type, Path trait, ::std::vector<AST::PathNode> nodes):
    m_class( AST::Path::Class::make_UFCS({box$(type), box$(trait), nodes}) )
{
}
AST::Path::Path(const Path& x):
    m_class()
    //,m_bindings(x.m_bindings)
{
    memcpy(&m_bindings, &x.m_bindings, sizeof(Bindings));

    TU_MATCH(Class, (x.m_class), (ent),
    (Invalid, m_class = Class::make_Invalid({});),
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
    m_bindings.value = PathBinding_Value::make_Variable({slot});
}
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
            assert( m_bindings.value.is_Variable() || m_bindings.value.is_Generic() || m_bindings.type.is_TypeParameter() );
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
