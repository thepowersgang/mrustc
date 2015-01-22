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
#include "ast_iterate.hpp"

// ====================================================================
// -- Path resolver (converts paths to absolute form)
// ====================================================================
class CPathResolver:
    public CASTIterator
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
    const AST::Module*  m_module;
    AST::Path m_module_path;
    ::std::vector< LocalItem >  m_locals;
    // TODO: Maintain a stack of variable scopes
    
public:
    CPathResolver(const AST::Crate& crate);

    virtual void handle_path(AST::Path& path, CASTIterator::PathMode mode) override;
    virtual void handle_type(TypeRef& type) override;
    virtual void handle_expr(AST::ExprNode& node) override;
    
    virtual void handle_pattern(AST::Pattern& pat, const TypeRef& type_hint) override;
    virtual void handle_module(AST::Path path, AST::Module& mod) override;

    virtual void start_scope() override;
    virtual void local_type(::std::string name, TypeRef type) override {
        m_locals.push_back( LocalItem(LocalItem::TYPE, ::std::move(name)) );
    }
    virtual void local_variable(bool _is_mut, ::std::string name, const TypeRef& _type) override {
        m_locals.push_back( LocalItem(LocalItem::VAR, ::std::move(name)) );
    }
    virtual void end_scope() override;
    
    void add_local_use(::std::string name, AST::Path path) {
        throw ParseError::Todo("CPathResolver::add_local_use - Determine type of path (type or value)");
        m_locals.push_back( LocalItem(LocalItem::VAR, ::std::move(name), path) );
    }
    ::rust::option<const AST::Path&> lookup_local(LocalItem::Type type, const ::std::string& name) const;
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
        m_res.handle_path(node.m_path, CASTIterator::MODE_EXPR);
    }
    void visit(AST::ExprNode_CallPath& node) {
        DEBUG("ExprNode_CallPath");
        AST::NodeVisitor::visit(node);
        m_res.handle_path(node.m_path, CASTIterator::MODE_EXPR);
    }
    
    void visit(AST::ExprNode_Match& node)
    {
        DEBUG("ExprNode_Match");
        AST::NodeVisitor::visit(node.m_val);
        
        for( auto& arm : node.m_arms )
        {
            m_res.start_scope();
            m_res.handle_pattern(arm.first, TypeRef());
            AST::NodeVisitor::visit(arm.second);
            m_res.end_scope();
        }
    }
    
    void visit(AST::ExprNode_LetBinding& node)
    {
        DEBUG("ExprNode_LetBinding");
        
        AST::NodeVisitor::visit(node.m_value);
        
        m_res.handle_pattern(node.m_pat, TypeRef());
    }
};

CPathResolver::CPathResolver(const AST::Crate& crate):
    m_crate(crate),
    m_module(nullptr)
{
}
void CPathResolver::start_scope()
{
    DEBUG("");
    m_locals.push_back( LocalItem() );
}
void CPathResolver::end_scope()
{
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
::rust::option<const AST::Path&> CPathResolver::lookup_local(LocalItem::Type type, const ::std::string& name) const
{
    for( auto it = m_locals.end(); it -- != m_locals.begin(); )
    {
        if( it->type == type && it->name == name ) {
            return ::rust::option<const AST::Path&>(it->path);
        }
    }
    return ::rust::option<const AST::Path&>();
}

void CPathResolver::handle_path(AST::Path& path, CASTIterator::PathMode mode)
{
    DEBUG("path = " << path);
    
    // Handle generic components of the path
    for( auto& ent : path.nodes() )
    {
        for( auto& arg : ent.args() )
        {
            handle_type(arg);
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
        DEBUG("Relative, local");
        
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
                throw ParseError::BugCheck("Local types should be handled in handle_type");
                return ;
            }
        
            // TODO: What about sub-types and methods on type params?
            // - Invalid afaik, instead Trait::method() is used
        }
        
        // Search relative to current module
        // > Search local use definitions (function-level)
        // - TODO: Local use statements (scoped)
        // > Search module-level definitions
        for( const auto& item_mod : m_module->submods() )
        {
            if( item_mod.first.name() == path[0].name() ) {
                // Check name down?
                // Add current module path
                path = m_module_path + path;
                path.resolve( m_crate );
                return ;
            }
        }
        for( const auto& import : m_module->imports() )
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
    
        // Types
        for( const auto& item : m_module->structs() )
        {
            if( item.name == path[0].name() ) {
                path = m_module_path + path;
                path.resolve( m_crate );
                return ;
            }
        }
        for( const auto& item : m_module->enums() )
        {
            if( item.name == path[0].name() ) {
                path = m_module_path + path;
                path.resolve( m_crate );
                return ;
            }
        }
        for( const auto& item : m_module->traits() )
        {
            if( item.name == path[0].name() ) {
                path = m_module_path + path;
                path.resolve( m_crate );
                return ;
            }
        }
        for( const auto& item : m_module->type_aliases() )
        {
            if( item.name == path[0].name() ) {
                path = m_module_path + path;
                path.resolve( m_crate );
                return ;
            }
        }
        
        // Values / Functions
        for( const auto& item_fcn : m_module->functions() )
        {
            if( item_fcn.name == path[0].name() ) {
                path = m_module_path + path;
                path.resolve( m_crate );
                return ;
            }
        }
        for( const auto& item : m_module->statics() )
        {
            if( item.name == path[0].name() ) {
                path = m_module_path + path;
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
void CPathResolver::handle_type(TypeRef& type)
{
    if( type.is_path() && type.path().is_relative() && type.path().size() == 1 )
    {
        const auto& name = type.path()[0].name();
         
        if( name == "Self" )
        {
            // TODO: Handle "Self" correctly
            // THIS IS WRONG! (well, I think)
            type = TypeRef(TypeRef::TagArg(), "Self");
        }
        else if( lookup_local(LocalItem::TYPE, name).is_some() )
        {
            type = TypeRef(TypeRef::TagArg(), name);
        }
        else
        {
            // Not a type param, fall back to other checks
        }
    }
    CASTIterator::handle_type(type);
}
void CPathResolver::handle_expr(AST::ExprNode& node)
{
    CResolvePaths_NodeVisitor   nv(*this);
    node.visit(nv);
}

void CPathResolver::handle_pattern(AST::Pattern& pat, const TypeRef& type_hint)
{
    DEBUG("pat = " << pat);
    // Resolve "Maybe Bind" entries
    if( pat.type() == AST::Pattern::MAYBE_BIND )
    {
        ::std::string   name = pat.binding();
        // Locate a _constant_ within the current namespace which matches this name
        // - Variables don't count
        AST::Path newpath;
        newpath.append( AST::PathNode(name, {}) );
        handle_path(newpath, CASTIterator::MODE_BIND);
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
    }
    
    // hand off to original code
    CASTIterator::handle_pattern(pat, type_hint);
}
void CPathResolver::handle_module(AST::Path path, AST::Module& mod)
{
    // NOTE: Assigning here is safe, as the CASTIterator handle_module iterates submodules as the last action
    m_module = &mod;
    m_module_path = path;
    CASTIterator::handle_module(path, mod);
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
            case AST::Path::ALIAS:
                throw ParseError::Todo("ResolvePaths_HandleModule_Use - ALIAS");
            case AST::Path::ENUM:
                throw ParseError::Todo("ResolvePaths_HandleModule_Use - ENUM");
            case AST::Path::ENUM_VAR:
                throw ParseError::Todo("ResolvePaths_HandleModule_Use - ENUM_VAR");
            case AST::Path::STRUCT:
                throw ParseError::Todo("ResolvePaths_HandleModule_Use - STRUCT");
            case AST::Path::STRUCT_METHOD:
                throw ParseError::Todo("ResolvePaths_HandleModule_Use - STRUCT_METHOD");
            case AST::Path::TRAIT:
                throw ParseError::Todo("ResolvePaths_HandleModule_Use - TRAIT");
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

void SetCrateName_Mod(::std::string name, AST::Module& mod)
{
    for(auto& submod : mod.submods())
        SetCrateName_Mod(name, submod.first);
    // Imports 'use' statements
    for(auto& imp : mod.imports())
        imp.data.set_crate(name);
    
    // TODO: All other types
}


// First pass of conversion
// - Tag paths of external crate items with crate name
// - Convert all paths into absolute paths (or local variable references)
void ResolvePaths(AST::Crate& crate)
{
    DEBUG(" >>>");
    // Pre-process external crates to tag all paths
    for(auto& ec : crate.extern_crates())
    {
        SetCrateName_Mod(ec.first, ec.second.root_module());
    }
    
    // Handle 'use' statements in an initial parss
    ResolvePaths_HandleModule_Use(crate, AST::Path(AST::Path::TagAbsolute()), crate.root_module());
    DEBUG(" ---");
    
    // Then do path resolution on all other items
    CPathResolver	pr(crate);
    pr.handle_module(AST::Path(AST::Path::TagAbsolute()), crate.root_module());
    DEBUG(" <<<");
}
