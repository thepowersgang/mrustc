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
    AST::Module*  m_module;
    AST::Path m_module_path;
    ::std::vector< LocalItem >  m_locals;
    // TODO: Maintain a stack of variable scopes
    ::std::vector< ::std::pair<unsigned int,AST::Module*> >   m_module_stack;
    
    friend class CResolvePaths_NodeVisitor;
    
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
    
    ::rust::option<const AST::Path&> lookup_local(LocalItem::Type type, const ::std::string& name) const;
    
    // TODO: Handle a block and obtain the local module (if any)
};

// Path resolution checking
void ResolvePaths(AST::Crate& crate);

class CResolvePaths_NodeVisitor:
    public AST::NodeVisitorDef
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
        DEBUG("ExprNode_CallPath - " << node);
        AST::NodeVisitorDef::visit(node);
        m_res.handle_path(node.m_path, CASTIterator::MODE_EXPR);
    }
    
    void visit(AST::ExprNode_Block& node) {
        // If there's an inner module on this node
        if( node.m_inner_mod.get() ) {
            // Add a reference to it to the parent node (add_anon_module will do dedup)
            AST::Module& parent_mod = *( (m_res.m_module_stack.size() > 0) ? m_res.m_module_stack.back().second : m_res.m_module );
            auto idx = parent_mod.add_anon_module( node.m_inner_mod.get() );
            // Increment the module path to include it? (No - Instead handle that when tracing the stack)
            // And add to the list of modules to use in lookup
            m_res.m_module_stack.push_back( ::std::make_pair(idx, node.m_inner_mod.get()) );
        }
        AST::NodeVisitorDef::visit(node);
        // Once done, pop the module
        if( node.m_inner_mod.get() )
            m_res.m_module_stack.pop_back();
    }
    
    void visit(AST::ExprNode_Match& node)
    {
        DEBUG("ExprNode_Match");
        AST::NodeVisitor::visit(node.m_val);
        
        for( auto& arm : node.m_arms )
        {
            m_res.start_scope();
            for( auto& pat : arm.m_patterns )
                m_res.handle_pattern(pat, TypeRef());
            AST::NodeVisitor::visit(arm.m_cond);
            AST::NodeVisitor::visit(arm.m_code);
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
// Returns the bound path for the local item
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

// Search relative to current module
// > Search local use definitions (function-level)
// - TODO: Local use statements (scoped)
// > Search module-level definitions
bool lookup_path_in_module(const AST::Crate& crate, const AST::Module& module, const AST::Path& mod_path, AST::Path& path)
{
    for( const auto& item_mod : module.submods() )
    {
        if( item_mod.first.name() == path[0].name() ) {
            // Check name down?
            // Add current module path
            path = mod_path + path;
            path.resolve( crate );
            return true;
        }
    }
    for( const auto& import : module.imports() )
    {
        const ::std::string& bind_name = import.name;
        const AST::Path& bind_path = import.data;
        if( bind_name == "" ) {
            throw ParseError::Todo("Handle lookup_path_in_module for wildcard imports");
        }
        else if( bind_name == path[0].name() ) {
            path = AST::Path::add_tailing(bind_path, path);
            path.resolve( crate );
            return true;
        }
    }

    // Types
    for( const auto& item : module.structs() )
    {
        if( item.name == path[0].name() ) {
            path = mod_path + path;
            path.resolve( crate );
            return true;
        }
    }
    for( const auto& item : module.enums() )
    {
        if( item.name == path[0].name() ) {
            path = mod_path + path;
            path.resolve( crate );
            return true;
        }
    }
    for( const auto& item : module.traits() )
    {
        if( item.name == path[0].name() ) {
            path = mod_path + path;
            path.resolve( crate );
            return true;
        }
    }
    for( const auto& item : module.type_aliases() )
    {
        if( item.name == path[0].name() ) {
            path = mod_path + path;
            path.resolve( crate );
            return true;
        }
    }

    // Values / Functions
    for( const auto& item_fcn : module.functions() )
    {
        if( item_fcn.name == path[0].name() ) {
            path = mod_path + path;
            path.resolve( crate );
            return true;
        }
    }
    for( const auto& item : module.statics() )
    {
        if( item.name == path[0].name() ) {
            path = mod_path + path;
            path.resolve( crate );
            return true;
        }
    }

    return false;
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
        INDENT();
        // Already absolute, our job is done
        // - However, if the path isn't bound, bind it
        if( !path.is_bound() ) {
            path.resolve(m_crate);
        }
        UNINDENT();
    }
    else if( path.is_relative() )
    {
        DEBUG("Relative, local");
        
        // If there's a single node, and we're in expresion mode, look for a variable
        // Otherwise, search for a type
        bool is_trivial_path = (path.size() == 1 && path[0].args().size() == 0);
        
        LocalItem::Type search_type = (is_trivial_path && mode == MODE_EXPR ? LocalItem::VAR : LocalItem::TYPE);
        auto local = lookup_local( search_type, path[0].name() );
        if( local.is_some() )
        {
            auto rpath = local.unwrap();
            DEBUG("Local hit: " << path[0].name() << " = " << rpath);
            
            switch(mode)
            {
            // Local variable?
            // - TODO: What would happen if MODE_EXPR but path isn't a single ident?
            case MODE_EXPR:
                if( is_trivial_path )
                {
                    DEBUG("Local variable " << path[0].name());
                    path = AST::Path(AST::Path::TagLocal(), path[0].name());
                    return ;
                }
                throw ParseError::Todo("TODO: MODE_EXPR, but not a single identifer, what do?");
            // Type parameter
            case MODE_TYPE:
                DEBUG("Local type " << path);
                // - Switch the path to be a "LOCAL"
                path.set_local();
                return ;
            }
        
            // TODO: What about sub-types and methods on type params?
            // - Invalid afaik, instead Trait::method() is used
        }
        
        if( m_module_stack.size() )
        {
            AST::Path   local_path = m_module_path;
            for(unsigned int i = 0; i < m_module_stack.size(); i ++)
                local_path.nodes().push_back( AST::PathNode( FMT("#"<<i), {} ) );
            
            for(unsigned int i = m_module_stack.size(); i--; )
            {
                if( lookup_path_in_module(m_crate, *m_module_stack[i].second, local_path, path) ) {
                    // Success!
                    return ;
                }
                local_path.nodes().pop_back();
            }
        }
        if( lookup_path_in_module(m_crate, *m_module, m_module_path, path) )
        {
            return;
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
    if( pat.data().tag() == AST::Pattern::Data::MaybeBind )
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
            pat.set_bind(name, false, false);
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
    TRACE_FUNCTION_F("modpath = " << modpath);
    ::std::vector<AST::Path>    new_imports;
    for( auto& imp : mod.imports() )
    {
        // TODO: Handle 'super' and 'self' imports
        // - Any other type of import will be absolute
        
        if( !imp.data.is_absolute() )
        {
            if( imp.data[0].name() == "super" ) {
                if( modpath.size() < 1 )
                    throw ParseError::Generic("Encountered 'super' at crate root");
                auto newpath = modpath;
                newpath.nodes().pop_back();
                newpath.add_tailing(imp.data);
                DEBUG("Absolutised path " << imp.data << " into " << newpath);
                imp.data = ::std::move(newpath);
            }
            else {
                auto newpath = modpath + imp.data;
                DEBUG("Absolutised path " << imp.data << " into " << newpath);
                imp.data = ::std::move(newpath);
            }
        }
        
        // Run resolution on import
        imp.data.resolve(crate);
        DEBUG("Resolved import : " << imp.data);
        
        // If wildcard, make sure it's sane
        if( imp.name == "" )
        {
            switch(imp.data.binding_type())
            {
            case AST::Path::UNBOUND:
                throw ParseError::BugCheck("path unbound after calling .resolve()");
            case AST::Path::MODULE:
                break;
            case AST::Path::ENUM:
                break;
            
            default:
                throw ParseError::Generic("Wildcard imports are only allowed on modules and enums");
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

void SetCrateName_Type(const AST::Crate& crate, ::std::string name, TypeRef& type)
{
    if( type.is_path() )
    {
        type.path().set_crate(name);
        type.path().resolve(crate);
    }
}

void SetCrateName_Mod(const AST::Crate& crate, ::std::string name, AST::Module& mod)
{
    for(auto& submod : mod.submods())
        SetCrateName_Mod(crate, name, submod.first);
    // Imports 'use' statements
    for(auto& imp : mod.imports())
    {
        imp.data.set_crate(name);
        imp.data.resolve(crate);
    }
    
    // TODO: All other types
    for(auto& fcn : mod.functions())
    {
        SetCrateName_Type(crate, name, fcn.data.rettype());
    }
}


// First pass of conversion
// - Tag paths of external crate items with crate name
// - Convert all paths into absolute paths (or local variable references)
void ResolvePaths(AST::Crate& crate)
{
    DEBUG(" >>>");
    // Pre-process external crates to tag all paths
    DEBUG(" --- Extern crates");
    INDENT();
    for(auto& ec : crate.extern_crates())
    {
        SetCrateName_Mod(crate, ec.first, ec.second.root_module());
    }
    UNINDENT();
    
    // Handle 'use' statements in an initial parss
    DEBUG(" --- Use Statements");
    INDENT();
    ResolvePaths_HandleModule_Use(crate, AST::Path(AST::Path::TagAbsolute()), crate.root_module());
    UNINDENT();
    
    // Then do path resolution on all other items
    CPathResolver	pr(crate);
    DEBUG(" ---");
    pr.handle_module(AST::Path(AST::Path::TagAbsolute()), crate.root_module());
    DEBUG(" <<<");
}
