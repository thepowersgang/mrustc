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
::std::ostream& operator<<(::std::ostream& os, const PathNode& pn) {
    os << pn.m_name;
    if( pn.m_params.size() )
    {
        os << "::<";
        os << pn.m_params;
        os << ">";
    }
    return os;
}
SERIALISE_TYPE(PathNode::, "PathNode", {
    s << m_name;
    s << m_params;
},{
    s.item(m_name);
    s.item(m_params);
})

// --- AST::Path
AST::Path::Path(TagUfcs, TypeRef type, TypeRef trait):
    m_class(UFCS),
    m_ufcs({::std::move(type), ::std::move(trait)} )
{
}

template<typename T>
typename ::std::vector<Item<T> >::const_iterator find_named(const ::std::vector<Item<T> >& vec, const ::std::string& name)
{
    return ::std::find_if(vec.begin(), vec.end(), [&name](const Item<T>& x) {
        return x.name == name;
    });
}

/// Resolve a path into a canonical form, and bind it to the target value
void Path::resolve(const Crate& root_crate, bool expect_params)
{
    TRACE_FUNCTION_F("*this = "<< *this);
    if(m_class == ABSOLUTE)
        resolve_absolute(root_crate, expect_params);
    else if(m_class == UFCS)
        resolve_ufcs(root_crate, expect_params);
    else
        throw ParseError::BugCheck("Calling Path::resolve on non-absolute path");
}
void Path::resolve_absolute(const Crate& root_crate, bool expect_params)
{
    DEBUG("m_crate = '" << m_crate << "'");
    
    unsigned int slice_from = 0;    // Used when rewriting the path to be relative to its crate root
    
    ::std::vector<const Module*>    mod_stack;
    const Module* mod = &root_crate.get_root_module(m_crate);
    for(unsigned int i = 0; i < m_nodes.size(); i ++ )
    {
        mod_stack.push_back(mod);
        const bool is_last = (i+1 == m_nodes.size());
        const bool is_sec_last = (i+2 == m_nodes.size());
        const PathNode& node = m_nodes[i];
        DEBUG("[" << i << "/"<<m_nodes.size()<<"]: " << node);
        
        if( node.name()[0] == '#' )
        {
            // HACK - Compiler-provided functions/types live in the special '#' module
            if( node.name() == "#" ) {
                if( i != 0 )
                    throw ParseError::BugCheck("# module not at path root");
                mod = &g_compiler_module;
                continue ;
            }
            
            // Hacky special case - Anon modules are indexed
            // - Darn you C++ and no string views
            unsigned int index = ::std::strtoul(node.name().c_str()+1, nullptr, 10);    // Parse the number at +1
            DEBUG(" index = " << index);
            if( index >= mod->anon_mods().size() )
                throw ParseError::Generic("Anon module index out of range");
            mod = mod->anon_mods().at(index);
            continue ;
        }
        
        auto item = mod->find_item(node.name(), is_last);  // Only allow leaf nodes (functions and statics) if this is the last node
        switch( item.type() )
        {
        // Not found
        case AST::Module::ItemRef::ITEM_none:
            // If parent node is anon, backtrack and try again
            // TODO: I feel like this shouldn't be done here, instead perform this when absolutising (now that find_item is reusable)
            if( i > 0 && m_nodes[i-1].name()[0] == '#' && m_nodes[i-1].name().size() > 1 )
            {
                i --;
                mod_stack.pop_back();
                mod = mod_stack.back();
                mod_stack.pop_back();
                m_nodes.erase(m_nodes.begin()+i);
                i --;
                DEBUG("Failed to locate item in nested, look upwards - " << *this);
                
                continue ;
            }
            throw ParseError::Generic("Unable to find component '" + node.name() + "'");
        
        // Sub-module
        case AST::Module::ItemRef::ITEM_Module:
            DEBUG("Sub-module : " << node.name());
            if( node.args().size() )
                throw ParseError::Generic("Generic params applied to module");
            mod = &item.unwrap_Module();
            break;
       
        // Crate 
        case AST::Module::ItemRef::ITEM_Crate: {
            const ::std::string& crate_name = item.unwrap_Crate();
            DEBUG("Extern crate '" << node.name() << "' = '" << crate_name << "'");
            if( node.args().size() )
                throw ParseError::Generic("Generic params applied to extern crate");
            m_crate = crate_name;
            slice_from = i+1;
            mod = &root_crate.get_root_module(crate_name);
            break; }
        
        // Type Alias
        case AST::Module::ItemRef::ITEM_TypeAlias: {
            const auto& ta = item.unwrap_TypeAlias();
            DEBUG("Type alias <"<<ta.params()<<"> " << ta.type());
            //if( node.args().size() != ta.params().size() )
            //    throw ParseError::Generic("Param count mismatch when referencing type alias");
            // Make a copy of the path, replace params with it, then replace *this?
            // - Maybe leave that up to other code?
            if( is_last ) {
                check_param_counts(ta.params(), expect_params, m_nodes[i]);
                m_binding = PathBinding(&ta);
                goto ret;
            }
            else {
                throw ParseError::Todo("Path::resolve() type method");
            }
            break; }
        
        // Function
        case AST::Module::ItemRef::ITEM_Function: {
            const auto& fn = item.unwrap_Function();
            DEBUG("Found function");
            if( is_last ) {
                check_param_counts(fn.params(), expect_params, m_nodes[i]);
                m_binding = PathBinding(&fn);
                goto ret;
            }
            else {
                throw ParseError::Generic("Import of function, too many extra nodes");
            }
            break; }
        
        // Trait
        case AST::Module::ItemRef::ITEM_Trait: {
            const auto& t = item.unwrap_Trait();
            DEBUG("Found trait");
            if( is_last ) {
                check_param_counts(t.params(), expect_params, m_nodes[i]);
                m_binding = PathBinding(&t);
                goto ret;
            }
            else if( is_sec_last ) {
                check_param_counts(t.params(), expect_params, m_nodes[i]);
                // TODO: Also check params on item
                m_binding = PathBinding(PathBinding::TagItem(), &t);
                goto ret;
            }
            else {
                throw ParseError::Generic("Import of trait, too many extra nodes");
            }
            break; }
        
        // Struct
        case AST::Module::ItemRef::ITEM_Struct: {
            const auto& str = item.unwrap_Struct();
            DEBUG("Found struct");
            if( is_last ) {
                check_param_counts(str.params(), expect_params, m_nodes[i]);
                bind_struct(str, node.args());
                goto ret;
            }
            else if( is_sec_last ) {
                check_param_counts(str.params(), expect_params, m_nodes[i]);
                bind_struct_member(str, node.args(), m_nodes[i+1]);
                goto ret;
            }
            else {
                throw ParseError::Generic("Import of struct, too many extra nodes");
            }
            break; }
        
        // Enum / enum variant
        case AST::Module::ItemRef::ITEM_Enum: {
            const auto& enm = item.unwrap_Enum();
            DEBUG("Found enum");
            if( is_last ) {
                check_param_counts(enm.params(), expect_params, m_nodes[i]);
                bind_enum(enm, node.args());
                goto ret;
            }
            else if( is_sec_last ) {
                check_param_counts(enm.params(), expect_params, m_nodes[i]);
                bind_enum_var(enm, m_nodes[i+1].name(), node.args());
                goto ret;
            }
            else {
                throw ParseError::Generic("Binding path to enum, too many extra nodes");
            }
            break; }
        
        case AST::Module::ItemRef::ITEM_Static: {
            const auto& st = item.unwrap_Static();
            DEBUG("Found static/const");
            if( is_last ) {
                if( node.args().size() )
                    throw ParseError::Generic("Unexpected generic params on static/const");
                bind_static(st);
                goto ret;
            }
            else {
                throw ParseError::Generic("Binding path to static, trailing nodes");
            }
            break; }
        
        // Re-export
        case AST::Module::ItemRef::ITEM_Use: {
            const auto& imp = item.unwrap_Use();
            if( imp.name == "" )
            {
                // Replace nodes 0:i-1 with source path, then recurse
                AST::Path   newpath = imp.data;
                for( unsigned int j = i; j < m_nodes.size(); j ++ )
                {
                    newpath.m_nodes.push_back( m_nodes[j] );
                }
                
                DEBUG("- newpath = " << newpath);
                // TODO: This should check for recursion somehow
                newpath.resolve(root_crate, expect_params);
                
                *this = newpath;
                DEBUG("Alias resolved, *this = " << *this);
                return;
            }
            else
            {
                // replace nodes 0:i with the source path
                DEBUG("Re-exported path " << imp.data);
                AST::Path   newpath = imp.data;
                for( unsigned int j = i+1; j < m_nodes.size(); j ++ )
                {
                    newpath.m_nodes.push_back( m_nodes[j] );
                }
                DEBUG("- newpath = " << newpath);
                // TODO: This should check for recursion somehow
                newpath.resolve(root_crate, expect_params);
                
                *this = newpath;
                DEBUG("Alias resolved, *this = " << *this);
                return;
            }
            break; }
        }
        
    }
    
    // We only reach here if the path points to a module
    m_binding = PathBinding(mod);
ret:
    if( slice_from > 0 )
    {
        DEBUG("Removing " << slice_from << " nodes to rebase path to crate root");
        m_nodes.erase(m_nodes.begin(), m_nodes.begin()+slice_from);
    }
    return ;
}

void Path::resolve_ufcs(const Crate& root_crate, bool expect_params)
{
    auto& type = m_ufcs.at(0);
    auto& trait = m_ufcs.at(1);
    
    // TODO: I can forsee <T>::Assoc::Item desugaring into < <T>::Assoc >::Item, but that will be messy to code
    assert(m_nodes.size());
    if(m_nodes.size() != 1) throw ParseError::Todo("Path::resolve_ufcs - Are multi-node UFCS paths valid?");
    auto& node = m_nodes.at(0);
    
    // If the type is unknown (at this time)
    if( type.is_wildcard() || type.is_type_param() )
    {
        // - _ as _ = BUG
        if( !trait.is_path() )
        {
            // Wait, what about <T as _>, is that valid?
            throw CompileError::BugCheck( FMT("Path::resolve_ufcs - Path invalid : " << *this) );
        }
        // - /*arg*/T as Trait = Type parameter
        else if( type.is_type_param() )
        {
            // Check that the param is bound on that trait?
            if( !type.type_params_ptr() )
                throw CompileError::BugCheck( FMT("Path::resolve_ufcs - No bound params on arg") );
            
            //const auto& tps = *type.type_params_ptr();
            //for( const auto& bound : tps.bounds() )
            //{
            //    // TODO: Check if this type impls the trait
            //    // - Not needed to do the bind, so ignore for now
            //}
            
            // Search trait for an impl
            //throw ParseError::Todo("Path::resolve_ufcs - Arg");
            resolve_ufcs_trait(trait.path(), node);
            //throw ParseError::Todo("Path::resolve_ufcs - Arg2");
        }
        // - _ as Trait = Inferred type (unknown at the moment)
        else
        {
            throw ParseError::Todo("Path::resolve_ufcs - Handle binding when type is unknown");
        }
    }
    else
    {
        // - Type as _ = ? Infer the trait from any matching impls
        if( trait.is_wildcard() )
        {
            // Search inherent impl first, then (somehow) search in-scope traits
            // - TODO: Shouldn't this be the job of CPathResolver?
            throw ParseError::Todo("Path::resolve_ufcs - Unknown trait (resolve)");
        }
        // - Type as Trait = Obtain from relevant impl
        else if( trait.is_path() )
        {
            // Locate in the trait, but store Self type somehow?
            trait.path().resolve(root_crate, true);
            resolve_ufcs_trait(trait.path(), node);
        }
        // - Type as ! = Item from the inherent impl (similar to above)
        else if( trait == TypeRef(TypeRef::TagInvalid()) )
        {
            // TODO: Handle case where 'type' is a trait object
            // 1. Obtain the impl
            AST::Impl* impl_ptr;
            if( ! root_crate.find_impl(AST::Path(), type, &impl_ptr) )
                throw ParseError::Generic("Path::resolve_ufcs - No impl block for type");
            assert( impl_ptr );
            
            for( const auto& it : impl_ptr->functions() )
            {
                if( it.name == node.name() ) {
                    check_param_counts(it.data.params(), expect_params, node);
                    m_binding = PathBinding(&it.data);
                    goto _impl_item_bound;
                }
            }
            throw ParseError::Generic("Path::resolve_ufcs - No item in inherent");
        _impl_item_bound:
            DEBUG("UFCS inherent bound to " << m_binding);
        }
        // - Type as * = Bug
        else
        {
            throw CompileError::BugCheck( FMT("Path::resolve_ufcs - Path invalid : " << *this) );
        }
    }
}

void Path::resolve_ufcs_trait(const AST::Path& trait_path, AST::PathNode& node)
{
    if(trait_path.m_binding.type() != PathBinding::TRAIT)
        throw ParseError::Generic("Path::resolve_ufcs - Trait in UFCS path is not a trait");
    const auto& trait_def = trait_path.m_binding.bound_trait();
    
    // Check that the requested item exists within the trait, and bind to that item
    for( const auto& fn : trait_def.functions() )
    {
        if( fn.name == node.name() ) {
            check_param_counts(fn.data.params(), true, node);
            m_binding = PathBinding(&fn.data);
            goto _trait_item_bound;
        }
    }
    for( const auto& it : trait_def.types() )
    {
        if( it.name == node.name() ) {
            check_param_counts(it.data.params(), true, node);
            m_binding = PathBinding(&it.data);
            goto _trait_item_bound;
        }
    }
    throw ParseError::Todo("Path::resolve_ufcs - Fully known");
_trait_item_bound:
    DEBUG("UFCS trait bound to " << m_binding);
}

void Path::check_param_counts(const TypeParams& params, bool expect_params, PathNode& node)
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
void Path::bind_enum(const Enum& ent, const ::std::vector<TypeRef>& args)
{
    DEBUG("Bound to enum");
    m_binding = PathBinding(&ent);
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
    m_binding = PathBinding(&ent, idx);
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
    m_binding = PathBinding(&ent);
}
void Path::bind_struct_member(const Struct& ent, const ::std::vector<TypeRef>& args, const PathNode& member_node)
{
    DEBUG("Binding to struct item. This needs to be deferred");
    m_binding = PathBinding(PathBinding::TagItem(), &ent);
}
void Path::bind_static(const Static& ent)
{
    m_binding = PathBinding(&ent);
}

void Path::resolve_args(::std::function<TypeRef(const char*)> fcn)
{
    TRACE_FUNCTION_F(*this);
    for(auto& n : nodes())
    {
        for(auto& p : n.args())
            p.resolve_args(fcn);
    }
    
    switch(m_class)
    {
    case Path::RELATIVE:
    case Path::ABSOLUTE:
        break;
    case Path::LOCAL:
        break;
    case Path::UFCS:
        m_ufcs[0].resolve_args(fcn);
        m_ufcs[1].resolve_args(fcn);
        break;
    }
}

Path& Path::operator+=(const Path& other)
{
    for(auto& node : other.m_nodes)
        append(node);
    // If the path is modified, clear the binding
    m_binding = PathBinding();
    return *this;
}

void Path::match_args(const Path& other, ::std::function<void(const char*,const TypeRef&)> fcn) const
{
    if( m_nodes.size() != other.m_nodes.size() )
        throw ::std::runtime_error("Type mismatch (path size)");
    for( unsigned int i = 0; i < m_nodes.size(); i++ )
    {
        auto& pn1 = m_nodes[i];
        auto& pn2 = other.m_nodes[i];
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

int Path::equal_no_generic(const Path& x) const
{
    if( m_class != x.m_class )
        return -1;
    if( m_crate != x.m_crate )
        return -1;
    
    bool conditional_match = false;
    unsigned int i = 0;
    for( const auto &e : m_nodes )
    {
        if( i >= x.m_nodes.size() )
            return -1;
        const auto& xe = x.m_nodes[i];
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
    
    rv = ::ord( (unsigned)m_class, (unsigned)x.m_class );
    if( rv != OrdEqual )    return rv;
    
    rv = ::ord( m_crate, x.m_crate );
    if( rv != OrdEqual )    return rv;
    
    rv = ::ord( m_nodes, x.m_nodes );
    if( rv != OrdEqual )    return rv;
    
    return OrdEqual;
}

void Path::print_pretty(::std::ostream& os) const
{
    switch(m_class)
    {
    case Path::RELATIVE:
        os << "self";
        for(const auto& n : m_nodes)
            os << n;
        break;
    case Path::ABSOLUTE:
        if( m_crate != "" )
            os << "::" << m_crate;
        for(const auto& n : m_nodes)
            os << n;
        break;
    case Path::LOCAL:
        os << m_nodes[0].name();
        break;
    case Path::UFCS:
        throw ParseError::Todo("Path::print_pretty");
    }
}

::std::ostream& operator<<(::std::ostream& os, const Path& path)
{
    if( path.m_nodes.size() == 0 && path.m_class == Path::RELATIVE )
    {
        os << "/* null path */";
        return os;
    }
    #if PRETTY_PATH_PRINT
    switch(path.m_class)
    {
    case Path::RELATIVE:
        os << "self";
        for(const auto& n : path.m_nodes)
        {
            #if PRETTY_PATH_PRINT
            os << "::";
            #endif
            os << n;
        }
        break;
    case Path::ABSOLUTE:
        if( path.m_crate != "" )
            os << "::\""<<path.m_crate<<"\"";
        for(const auto& n : path.m_nodes)
        {
            #if PRETTY_PATH_PRINT
            os << "::";
            #endif
            os << n;
        }
        os << "/*" << path.m_binding << "*/";
        break;
    case Path::LOCAL:
        os << path.m_nodes[0].name();
        break;
    case Path::UFCS:
        os << "/*ufcs*/<" << path.m_ufcs[0] << " as " << path.m_ufcs[1] << ">";
        for(const auto& n : path.m_nodes)
            os << "::" << n;
    }
    #else
    switch(path.m_class)
    {
    case Path::RELATIVE:
        os << "Path({" << path.m_nodes << "})";
        break;
    case Path::ABSOLUTE:
        os << "Path(TagAbsolute, \""<<path.m_crate<<"\", {" << path.m_nodes << "})";
        break;
    case Path::LOCAL:
        os << "Path(TagLocal, " << path.m_nodes[0].name() << ")";
        break;
    }
    #endif
    return os;
}
::Serialiser& operator<<(Serialiser& s, Path::Class pc)
{
    switch(pc)
    {
    case Path::RELATIVE:  s << "RELATIVE";    break;
    case Path::ABSOLUTE:  s << "ABSOLUTE";    break;
    case Path::LOCAL: s << "LOCAL"; break;
    case Path::UFCS:  s << "UFCS";  break;
    }
    return s;
}
void operator>>(Deserialiser& s, Path::Class& pc)
{
    ::std::string   n;
    s.item(n);
         if(n == "RELATIVE")    pc = Path::RELATIVE;
    else if(n == "ABSOLUTE")    pc = Path::ABSOLUTE;
    else if(n == "LOCAL")       pc = Path::LOCAL;
    else if(n == "UFCS")        pc = Path::UFCS;
    else    throw ::std::runtime_error("Unknown path class : " + n);
}
SERIALISE_TYPE(Path::, "AST_Path", {
    s << m_class;
    s << m_crate;
    s << m_nodes;
},{
    s >> m_class;
    s.item(m_crate);
    s.item(m_nodes);
})

}
