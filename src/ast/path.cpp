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
bool PathNode::operator==(const PathNode& x) const
{
    return m_name == x.m_name && m_params == x.m_params;
}
::std::ostream& operator<<(::std::ostream& os, const PathNode& pn) {
    os << pn.m_name;
    if( pn.m_params.size() )
    {
        os << "<";
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
void Path::resolve(const Crate& root_crate)
{
    TRACE_FUNCTION_F("*this = "<< *this);
    if(m_class != ABSOLUTE)
        throw ParseError::BugCheck("Calling Path::resolve on non-absolute path");
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
                m_binding_type = ALIAS;
                m_binding.alias_ = &ta;
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
                m_binding_type = FUNCTION;
                m_binding.func_ = &fn;
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
                m_binding_type = TRAIT;
                m_binding.trait_ = &t;
                goto ret;
            }
            else if( is_sec_last ) {
                m_binding_type = TRAIT_METHOD;
                m_binding.trait_ = &t;
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
                bind_struct(str, node.args());
                goto ret;
            }
            else if( is_sec_last ) {
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
                bind_enum(enm, node.args());
                goto ret;
            }
            else if( is_sec_last ) {
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
                newpath.resolve(root_crate);
                
                *this = newpath;
                DEBUG("Alias resolved, *this = " << *this);
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
                newpath.resolve(root_crate);
                
                *this = newpath;
                DEBUG("Alias resolved, *this = " << *this);
            }
            break; }
        }
        
    }
    
    // We only reach here if the path points to a module
    bind_module(*mod);
ret:
    if( slice_from > 0 )
    {
        DEBUG("Removing " << slice_from << " nodes to rebase path to crate root");
        m_nodes.erase(m_nodes.begin(), m_nodes.begin()+slice_from);
    }
    return ;
}
void Path::bind_module(const Module& mod)
{
    m_binding_type = MODULE;
    m_binding.module_ = &mod;
}
void Path::bind_enum(const Enum& ent, const ::std::vector<TypeRef>& args)
{
    DEBUG("Bound to enum");
    m_binding_type = ENUM;
    m_binding.enum_ = &ent;
    //if( args.size() > 0 )
    //{
    //    if( args.size() != ent.params().size() )
    //        throw ParseError::Generic("Parameter count mismatch");
    //    throw ParseError::Todo("Bind enum with params passed");
    //}
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
    m_binding_type = ENUM_VAR;
    m_binding.enumvar = {&ent, idx};
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
    m_binding_type = STRUCT;
    m_binding.struct_ = &ent;
}
void Path::bind_struct_member(const Struct& ent, const ::std::vector<TypeRef>& args, const PathNode& member_node)
{
    DEBUG("Binding to struct item. This needs to be deferred");
    m_binding_type = STRUCT_METHOD;
    m_binding.struct_ = &ent;
}
void Path::bind_static(const Static& ent)
{
    m_binding_type = STATIC;
    m_binding.static_ = &ent;
}


Path& Path::operator+=(const Path& other)
{
    for(auto& node : other.m_nodes)
        append(node);
    return *this;
}

bool Path::operator==(const Path& x) const
{
    return m_class == x.m_class && m_crate == x.m_crate && m_nodes == x.m_nodes;
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
    }
}

::std::ostream& operator<<(::std::ostream& os, const Path& path)
{
    if( path.m_nodes.size() == 0 )
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
        break;
    case Path::LOCAL:
        os << path.m_nodes[0].name();
        break;
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
