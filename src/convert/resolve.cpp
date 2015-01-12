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
    const AST::Crate&   m_crate;
    const AST::Module&  m_module;
    const AST::Path m_module_path;
    ::std::vector< ::std::string >  m_locals;
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
        m_locals.push_back( ::std::string() );
    }
    void pop_scope() {
        for( auto it = m_locals.end(); --it != m_locals.begin(); ) {
            if( *it == "" ) {
                m_locals.erase(it, m_locals.end());
                return ;
            }
        }
        m_locals.clear();
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
        m_res.resolve_path(node.m_path, CPathResolver::MODE_EXPR);
    }
    
    void visit(AST::ExprNode_Match& node)
    {
        AST::NodeVisitor::visit(node.m_val);
        
        for( auto& arm : node.m_arms )
        {
            m_res.push_scope();
            m_res.handle_pattern(arm.first);
            AST::NodeVisitor::visit(arm.second);
            m_res.pop_scope();
        }
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
    if( !path.is_relative() )
    {
        DEBUG("Absolute - binding");
        // Already absolute, our job is done
        // - However, if the path isn't bound, bind it
        if( !path.is_bound() ) {
            path.resolve(m_crate);
        }
    } 
    else
    {
        if( mode == MODE_EXPR && path.size() == 1 && path[0].args().size() == 0 )
        {
            // One non-generic component, look in the current function for a variable
            const ::std::string& var = path[0].name();
            DEBUG("varname = " << var);
            auto varslot = m_locals.end();
            for(auto slot = m_locals.begin(); slot != m_locals.end(); ++slot)
            {
                if( *slot == var ) {
                    varslot = slot;
                }
            }
            
            if( varslot != m_locals.end() )
            {
                DEBUG("Located slot");
                path = AST::Path(AST::Path::TagLocal(), var);
                return ;
            }
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
    throw ParseError::Todo("CPathResolver::resolve_type()");
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
        m_locals.push_back( pat.binding() );
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
        m_locals.push_back(arg.first);
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
    for( auto& fcn : mod.functions() )
    {
        CPathResolver	pr(crate, mod, modpath);
        pr.handle_function(fcn.data);
    }
    
    for( auto& submod : mod.submods() )
    {
        ResolvePaths_HandleModule(crate, modpath + submod.first.name(), submod.first);
    }
}

void ResolvePaths(AST::Crate& crate)
{
    // Handle 'use' statements in an initial parss
    ResolvePaths_HandleModule_Use(crate, AST::Path(AST::Path::TagAbsolute()), crate.root_module());
    
    // Then do path resolution on all other item
    ResolvePaths_HandleModule(crate, AST::Path(AST::Path::TagAbsolute()), crate.root_module());
}
