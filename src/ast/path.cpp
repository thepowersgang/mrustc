/*
 */
#include "path.hpp"
#include "ast.hpp"
#include "../types.hpp"
#include <iostream>
#include "../parse/parseerror.hpp"
#include <algorithm>

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
SERIALISE_TYPE(PathNode::, "PathNode", {
    s << m_name;
    s << m_params;
})

// --- AST::Path
template<typename T>
typename ::std::vector<Item<T> >::const_iterator find_named(const ::std::vector<Item<T> >& vec, const ::std::string& name)
{
    return ::std::find_if(vec.begin(), vec.end(), [&name](const Item<T>& x) {
        return x.name == name;
    });
}

void Path::resolve(const Crate& root_crate)
{
    DEBUG("*this = " << *this);
    if(m_class != ABSOLUTE)
        throw ParseError::BugCheck("Calling Path::resolve on non-absolute path");
    DEBUG("m_crate = '" << m_crate << "'");
    
    const Module* mod = &root_crate.get_root_module(m_crate);
    for(unsigned int i = 0; i < m_nodes.size(); i ++ )
    {
        const bool is_last = (i+1 == m_nodes.size());
        const bool is_sec_last = (i+2 == m_nodes.size());
        const PathNode& node = m_nodes[i];
        DEBUG("mod = " << mod << ", node = " << node);
        
        // Sub-modules
        {
            auto& sms = mod->submods();
            auto it = ::std::find_if(sms.begin(), sms.end(), [&node](const ::std::pair<Module,bool>& x) {
                    return x.first.name() == node.name();
                });
            if( it != sms.end() )
            {
                DEBUG("Sub-module '" << node.name() << "'");
                if( node.args().size() )
                    throw ParseError::Generic("Generic params applied to module");
                mod = &it->first;
                continue;
            }
        }
        // External crates
        {
            auto& crates = mod->extern_crates();
            auto it = find_named(crates, node.name());
            if( it != crates.end() )
            {
                DEBUG("Extern crate '" << node.name() << "' = '" << it->data << "'");
                if( node.args().size() )
                    throw ParseError::Generic("Generic params applied to extern crate");
                mod = &root_crate.get_root_module(it->data);
                continue;
            }
        }
        
        // Start searching for:
        // - Re-exports
        {
            auto& imp = mod->imports();
            auto it = find_named(imp, node.name());
            if( it != imp.end() )
            {
                DEBUG("Re-exported path " << it->data);
                throw ParseError::Todo("Path::resolve() re-export");
            }
        }
        // Type Aliases
        {
            auto& items = mod->type_aliases();
            auto it = find_named(items, node.name());
            if( it != items.end() )
            {
                DEBUG("Type alias <"<<it->data.params()<<"> " << it->data.type());
                if( node.args().size() != it->data.params().size() )
                    throw ParseError::Generic("Param count mismatch when referencing type alias");
                // Make a copy of the path, replace params with it, then replace *this?
                // - Maybe leave that up to other code?
                throw ParseError::Todo("Path::resolve() type alias");
            }
        }

        // - Functions
        {
            auto& items = mod->functions();
            auto it = find_named(items, node.name());
            if( it != items.end() )
            {
                DEBUG("Found function");
                if( is_last ) {
                    throw ParseError::Todo("Path::resolve() bind to function");
                }
                else {
                    throw ParseError::Generic("Import of function, too many extra nodes");
                }
            }
        }
        // - Structs
        {
            auto& items = mod->structs();
            auto it = find_named(items, node.name());
            if( it != items.end() )
            {
                DEBUG("Found struct");
                if( is_last ) {
                    bind_struct(it->data, node.args());
                    return;
                }
                else if( is_sec_last ) {
                    throw ParseError::Todo("Path::resolve() struct method");
                }
                else {
                    throw ParseError::Generic("Import of struct, too many extra nodes");
                }
            }
        }
        // - Enums (and variants)
        {
            auto& enums = mod->enums();
            auto it = find_named(enums, node.name());
            if( it != enums.end() )
            {
                DEBUG("Found enum");
                if( is_last ) {
                    bind_enum(it->data, node.args());
                    return ;
                }
                else if( is_sec_last ) {
                    bind_enum_var(it->data, m_nodes[i+1].name(), node.args());
                    return ;
                }
                else {
                    throw ParseError::Generic("Binding path to enum, too many extra nodes");
                }
            }
        }
        // - Constants / statics
        {
            auto& items = mod->statics();
            auto it = find_named(items, node.name());
            if( it != items.end() )
            {
                DEBUG("Found static/const");
                if( is_last ) {
                    if( node.args().size() )
                        throw ParseError::Generic("Unexpected generic params on static/const");
                    bind_static(it->data);
                    return ;
                }
                else {
                    throw ParseError::Generic("Binding path to static, trailing nodes");
                }
            }
        }
        
        throw ParseError::Generic("Unable to find component '" + node.name() + "'");
    }
    
    // We only reach here if the path points to a module
    bind_module(*mod);
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
    if( args.size() > 0 )
    {
        if( args.size() != ent.params().size() )
            throw ParseError::Generic("Parameter count mismatch");
        throw ParseError::Todo("Bind enum with params passed");
    }
}
void Path::bind_enum_var(const Enum& ent, const ::std::string& name, const ::std::vector<TypeRef>& args)
{
    unsigned int idx = 0;
    for( idx = 0; idx < ent.variants().size(); idx ++ )
    {
        if( ent.variants()[idx].first == name ) {
            break;
        }
    }
    if( idx == ent.variants().size() )
        throw ParseError::Generic("Enum variant not found");
    
    if( args.size() > 0 )
    {
        if( args.size() != ent.params().size() )
            throw ParseError::Generic("Parameter count mismatch");
        throw ParseError::Todo("Bind enum variant with params passed");
    }
    
    DEBUG("Bound to enum variant '" << name << "' (#" << idx << ")");
    m_binding_type = ENUM_VAR;
    m_binding.enumvar = {&ent, idx};
}
void Path::bind_struct(const Struct& ent, const ::std::vector<TypeRef>& args)
{
    if( args.size() > 0 )
    {
        if( args.size() != ent.params().size() )
            throw ParseError::Generic("Parameter count mismatch");
        // TODO: Is it the role of this section of code to ensure that the passed args are valid?
        // - Probably not, it should instead be the type checker that does it
        // - Count validation is OK here though
    }
    
    DEBUG("Bound to struct");
    m_binding_type = STRUCT;
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
::std::ostream& operator<<(::std::ostream& os, const Path& path)
{
    switch(path.m_class)
    {
    case Path::RELATIVE:
        os << "Path({" << path.m_nodes << "})";
        break;
    case Path::ABSOLUTE:
        os << "Path(TagAbsolute, {" << path.m_nodes << "})";
        break;
    case Path::LOCAL:
        os << "Path(TagLocal, " << path.m_nodes[0].name() << ")";
        break;
    }
    return os;
}
::Serialiser& operator<<(Serialiser& s, Path::Class pc)
{
    switch(pc)
    {
    case Path::RELATIVE:  s << "RELATIVE";    break;
    case Path::ABSOLUTE:  s << "ABSOLUTE";    break;
    case Path::LOCAL: s << "LOCAL";   break;
    }
    return s;
}
SERIALISE_TYPE(Path::, "AST_Path", {
    s << m_class;
    s << m_nodes;
})

}
