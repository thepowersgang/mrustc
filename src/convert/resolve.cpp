/*
 * "mrustc" Rust->C converter
 * - By John Hodge (Mutabah / thePowersGang)
 *
 * convert/resolve.cpp
 * - Resolve names into absolute format
 */
#include "../common.hpp"
#include "../ast/ast.hpp"
#include "../parse/parseerror.hpp"

class CPathResolver
{
    struct LocalItem
    {
        enum Type {
            TYPE,
            VAR,
        }   type;
        ::std::string   name;
        AST::Path   path;
     
        LocalItem():
            type(VAR), name()
        {}
        LocalItem(Type t, ::std::string name, AST::Path path=AST::Path()):
            type(t),
            name( ::std::move(name) ),
            path( ::std::move(path) )
        {}
    };
    const AST::Crate&   m_crate;
    const AST::Module&  m_module;
    const AST::Path m_module_path;
    ::std::vector< LocalItem >  m_locals;
    // TODO: Maintain a stack of variable scopes
    
public:
    CPathResolver(const AST::Crate& crate, const AST::Module& mod, AST::Path module_path);

    enum ResolvePathMode {
        MODE_EXPR,  // Variables allowed
        MODE_TYPE,
        MODE_BIND,  // Failure is allowed
    };
    
    void resolve_path(AST::Path& path, ResolvePathMode mode) const;
    void resolve_type(TypeRef& type) const;
    
    void handle_function(AST::Function& fcn);
    void handle_pattern(AST::Pattern& pat);

    void push_scope() {
        DEBUG("");
        m_locals.push_back( LocalItem() );
    }
    void pop_scope() {
        DEBUG(m_locals.size() << " items");
        for( auto it = m_locals.end(); it-- != m_locals.begin(); )
        {
            if( it->name == "" ) {
                m_locals.erase(it, m_locals.end());
                return ;
            }
        }
        m_locals.clear();
    }
    void add_local_type(::std::string name) {
        m_locals.push_back( LocalItem(LocalItem::TYPE, ::std::move(name)) );
    }
    void add_local_var(::std::string name) {
        m_locals.push_back( LocalItem(LocalItem::VAR, ::std::move(name)) );
    }
    void add_local_use(::std::string name, AST::Path path) {
        throw ParseError::Todo("CPathResolver::add_local_use - Determine type of path (type or value)");
        m_locals.push_back( LocalItem(LocalItem::VAR, ::std::move(name), path) );
    }
    ::rust::option<const AST::Path&> lookup_local(LocalItem::Type type, const ::std::string& name) const {
        for( auto it = m_locals.end(); it -- != m_locals.begin(); )
        {
            if( it->type == type && it->name == name ) {
                return ::rust::option<const AST::Path&>(it->path);
            }
        }
        return ::rust::option<const AST::Path&>();
    }
};

// Path resolution checking
void ResolvePaths(AST::Crate& crate);

class CResolvePaths_NodeVisitor:
    public AST::NodeVisitor
{
    CPathResolver&    m_res;
public:
    CResolvePaths_NodeVisitor(CPathResolver& res):
        m_res(res)
    {
    }

    void visit(AST::ExprNode_Macro& node) {
        throw ParseError::Todo("Resolve-time expanding of macros");
        
        //MacroExpander expanded_macro = Macro_Invoke(node.m_name.c_str(), node.m_tokens);
        // TODO: Requires being able to replace the node with a completely different type of node
        //node.replace( Parse_Expr0(expanded_macro) );
    }

    void visit(AST::ExprNode_NamedValue& node) {
        DEBUG("ExprNode_NamedValue");
        m_res.resolve_path(node.m_path, CPathResolver::MODE_EXPR);
    }
    
    void visit(AST::ExprNode_Match& node)
    {
        DEBUG("ExprNode_Match");
        AST::NodeVisitor::visit(node.m_val);
        
        for( auto& arm : node.m_arms )
        {
            m_res.push_scope();
            m_res.handle_pattern(arm.first);
            AST::NodeVisitor::visit(arm.second);
            m_res.pop_scope();
        }
    }
    
    void visit(AST::ExprNode_LetBinding& node)
    {
        DEBUG("ExprNode_LetBinding");
        
        AST::NodeVisitor::visit(node.m_value);
        
        m_res.handle_pattern(node.m_pat);
    }
};

CPathResolver::CPathResolver(const AST::Crate& crate, const AST::Module& mod, AST::Path module_path):
    m_crate(crate),
    m_module(mod),
    m_module_path( ::std::move(module_path) )
{
}

void CPathResolver::resolve_path(AST::Path& path, ResolvePathMode mode) const
{
    DEBUG("path = " << path);
    
    // Handle generic components of the path
    for( auto& ent : path.nodes() )
    {
        for( auto& arg : ent.args() )
        {
            resolve_type(arg);
        }
    }
    
    // Convert to absolute
    if( path.is_absolute() )
    {
        DEBUG("Absolute - binding");
        // Already absolute, our job is done
        // - However, if the path isn't bound, bind it
        if( !path.is_bound() ) {
            path.resolve(m_crate);
        }
    }
    else if( path.is_relative() )
    {
        // If there's a single node, and we're in expresion mode, look for a variable
        // Otherwise, search for a type
        bool is_trivial_path = path.size() == 1 && path[0].args().size() == 0;
        LocalItem::Type search_type = (is_trivial_path && mode == MODE_EXPR ? LocalItem::VAR : LocalItem::TYPE);
        auto local = lookup_local( search_type, path[0].name() );
        if( local.is_some() )
        {
            auto rpath = local.unwrap();
            DEBUG("Local hit: " << path[0].name() << " = " << rpath);
            
            // Local import?
            if( rpath.size() > 0 )
            {
                path = AST::Path::add_tailing(rpath, path);
                path.resolve( m_crate );
                return ;
            }
            
            // Local variable?
            // - TODO: What would happen if MODE_EXPR but path isn't a single ident?
            if( mode == MODE_EXPR && is_trivial_path )
            {
                DEBUG("Local variable " << path[0].name());
                path = AST::Path(AST::Path::TagLocal(), path[0].name());
                return ;
            }
        
            // Type parameter, return verbatim?
            // - TODO: Are extra params/entries valid here?
            if( mode == MODE_TYPE )
            {
                DEBUG("Local type " << path[0].name());
                return ;
            }
        
            // TODO: What about sub-types and methods on type params?
            // - Invalid afaik, instead Trait::method() is used
        }
        
        // Search relative to current module
        // > Search local use definitions (function-level)
        // - TODO: Local use statements (scoped)
        // > Search module-level definitions
        for( const auto& item_mod : m_module.submods() )
        {
            if( item_mod.first.name() == path[0].name() ) {
                // Check name down?
                // Add current module path
                path = m_module_path + path;
                path.resolve( m_crate );
                return ;
            }
        }
        for( const auto& item_fcn : m_module.functions() )
        {
            if( item_fcn.name == path[0].name() ) {
                path = m_module_path + path;
                path.resolve( m_crate );
                return ;
            }
        }
        for( const auto& item : m_module.structs() )
        {
            if( item.name == path[0].name() ) {
                path = m_module_path + path;
                path.resolve( m_crate );
                return ;
            }
        }
        for( const auto& item : m_module.statics() )
        {
            if( item.name == path[0].name() ) {
                path = m_module_path + path;
                path.resolve( m_crate );
                return ;
            }
        }
        for( const auto& import : m_module.imports() )
        {
            const ::std::string& bind_name = import.name;
            const AST::Path& bind_path = import.data;
            if( bind_name == "" ) {
            }
            else if( bind_name == path[0].name() ) {
                path = AST::Path::add_tailing(bind_path, path);
                path.resolve( m_crate );
                return ;
            }
        }
        
        DEBUG("no matches found for path = " << path);
        assert( path.is_relative() );
        if( mode != MODE_BIND )
            throw ParseError::Generic("Name resolution failed");
    }
}

void CPathResolver::resolve_type(TypeRef& type) const
{
    // TODO: Convert type into absolute (and check bindings)
    DEBUG("type = " << type);
    if( type.is_path() )
    {
        resolve_path(type.path(), MODE_TYPE);
    }
    else
    {
        for(auto& subtype : type.sub_types())
            resolve_type(subtype);
    }
}

void CPathResolver::handle_pattern(AST::Pattern& pat)
{
    DEBUG("pat = " << pat);
    // Resolve names
    switch(pat.type())
    {
    case AST::Pattern::ANY:
        // Wildcard, nothing to do
        break;
    case AST::Pattern::MAYBE_BIND: {
        ::std::string   name = pat.binding();
        // Locate a _constant_ within the current namespace which matches this name
        // - Variables don't count
        AST::Path newpath;
        newpath.append( AST::PathNode(name, {}) );
        resolve_path(newpath, CPathResolver::MODE_BIND);
        if( newpath.is_relative() )
        {
            // It's a name binding (desugar to 'name @ _')
            pat = AST::Pattern();
            pat.set_bind(name);
        }
        else
        {
            // It's a constant (enum variant usually)
            pat = AST::Pattern(
                AST::Pattern::TagValue(),
                ::std::unique_ptr<AST::ExprNode>( new AST::ExprNode_NamedValue( ::std::move(newpath) ) )
                );
        }
        break; }
    case AST::Pattern::VALUE: {
        CResolvePaths_NodeVisitor   nv(*this);
        pat.node().visit(nv);
        break; }
    case AST::Pattern::TUPLE:
        // Tuple is handled by subpattern code
        break;
    case AST::Pattern::TUPLE_STRUCT:
        // Resolve the path!
        // - TODO: Restrict to types and enum variants
        resolve_path( pat.path(), CPathResolver::MODE_TYPE );
        break;
    }
    // Extract bindings and add to namespace
    if( pat.binding().size() > 0 )
        add_local_var(pat.binding());
    for( auto& subpat : pat.sub_patterns() )
        handle_pattern(subpat);
    
    
    //throw ParseError::Todo("CPathResolver::handle_pattern()");
}

/// Perform name resolution in a function
void CPathResolver::handle_function(AST::Function& fcn)
{
    CResolvePaths_NodeVisitor   node_visitor(*this);

    DEBUG("Return type");
    resolve_type(fcn.rettype());
    DEBUG("Args");
    for( auto& arg : fcn.args() )
        resolve_type(arg.second);

    DEBUG("Code");
    for( auto& arg : fcn.args() )
        add_local_var(arg.first);
    fcn.code().visit_nodes( node_visitor );
    pop_scope();
    if( m_locals.size() != 0 )
        throw ParseError::BugCheck("m_locals.size() != 0");
}

void ResolvePaths_HandleModule_Use(const AST::Crate& crate, const AST::Path& modpath, AST::Module& mod)
{
    DEBUG("modpath = " << modpath);
    ::std::vector<AST::Path>    new_imports;
    for( auto& imp : mod.imports() )
    {
        // TODO: Handle 'super' and 'self' imports
        // - Any other type of import will be absolute
        
        
        if( imp.name == "" )
        {
            DEBUG("Wildcard of " << imp.data);
            if( imp.is_pub ) {
                throw ParseError::Generic("Wildcard uses can't be public");
            }
            
            // Wildcard import
            AST::Path& basepath = imp.data;
            basepath.resolve(crate);
            DEBUG("basepath = " << basepath);
            switch(basepath.binding_type())
            {
            case AST::Path::UNBOUND:
                throw ParseError::BugCheck("path unbound after calling .resolve()");
            case AST::Path::MODULE:
                for( auto& submod : basepath.bound_module().submods() )
                {
                    if( submod.second == true )
                    {
                        new_imports.push_back( basepath + AST::PathNode(submod.first.name(), {}) );
                    }
                }
                for(const auto& imp : basepath.bound_module().imports() )
                {
                    if( imp.is_pub )
                    {
                        DEBUG("Re-export " << imp.data);
                        if(imp.name == "")
                            throw ParseError::Generic("Wilcard uses can't be public");
                        AST::Path   path = imp.data;
                        path.resolve(crate);
                        DEBUG("Re-export (resolved) " << path);
                        new_imports.push_back( ::std::move(path) );
                    }
                }
                //throw ParseError::Todo("ResolvePaths_HandleModule - wildcard use on module");
                break;
            case AST::Path::ENUM:
                throw ParseError::Todo("ResolvePaths_HandleModule_Use - ENUM");
            case AST::Path::ENUM_VAR:
                throw ParseError::Todo("ResolvePaths_HandleModule_Use - ENUM_VAR");
            case AST::Path::STRUCT:
                throw ParseError::Todo("ResolvePaths_HandleModule_Use - STRUCT");
            case AST::Path::STRUCT_METHOD:
                throw ParseError::Todo("ResolvePaths_HandleModule_Use - STRUCT_METHOD");
            case AST::Path::FUNCTION:
                throw ParseError::Todo("ResolvePaths_HandleModule_Use - FUNCTION");
            case AST::Path::STATIC:
                throw ParseError::Todo("ResolvePaths_HandleModule_Use - STATIC");
            }
        }
    }
    
    for( auto& new_imp : new_imports )
    {
        if( not new_imp.is_bound() ) {
            new_imp.resolve(crate);
        }
        mod.add_alias(false, new_imp, new_imp[new_imp.size()-1].name());
    }
    
    for( auto& submod : mod.submods() )
    {
        ResolvePaths_HandleModule_Use(crate, modpath + submod.first.name(), submod.first);
    }
}

void ResolvePaths_HandleModule(const AST::Crate& crate, const AST::Path& modpath, AST::Module& mod)
{
    CPathResolver	pr(crate, mod, modpath);
    for( auto& fcn : mod.functions() )
    {
        DEBUG("Handling function '" << fcn.name << "'");
        pr.handle_function(fcn.data);
    }
    
    for( auto& impl : mod.impls() )
    {
        DEBUG("Handling impl<" << impl.params() << "> " << impl.trait() << " for " << impl.type());
        
        // Params
        pr.push_scope();
        for( auto& param : impl.params() )
        {
            DEBUG("Param " << param);
            pr.add_local_type(param.name());
            for(auto& trait : param.get_bounds())
                pr.resolve_type(trait);
        }
        // Trait
        pr.resolve_type( impl.trait() );
        // Type
        pr.resolve_type( impl.type() );
        
        // TODO: Associated types
        
        // Functions
        for( auto& fcn : impl.functions() )
        {
            DEBUG("- Function '" << fcn.name << "'");
            pr.handle_function(fcn.data);
        }
        pr.pop_scope();
    }
    
    for( auto& submod : mod.submods() )
    {
        DEBUG("Handling submod '" << submod.first.name() << "'");
        ResolvePaths_HandleModule(crate, modpath + submod.first.name(), submod.first);
    }
}

void ResolvePaths(AST::Crate& crate)
{
    // Handle 'use' statements in an initial parss
    ResolvePaths_HandleModule_Use(crate, AST::Path(AST::Path::TagAbsolute()), crate.root_module());
    
    // Then do path resolution on all other items
    ResolvePaths_HandleModule(crate, AST::Path(AST::Path::TagAbsolute()), crate.root_module());
}
