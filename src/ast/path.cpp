/*
 */
#include "path.hpp"
#include "ast.hpp"
#include "../types.hpp"
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

// --- AST::PathNode
PathNode::PathNode(::std::string name, ::std::vector<TypeRef> args):
    m_name(name),
    m_params(args)
{
}
const ::std::string& PathNode::name() const
{
    return m_name;
}
const ::std::vector<TypeRef>& PathNode::args() const
{
    return m_params;
}
Ordering PathNode::ord(const PathNode& x) const
{
    Ordering    rv;
    rv = ::ord(m_name, x.m_name);
    if(rv != OrdEqual)  return rv;
    rv = ::ord(m_params, x.m_params);
    if(rv != OrdEqual)  return rv;
    return OrdEqual;
}
void PathNode::print_pretty(::std::ostream& os, bool is_type_context) const
{
    os << m_name;
    if( m_params.size() )
    {
        if( ! is_type_context )
            os << "::";
        os << "<";
        os << m_params;
        os << ">";
    }
}
::std::ostream& operator<<(::std::ostream& os, const PathNode& pn) {
    pn.print_pretty(os, false);
    return os;
}
SERIALISE_TYPE(PathNode::, "PathNode", {
    s << m_name;
    s << m_params;
},{
    s.item(m_name);
    s.item(m_params);
})

/// Return an iterator to the named item
template<typename T>
typename ::std::vector<Item<T> >::const_iterator find_named(const ::std::vector<Item<T> >& vec, const ::std::string& name)
{
    return ::std::find_if(vec.begin(), vec.end(), [&name](const Item<T>& x) {
        return x.name == name;
    });
}

// --- AST::Path
AST::Path::Path(TagUfcs, TypeRef type, TypeRef trait, ::std::vector<AST::PathNode> nodes):
    m_class( AST::Path::Class::make_UFCS({box$(type), box$(trait), nodes}) )
{
}
AST::Path::Path(const Path& x):
    m_crate(x.m_crate),
    m_class()
    //m_binding(x.m_binding)
{
    TU_MATCH(Class, (x.m_class), (ent),
    (Invalid, m_class = Class::make_Invalid({});),
    (Local,
        m_class = Class::make_Local({name: ent.name});
        ),
    (Relative,
        m_class = Class::make_Relative({nodes: ent.nodes});
        ),
    (Self,
        m_class = Class::make_Self({nodes: ent.nodes});
        ),
    (Super,
        m_class = Class::make_Super({nodes: ent.nodes});
        ),
    (Absolute,
        m_class = Class::make_Absolute({nodes: ent.nodes});
        ),
    (UFCS,
        m_class = Class::make_UFCS({ box$(TypeRef(*ent.type)), box$(TypeRef(*ent.trait)), ent.nodes });
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
    
    DEBUG("clone, x = " << x << ", this = " << *this );
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
void Path::bind_enum(const Enum& ent, const ::std::vector<TypeRef>& args)
{
    DEBUG("Bound to enum");
    m_binding = PathBinding::make_Enum({&ent});
}
void Path::bind_enum_var(const Enum& ent, const ::std::string& name, const ::std::vector<TypeRef>& args)
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
void Path::bind_struct(const Struct& ent, const ::std::vector<TypeRef>& args)
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
void Path::bind_struct_member(const Struct& ent, const ::std::vector<TypeRef>& args, const PathNode& member_node)
{
    DEBUG("Binding to struct item. This needs to be deferred");
    m_binding = PathBinding::make_StructMethod({&ent, member_node.name()});
}
void Path::bind_static(const Static& ent)
{
    m_binding = PathBinding::make_Static({&ent});
}
void Path::bind_trait(const Trait& ent, const ::std::vector<TypeRef>& args)
{
    m_binding = PathBinding::make_Trait({&ent});
}

void Path::resolve_args(::std::function<TypeRef(const char*)> fcn)
{
    TRACE_FUNCTION_F(*this);
    
    TU_MATCH(Path::Class, (m_class), (ent),
    (Invalid),
    (Local,  ),
    
    (Relative, Path::resolve_args_nl(ent.nodes, fcn); ),
    (Absolute, Path::resolve_args_nl(ent.nodes, fcn); ),
    (Self    , Path::resolve_args_nl(ent.nodes, fcn); ),
    (Super   , Path::resolve_args_nl(ent.nodes, fcn); ),
    (UFCS,
        ent.type->resolve_args(fcn);
        ent.trait->resolve_args(fcn);
        Path::resolve_args_nl(ent.nodes, fcn);
        )
    )
}
void Path::resolve_args_nl(::std::vector<PathNode>& nodes, ::std::function<TypeRef(const char*)> fcn)
{
    for(auto& n : nodes)
    {
        for(auto& p : n.args())
            p.resolve_args(fcn);
    }
}

Path& Path::operator+=(const Path& other)
{
    for(auto& node : other.nodes())
        append(node);
    // If the path is modified, clear the binding
    m_binding = PathBinding();
    return *this;
}

/// Match two same-format (i.e. same components) paths together, calling TypeRef::match_args on arguments
void Path::match_args(const Path& other, ::std::function<void(const char*,const TypeRef&)> fcn) const
{
    // TODO: Ensure that the two paths are of a compatible class (same class?)
    // - This will crash atm if they aren't the same
    TU_MATCH(Path::Class, (m_class, other.m_class), (ent, x_ent),
    (Invalid),
    (Local,  ),
    
    (Relative, Path::match_args_nl(ent.nodes, x_ent.nodes, fcn); ),
    (Absolute, Path::match_args_nl(ent.nodes, x_ent.nodes, fcn); ),
    (Self    , Path::match_args_nl(ent.nodes, x_ent.nodes, fcn); ),
    (Super   , Path::match_args_nl(ent.nodes, x_ent.nodes, fcn); ),
    (UFCS,
        Path::match_args_nl(ent.nodes, x_ent.nodes, fcn);
        throw ::std::runtime_error("TODO: UFCS Path::match_args");
        )
    )
}

void Path::match_args_nl(const ::std::vector<PathNode>& nodes_a, const ::std::vector<PathNode>& nodes_b, ::std::function<void(const char*,const TypeRef&)> fcn)
{
    if( nodes_a.size() != nodes_b.size() )
        throw ::std::runtime_error("Type mismatch (path size)");
    for( unsigned int i = 0; i < nodes_a.size(); i++ )
    {
        auto& pn1 = nodes_a[i];
        auto& pn2 = nodes_b[i];
        if( pn1.name() != pn2.name() )
            throw ::std::runtime_error("Type mismatch (path component)");
        if( pn1.args().size() != pn2.args().size() )
            throw ::std::runtime_error("Type mismatch (path component param count)");
        
        for( unsigned int j = 0; j < pn1.args().size(); j ++ )
        {
            auto& t1 = pn1.args()[j];
            auto& t2 = pn2.args()[j];
            t1.match_args( t2, fcn );
        }
    }
}

bool Path::is_concrete() const
{
    for(const auto& n : this->nodes())
    {
        for(const auto& p : n.args())
            if( not p.is_concrete() )
                return false;
    }
    return true;
}

/// Compare if two paths refer to the same non-generic item
///
/// - This doesn't handle the (impossible?) case where a generic might
///   cause two different paths to look the same.
int Path::equal_no_generic(const Path& x) const
{
    if( m_class.tag() != x.m_class.tag() )
        return -1;
    if( m_crate != x.m_crate )
        return -1;
    
    TU_MATCH(Path::Class, (m_class, x.m_class), (ent, x_ent),
    (Invalid, return 0; ),
    (Local,    return (ent.name == x_ent.name ? 0 : 1); ),
    
    (Relative, return Path::node_lists_equal_no_generic(ent.nodes, x_ent.nodes); ),
    (Absolute, return Path::node_lists_equal_no_generic(ent.nodes, x_ent.nodes); ),
    (Self    , return Path::node_lists_equal_no_generic(ent.nodes, x_ent.nodes); ),
    (Super   , return Path::node_lists_equal_no_generic(ent.nodes, x_ent.nodes); ),
    (UFCS,
        throw ::std::runtime_error("TODO: UFCS Path::equal_no_generic");
        return Path::node_lists_equal_no_generic(ent.nodes, x_ent.nodes);
        )
    )
    throw ::std::runtime_error("Path::equal_no_generic - fell off");
}

int Path::node_lists_equal_no_generic(const ::std::vector<PathNode>& nodes_a, const ::std::vector<PathNode>& nodes_b)
{
    if( nodes_a.size() != nodes_b.size() ) {
        return -1;
    }
    
    bool conditional_match = false;
    unsigned int i = 0;
    for( const auto &e : nodes_a )
    {
        const auto& xe = nodes_b[i];
        if( e.name() != xe.name() )
            return -1;
        
        if( e.args().size() || xe.args().size() )
        {
            DEBUG("e = " << e << ", xe = " << xe);
            if( e.args().size() != xe.args().size() )
                throw CompileError::BugCheck("Generics should be resolved, and hence have the correct argument count");
            for( unsigned int j = 0; j < e.args().size(); j ++ )
            {
                int rv = e.args()[j].equal_no_generic( xe.args()[j] );
                if(rv < 0) return rv;
                if(rv > 0)  conditional_match = true;
            }
        }
        
        i ++;
    }
    
    return (conditional_match ? 1 : 0);
}

Ordering Path::ord(const Path& x) const
{
    Ordering rv;
    
    rv = ::ord( (unsigned)m_class.tag(), (unsigned)x.m_class.tag() );
    if( rv != OrdEqual )    return rv;
    
    rv = ::ord( m_crate, x.m_crate );
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
        if( m_crate != "" )
            os << "::\"" << m_crate << "\"";
        for(const auto& n : ent.nodes)
        {
            os << "::";
            n.print_pretty(os, is_type_context);
        }
        ),
    (UFCS,
        //os << "/*ufcs*/";
        if( ! ent.trait->m_data.is_None() ) {
            os << "<" << *ent.type << " as " << *ent.trait << ">";
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
    #if PRETTY_PATH_PRINT
    path.print_pretty(os, false);
    #else
    switch(path.m_class)
    {
    case Path::RELATIVE:
        os << "Path({" << path.m_nodes << "})";
        break;
    case Path::ABSOLUTE:
        os << "Path(TagAbsolute, \""<<path.m_crate<<"\", {" << path.m_nodes << "})";
        break;
    }
    #endif
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
#define _D(VAR, ...)  case Class::TAG_##VAR: { m_class = Class::make_null_##VAR(); auto& ent = m_class.as_##VAR(); (void)&ent; __VA_ARGS__ } break;
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
