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
    
    (TypeParameter, os << "TypeParameter(" << i.level << " # " << i.idx << ")"; ),
    (Variable, os << "Variable(" << i.slot << ")"; )
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
    m_class(),
    m_span(x.m_span)
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

/// Resolve a path into a canonical form, and bind it to the target value
void Path::resolve(const Crate& root_crate, bool expect_params)
{
    TRACE_FUNCTION_F("*this = "<< *this);
    if( m_binding.is_Unbound() )
    {
        if( m_class.is_Absolute() ) {
            resolve_absolute(root_crate, expect_params);
        }
        else if(m_class.is_UFCS()) {
            resolve_ufcs(root_crate, expect_params);
        }
        else
            throw ParseError::BugCheck("Calling Path::resolve on non-absolute path");
    }
}
void Path::resolve_absolute(const Crate& root_crate, bool expect_params)
{
    auto& nodes = m_class.as_Absolute().nodes;
    DEBUG("m_crate = '" << m_crate << "'");
    
    unsigned int slice_from = 0;    // Used when rewriting the path to be relative to its crate root
    
    ::std::vector<const Module*>    mod_stack;
    const Module* mod = &root_crate.get_root_module(m_crate);
    for(unsigned int i = 0; i < nodes.size(); i ++ )
    {
        mod_stack.push_back(mod);
        const bool is_last = (i+1 == nodes.size());
        const bool is_sec_last = (i+2 == nodes.size());
        const PathNode& node = nodes[i];
        DEBUG("[" << i << "/"<<nodes.size()<<"]: " << node);
        
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
            if( i > 0 && nodes[i-1].name()[0] == '#' && nodes[i-1].name().size() > 1 )
            {
                i --;
                mod_stack.pop_back();
                mod = mod_stack.back();
                mod_stack.pop_back();
                nodes.erase(nodes.begin()+i);
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
                check_param_counts(ta.params(), expect_params, nodes[i]);
                m_binding = PathBinding::make_TypeAlias( {&ta} );
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
                check_param_counts(fn.params(), expect_params, nodes[i]);
                m_binding = PathBinding::make_Function({&fn});
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
                check_param_counts(t.params(), expect_params, nodes[i]);
                m_binding = PathBinding::make_Trait({&t});
                goto ret;
            }
            else if( is_sec_last ) {
                check_param_counts(t.params(), expect_params, nodes[i]);
                // TODO: Also check params on item
                m_binding = PathBinding::make_TraitMethod( {&t, nodes[i+1].name()} );
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
                check_param_counts(str.params(), expect_params, nodes[i]);
                bind_struct(str, node.args());
                goto ret;
            }
            else if( is_sec_last ) {
                check_param_counts(str.params(), expect_params, nodes[i]);
                bind_struct_member(str, node.args(), nodes[i+1]);
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
                check_param_counts(enm.params(), expect_params, nodes[i]);
                bind_enum(enm, node.args());
                goto ret;
            }
            else if( is_sec_last ) {
                check_param_counts(enm.params(), expect_params, nodes[i]);
                bind_enum_var(enm, nodes[i+1].name(), node.args());
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
            AST::Path   newpath = imp.data;
            auto& newnodes = newpath.m_class.as_Absolute().nodes;
            DEBUG("Re-exported path " << imp.data);
            if( imp.name == "" )
            {
                // Replace nodes 0:i-1 with source path, then recurse
                for( unsigned int j = i; j < nodes.size(); j ++ )
                {
                    newnodes.push_back( nodes[j] );
                }
            }
            else
            {
                // replace nodes 0:i with the source path
                for( unsigned int j = i+1; j < nodes.size(); j ++ )
                {
                    newnodes.push_back( nodes[j] );
                }
            }
            
            DEBUG("- newpath = " << newpath);
            // TODO: This should check for recursion somehow
            newpath.resolve(root_crate, expect_params);
            
            *this = mv$(newpath);
            DEBUG("Alias resolved, *this = " << *this);
            return; }
        }
        
    }
    
    // We only reach here if the path points to a module
    m_binding = PathBinding::make_Module({mod});
ret:
    if( slice_from > 0 )
    {
        DEBUG("Removing " << slice_from << " nodes to rebase path to crate root");
        nodes.erase(nodes.begin(), nodes.begin()+slice_from);
    }
    return ;
}

void Path::resolve_ufcs(const Crate& root_crate, bool expect_params)
{
    auto& data = m_class.as_UFCS();
    auto& type = *data.type;
    auto& trait = *data.trait;
    
    // TODO: I can forsee <T>::Assoc::Item desugaring into < <T>::Assoc >::Item, but that will be messy to code
    assert(data.nodes.size());
    if(data.nodes.size() != 1) throw ParseError::Todo("Path::resolve_ufcs - Are multi-node UFCS paths valid?");
    auto& node = data.nodes.at(0);
    
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
            //if( !type.type_params_ptr() )
            //    throw CompileError::BugCheck( FMT("Path::resolve_ufcs - No bound params on arg") );
            
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
                    m_binding = PathBinding::make_Function( {&it.data} );
                    goto _impl_item_bound;
                }
            }
            throw ParseError::Generic( FMT("Path::resolve_ufcs - No item named '"<<node.name()<<"' in inherent"));
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
    if( !trait_path.m_binding.is_Trait() )
        ERROR(trait_path.span(), E0000, "Trait in UFCS path is not a trait");
    const auto& trait_def = *trait_path.m_binding.as_Trait().trait_;
    
    // Check that the requested item exists within the trait, and bind to that item
    for( const auto& fn : trait_def.functions() )
    {
        if( fn.name == node.name() ) {
            check_param_counts(fn.data.params(), true, node);
            m_binding = PathBinding::make_Function( {&fn.data} );
            goto _trait_item_bound;
        }
    }
    for( const auto& it : trait_def.types() )
    {
        if( it.name == node.name() ) {
            check_param_counts(it.data.params(), true, node);
            m_binding = PathBinding::make_TypeAlias( {&it.data} );
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

void Path::bind_variable(unsigned int slot)
{
    m_binding = PathBinding::make_Variable({slot});
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

void Path::print_pretty(::std::ostream& os) const
{
    TU_MATCH(Path::Class, (m_class), (ent),
    (Invalid, os << "/* inv */"; ),
    (Local, os << ent.name;),
    (Relative,
        for(const auto& n : ent.nodes)    os << "::" << n;
        ),
    (Self,
        os << "self";
        for(const auto& n : ent.nodes)    os << "::" << n;
        ),
    (Super,
        os << "super";
        for(const auto& n : ent.nodes)    os << "::" << n;
        ),
    (Absolute,
        if( m_crate != "" )
            os << "::\"" << m_crate << "\"";
        for(const auto& n : ent.nodes)
            os << "::" << n;
        ),
    (UFCS,
        throw ParseError::Todo("Path::print_pretty - UFCS");
        )
    )
}

::std::ostream& operator<<(::std::ostream& os, const Path& path)
{
    //if( path.m_nodes.size() == 0 && path.m_class == Path::RELATIVE )
    //{
    //    os << "/* null path */";
    //    return os;
    //}
    #if PRETTY_PATH_PRINT
    TU_MATCH(Path::Class, (path.m_class), (ent),
    (Invalid,
        os << "/*inv*/";
        ),
    (Local,
        os << "/*var*/" << ent.name;
        ),
    (Relative,
        for(const auto& n : ent.nodes)
        {
            #if PRETTY_PATH_PRINT
            if( &n != &ent.nodes[0] ) {
                os << "::";
            }
            #endif
            os << n;
        }
        ),
    (Self,
        os << "self";
        for(const auto& n : ent.nodes)
        {
            #if PRETTY_PATH_PRINT
            os << "::";
            #endif
            os << n;
        }
        ),
    (Super,
        os << "super";
        for(const auto& n : ent.nodes)
        {
            #if PRETTY_PATH_PRINT
            os << "::";
            #endif
            os << n;
        }
        ),
    (Absolute,
        if( path.m_crate != "" )
            os << "::\""<<path.m_crate<<"\"";
        for(const auto& n : ent.nodes)
        {
            #if PRETTY_PATH_PRINT
            os << "::";
            #endif
            os << n;
        }
        ),
    (UFCS,
        os << "/*ufcs*/<" << *ent.type << " as " << *ent.trait << ">";
        for(const auto& n : ent.nodes)
            os << "::" << n;
        )
    )
    os << "/*" << path.m_binding << " [" << path.span().filename << ":" << path.span().start_line << "]*/";
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
#define _D(VAR, ...)  case Class::VAR: { m_class = Class::make_null_##VAR(); auto& ent = m_class.as_##VAR(); (void)&ent; __VA_ARGS__ } break;
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
