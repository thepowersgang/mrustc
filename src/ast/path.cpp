/*
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
::std::ostream& operator<<(::std::ostream& os, const PathBinding& x) {
    TU_MATCH(PathBinding, (x), (i),
    (Unbound, os << "UNBOUND";   ),
    (Module,  os << "Module";    ),
    (Trait,     os << "Trait";   ),
    (Struct,    os << "Struct";  ),
    (Enum,      os << "Enum";    ),
    (Static,    os << "Static";  ),
    (Function,  os << "Function";),
    (EnumVar,  os << "EnumVar(" << i.idx << ")"; ),
    (TypeAlias, os << "TypeAlias";),
    (StructMethod, os << "StructMethod"; ),
    (TraitMethod,  os << "TraitMethod";  ),
    
    (TypeParameter, os << "TyParam(" << i.level << " # " << i.idx << ")"; ),
    (Variable, os << "Var(" << i.slot << ")"; )
    )
    return os;
}
PathBinding PathBinding::clone() const
{
    TU_MATCH(::AST::PathBinding, (*this), (e),
    (Unbound , return PathBinding::make_Unbound({}); ),
    (Module  , return PathBinding::make_Module(e);   ),
    (Trait   , return PathBinding::make_Trait(e);    ),
    (Struct  , return PathBinding::make_Struct(e);   ),
    (Enum    , return PathBinding::make_Enum(e);     ),
    (Static  , return PathBinding::make_Static(e);   ),
    (Function, return PathBinding::make_Function(e); ),
    (TypeAlias, return PathBinding::make_TypeAlias(e); ),
    (EnumVar , return PathBinding::make_EnumVar(e);  ),
    (StructMethod, return PathBinding::make_StructMethod(e); ),
    (TraitMethod, return PathBinding::make_TraitMethod(e); ),
    
    (TypeParameter, return PathBinding::make_TypeParameter(e); ),
    (Variable, return PathBinding::make_Variable(e); )
    )
    throw "BUG: Fell off the end of PathBinding::clone";
}

::std::ostream& operator<<(::std::ostream& os, const PathParams& x)
{
    bool needs_comma = false;
    os << "<";
    for(const auto& v : x.m_lifetimes) {
        if(needs_comma) os << ", ";
        needs_comma = true;
        os << "'" << v;
    }
    for(const auto& v : x.m_types) {
        if(needs_comma) os << ", ";
        needs_comma = true;
        os << v;
    }
    for(const auto& v : x.m_assoc) {
        if(needs_comma) os << ", ";
        needs_comma = true;
        os << v.first << "=" << v.second;
    }
    os << ">";
    return os;
}
Ordering PathParams::ord(const PathParams& x) const
{
    Ordering rv;
    rv = ::ord(m_lifetimes, x.m_lifetimes);
    if(rv != OrdEqual)  return rv;
    rv = ::ord(m_types, x.m_types);
    if(rv != OrdEqual)  return rv;
    rv = ::ord(m_assoc, x.m_assoc);
    if(rv != OrdEqual)  return rv;
    return rv;
}

// --- AST::PathNode
PathNode::PathNode(::std::string name, PathParams args):
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
SERIALISE_TYPE(PathNode::, "PathNode", {
    s << m_name;
    //s << m_params;
},{
    s.item(m_name);
    //s.item(m_params);
})

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
    //m_binding(x.m_binding)
{
    TU_MATCH(Class, (x.m_class), (ent),
    (Invalid, m_class = Class::make_Invalid({});),
    (Local,
        m_class = Class::make_Local({ent.name});
        ),
    (Relative,
        m_class = Class::make_Relative({ent.nodes});
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
            m_class = Class::make_UFCS({ box$(TypeRef(*ent.type)), ::std::unique_ptr<Path>(new Path(*ent.trait)), ent.nodes });
        else
            m_class = Class::make_UFCS({ box$(TypeRef(*ent.type)), nullptr, ent.nodes });
        )
    )
    
    memcpy(&m_binding, &x.m_binding, sizeof(PathBinding));
    //TU_MATCH(PathBinding, (x.m_binding), (ent),
    //(Unbound, m_binding = PathBinding::make_Unbound({}); ),
    //(Module,  os << "Module";    ),
    //(Trait,     os << "Trait";   ),
    //(Struct,    os << "Struct";  ),
    //(Enum,      os << "Enum";    ),
    //(Static,    os << "Static";  ),
    //(Function,  os << "Function";),
    //(EnumVar,  os << "EnumVar(" << i.idx << ")"; ),
    //(TypeAlias, os << "TypeAlias";),
    //(StructMethod, os << "StructMethod"; ),
    //(TraitMethod,  os << "TraitMethod";  ),
    //
    //(TypeParameter, os << "TypeParameter(" << i.level << " # " << i.idx << ")"; ),
    //(Variable, os << "Variable(" << i.slot << ")"; )
    //)
    
    //DEBUG("clone, x = " << x << ", this = " << *this );
}

/*
void Path::check_param_counts(const GenericParams& params, bool expect_params, PathNode& node)
{
    if( !expect_params )
    {
        if( node.args().size() )
            throw CompileError::BugCheck(FMT("Unexpected parameters in path " << *this));
    }
    else if( node.args().size() != params.ty_params().size() )
    {
        DEBUG("Count mismatch");
        if( node.args().size() > params.ty_params().size() )
        {
            // Too many, definitely an error
            throw CompileError::Generic(FMT("Too many type parameters passed in path " << *this));
        }
        else
        {
            // Too few, allow defaulting
            while( node.args().size() < params.ty_params().size() )
            {
                unsigned int i = node.args().size();
                const auto& p = params.ty_params()[i];
                DEBUG("Extra #" << i << ", p = " << p);
                // XXX: Currently, the default is just inserted (_ where not specified)
                // - Erroring failed on transmute, and other omitted for inferrence instnaces
                if( true || p.get_default() != TypeRef() )
                    node.args().push_back( p.get_default() );
                else
                    throw CompileError::Generic(FMT("Not enough type parameters passed in path " << *this));
            }
        }
    }
}
*/

void Path::bind_variable(unsigned int slot)
{
    m_binding = PathBinding::make_Variable({slot});
}
void Path::bind_module(const Module& mod) {
    m_binding = PathBinding::make_Module({&mod});
}
void Path::bind_enum(const Enum& ent, const ::std::vector<TypeRef>& /*args*/)
{
    DEBUG("Bound to enum");
    m_binding = PathBinding::make_Enum({&ent});
}
void Path::bind_enum_var(const Enum& ent, const ::std::string& name, const ::std::vector<TypeRef>& /*args*/)
{
    unsigned int idx = 0;
    for( idx = 0; idx < ent.variants().size(); idx ++ )
    {
        if( ent.variants()[idx].m_name == name ) {
            break;
        }
    }
    if( idx == ent.variants().size() )
        throw ParseError::Generic("Enum variant not found");
    
    //if( args.size() > 0 )
    //{
    //    if( args.size() != ent.params().size() )
    //        throw ParseError::Generic("Parameter count mismatch");
    //    throw ParseError::Todo("Bind enum variant with params passed");
    //}
    
    DEBUG("Bound to enum variant '" << name << "' (#" << idx << ")");
    m_binding = PathBinding::make_EnumVar({&ent, idx});
}
void Path::bind_struct(const Struct& ent, const ::std::vector<TypeRef>& /*args*/)
{
    //if( args.size() > 0 )
    //{
    //    if( args.size() != ent.params().n_params() )
    //        throw ParseError::Generic("Parameter count mismatch");
    //    // TODO: Is it the role of this section of code to ensure that the passed args are valid?
    //    // - Probably not, it should instead be the type checker that does it
    //    // - Count validation is OK here though
    //}
    
    DEBUG("Bound to struct");
    m_binding = PathBinding::make_Struct({&ent});
}
void Path::bind_struct_member(const Struct& ent, const ::std::vector<TypeRef>& /*args*/, const PathNode& member_node)
{
    DEBUG("Binding to struct item. This needs to be deferred");
    m_binding = PathBinding::make_StructMethod({&ent, member_node.name()});
}
void Path::bind_static(const Static& ent)
{
    m_binding = PathBinding::make_Static({&ent});
}
void Path::bind_trait(const Trait& ent, const ::std::vector<TypeRef>& /*args*/)
{
    m_binding = PathBinding::make_Trait({&ent});
}

Path& Path::operator+=(const Path& other)
{
    for(auto& node : other.nodes())
        append(node);
    // If the path is modified, clear the binding
    m_binding = PathBinding();
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

void Path::print_pretty(::std::ostream& os, bool is_type_context) const
{
    TU_MATCH(Path::Class, (m_class), (ent),
    (Invalid,
        os << "/*inv*/";
        // NOTE: Don't print the binding for invalid paths
        return ;
        ),
    (Local,
        // Only print comment if there's no binding
        if( m_binding.is_Unbound() )
            os << "/*var*/";
        else
            assert( m_binding.is_Variable() );
        os << ent.name;
        ),
    (Relative,
        for(const auto& n : ent.nodes)
        {
            if( &n != &ent.nodes[0] ) {
                os << "::";
            }
            n.print_pretty(os, is_type_context);
        }
        ),
    (Self,
        os << "self";
        for(const auto& n : ent.nodes)
        {
            os << "::";
            n.print_pretty(os, is_type_context);
        }
        ),
    (Super,
        os << "super";
        for(const auto& n : ent.nodes)
        {
            os << "::";
            n.print_pretty(os, is_type_context);
        }
        ),
    (Absolute,
        if( ent.crate != "" )
            os << "::\"" << ent.crate << "\"";
        for(const auto& n : ent.nodes)
        {
            os << "::";
            n.print_pretty(os, is_type_context);
        }
        ),
    (UFCS,
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
        )
    )
    os << "/*" << m_binding << "*/";
}

::std::ostream& operator<<(::std::ostream& os, const Path& path)
{
    path.print_pretty(os, false);
    return os;
}
void operator%(Serialiser& s, Path::Class::Tag c) {
    s << Path::Class::tag_to_str(c);
}
void operator%(::Deserialiser& s, Path::Class::Tag& c) {
    ::std::string   n;
    s.item(n);
    c = Path::Class::tag_from_str(n);
}
#define _D(VAR, ...)  case Class::TAG_##VAR: { m_class = Class::make_##VAR({}); auto& ent = m_class.as_##VAR(); (void)&ent; __VA_ARGS__ } break;
::std::unique_ptr<Path> Path::from_deserialiser(Deserialiser& s) {
    Path    p;
    s.item(p);
    return ::std::unique_ptr<Path>( new Path( mv$(p) ) );
}
SERIALISE_TYPE(Path::, "AST_Path", {
    s % m_class.tag();
    TU_MATCH(Path::Class, (m_class), (ent),
    (Invalid),
    (Local, s << ent.name; ),
    (Relative, s.item(ent.nodes); ),
    (Absolute, s.item(ent.nodes); ),
    (Self    , s.item(ent.nodes); ),
    (Super   , s.item(ent.nodes); ),
    (UFCS,
        s.item( ent.type );
        s.item( ent.trait );
        s.item( ent.nodes );
        )
    )
},{
    Class::Tag  tag;
    s % tag;
    switch(tag)
    {
    case Class::TAGDEAD: throw "";
    _D(Invalid)
    _D(Local   , s.item( ent.name ); )
    
    _D(Relative, s.item(ent.nodes); )
    _D(Absolute, s.item(ent.nodes); )
    _D(Self    , s.item(ent.nodes); )
    _D(Super   , s.item(ent.nodes); )
    _D(UFCS,
        s.item( ent.type );
        s.item( ent.trait );
        s.item( ent.nodes );
        )
    }
})
#undef _D

}
