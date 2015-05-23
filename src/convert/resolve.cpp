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
        TypeRef tr;
     
        LocalItem():
            type(VAR), name()
        {}
        LocalItem(Type t, ::std::string name, TypeRef tr=TypeRef()):
            type(t),
            name( ::std::move(name) ),
            tr( ::std::move(tr) )
        {}
        
        friend ::std::ostream& operator<<(::std::ostream& os, const LocalItem& x) {
            if( x.name == "" )
                return os << "#";
            else if( x.type == TYPE )
                return os << "type '" << x.name << "' = " << x.tr;
            else
                return os << "var '" << x.name << "'";
        }
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

    void handle_params(AST::TypeParams& params) override;

    virtual void handle_path(AST::Path& path, CASTIterator::PathMode mode) override;
    void handle_path_ufcs(AST::Path& path, CASTIterator::PathMode mode);
    virtual void handle_type(TypeRef& type) override;
    virtual void handle_expr(AST::ExprNode& node) override;
    
    virtual void handle_pattern(AST::Pattern& pat, const TypeRef& type_hint) override;
    virtual void handle_module(AST::Path path, AST::Module& mod) override;

    virtual void start_scope() override;
    virtual void local_type(::std::string name, TypeRef type) override {
        DEBUG("(name = " << name << ", type = " << type << ")");
        if( lookup_local(LocalItem::TYPE, name).is_some() ) {
            // Shadowing the type... check for recursion by doing a resolve check?
            type.resolve_args([&](const char *an){ if(an == name) return TypeRef(name+" "); else return TypeRef(an); });
        }
        m_locals.push_back( LocalItem(LocalItem::TYPE, ::std::move(name), ::std::move(type)) );
    }
    virtual void local_variable(bool _is_mut, ::std::string name, const TypeRef& _type) override {
        m_locals.push_back( LocalItem(LocalItem::VAR, ::std::move(name)) );
    }
    virtual void end_scope() override;
    
    ::rust::option<const LocalItem&> lookup_local(LocalItem::Type type, const ::std::string& name) const;
    
    // TODO: Handle a block and obtain the local module (if any)
private:
    void handle_path_int(AST::Path& path, CASTIterator::PathMode mode);
};

// Path resolution checking
void ResolvePaths(AST::Crate& crate);
void ResolvePaths_HandleModule_Use(const AST::Crate& crate, const AST::Path& modpath, AST::Module& mod);

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
        DEBUG(node.get_pos() << " ExprNode_CallPath - " << node);
        AST::NodeVisitorDef::visit(node);
        m_res.handle_path(node.m_path, CASTIterator::MODE_EXPR);
    }
    
    void visit(AST::ExprNode_Block& node) {
        // If there's an inner module on this node
        if( node.m_inner_mod.get() )
        {
            
            // Add a reference to it to the parent node (add_anon_module will do dedup)
            AST::Module& parent_mod = *( (m_res.m_module_stack.size() > 0) ? m_res.m_module_stack.back().second : m_res.m_module );
            auto idx = parent_mod.add_anon_module( node.m_inner_mod.get() );
            
            // And add to the list of modules to use in lookup
            m_res.m_module_stack.push_back( ::std::make_pair(idx, node.m_inner_mod.get()) );
            
            // Do use resolution on this module, then do 
            AST::Path   local_path = m_res.m_module_path;
            for(unsigned int i = 0; i < m_res.m_module_stack.size(); i ++)
                local_path.nodes().push_back( AST::PathNode( FMT("#" << m_res.m_module_stack[i].first), {} ) );
            
            ResolvePaths_HandleModule_Use(m_res.m_crate, local_path, *node.m_inner_mod);
        }
        AST::NodeVisitorDef::visit(node);
        // Once done, pop the module
        if( node.m_inner_mod.get() )
            m_res.m_module_stack.pop_back();
    }
    
    void visit(AST::ExprNode_IfLet& node)
    {
        DEBUG("ExprNode_IfLet");
        AST::NodeVisitor::visit(node.m_value);
        
        m_res.start_scope();
        m_res.handle_pattern(node.m_pattern, TypeRef());
        AST::NodeVisitor::visit(node.m_true);
        m_res.end_scope();
        
        AST::NodeVisitor::visit(node.m_false);
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
    
    void visit(AST::ExprNode_Loop& node)
    {
        switch( node.m_type )
        {
        case AST::ExprNode_Loop::FOR:
            AST::NodeVisitor::visit(node.m_cond);
            m_res.start_scope();
            m_res.handle_pattern(node.m_pattern, TypeRef());
            AST::NodeVisitor::visit(node.m_code);
            m_res.end_scope();
            break;
        case AST::ExprNode_Loop::WHILELET:
            AST::NodeVisitor::visit(node.m_cond);
            m_res.start_scope();
            m_res.handle_pattern(node.m_pattern, TypeRef());
            AST::NodeVisitor::visit(node.m_code);
            m_res.end_scope();
            break;
        default:
            AST::NodeVisitorDef::visit(node);
            break;
        }
    }
    
    void visit(AST::ExprNode_LetBinding& node)
    {
        DEBUG("ExprNode_LetBinding");
        
        AST::NodeVisitor::visit(node.m_value);
        m_res.handle_type(node.m_type);
        m_res.handle_pattern(node.m_pat, TypeRef());
    }
    
    void visit(AST::ExprNode_StructLiteral& node) override
    {
        DEBUG("ExprNode_StructLiteral");
        
        m_res.handle_path(node.m_path, CASTIterator::MODE_EXPR);
        AST::NodeVisitorDef::visit(node);
    }
    
    void visit(AST::ExprNode_Closure& node) override
    {
        DEBUG("ExprNode_Closure");
        m_res.start_scope();
        for( auto& param : node.m_args )
        {
            m_res.handle_type(param.second);
            m_res.handle_pattern(param.first, param.second);
        }
        m_res.handle_type(node.m_return);
        AST::NodeVisitor::visit(node.m_code);
        m_res.end_scope();
    }
    
    void visit(AST::ExprNode_Cast& node) override
    {
        DEBUG("ExprNode_Cast");
        m_res.handle_type(node.m_type);
        AST::NodeVisitorDef::visit(node);
    }
    
    void visit(AST::ExprNode_CallMethod& node) override
    {
        DEBUG("ExprNode_CallMethod");
        for( auto& arg : node.m_method.args() )
            m_res.handle_type(arg);
        AST::NodeVisitorDef::visit(node);
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
::rust::option<const CPathResolver::LocalItem&> CPathResolver::lookup_local(LocalItem::Type type, const ::std::string& src_name) const
{
    DEBUG("m_locals = [" << m_locals << "]");
    ::std::string   name = src_name;
    unsigned int    count = 0;
    while( name.size() > 0 && name.back() == ' ') {
        name.pop_back();
        count ++;
    }
    for( auto it = m_locals.end(); it -- != m_locals.begin(); )
    {
        if( it->type == type )
        {
            if( it->name == name && count-- == 0 )
                return ::rust::option<const LocalItem&>(*it);
        }
    }
    return ::rust::option<const LocalItem&>();
}

// Search relative to current module
// > Search local use definitions (function-level)
// - TODO: Local use statements (scoped)
// > Search module-level definitions
bool lookup_path_in_module(const AST::Crate& crate, const AST::Module& module, const AST::Path& mod_path, AST::Path& path)
{
    // - Allow leaf nodes if path is a single node, don't skip private wildcard imports
    auto item = module.find_item(path[0].name(), (path.size() == 1), false);
    switch(item.type())
    {
    case AST::Module::ItemRef::ITEM_none:
        return false;
    case AST::Module::ItemRef::ITEM_Use: {
        const auto& imp = item.unwrap_Use();
        if( imp.name == "" )
        {
            DEBUG("Wildcard import found, " << imp.data << " + " << path);
            // Wildcard path, prefix entirely with the path
            path = imp.data + path;
            path.resolve( crate );
            return true;
        }
        else
        {
            DEBUG("Named import found, " << imp.data << " + " << path << " [1..]");
            path = AST::Path::add_tailing(imp.data, path);
            path.resolve( crate );
            return true;
        }
        break; }
    case AST::Module::ItemRef::ITEM_Module:
        // Check name down?
        // Add current module path
        path = mod_path + path;
        path.resolve( crate );
        return true;
    default:
        path = mod_path + path;
        path.resolve( crate );
        return true;
    }
    #if 0
    for( const auto& import : module.imports() )
    {
        const ::std::string& bind_name = import.name;
        const AST::Path& bind_path = import.data;
        if( bind_name == "" )
        {
            // Check the bind type of this path
            switch(bind_path.binding_type())
            {
            case AST::Path::UNBOUND:
                throw ParseError::BugCheck("Wildcard import path not bound");
            // - If it's a module, recurse
            case AST::Path::MODULE:
                throw ParseError::Todo("Handle lookup_path_in_module for wildcard imports - module");
                break;
            // - If it's an enum, search for this name and then pass to resolve
            case AST::Path::ENUM:
                throw ParseError::Todo("Handle lookup_path_in_module for wildcard imports - enum");
                break;
            // - otherwise, error
            default:
                DEBUG("ERROR: Import of invalid class : " << bind_path);
                throw ParseError::Generic("Wildcard import of non-module/enum");
            }
        }
        else if( bind_name == path[0].name() ) {
            path = AST::Path::add_tailing(bind_path, path);
            path.resolve( crate );
            return true;
        }
    }
    #endif
}
void CPathResolver::handle_params(AST::TypeParams& params)
{
    DEBUG("params");
    for( auto& param : params.ty_params() )
    {
        handle_type(param.get_default());
        local_type( param.name(), TypeRef(TypeRef::TagArg(), param.name(), params) );
    }
    DEBUG("Bounds");
    for( auto& bound : params.bounds() )
    {
        handle_type(bound.test());
        
        // TODO: Will there be a case where within the test there's a Self that isn't the bound?
        local_type("Self", bound.test());
        if( !bound.is_trait() )
            DEBUG("namecheck lifetime bounds?");
        else
            handle_path(bound.bound(), CASTIterator::MODE_TYPE);
        m_locals.pop_back();
    }
}
void CPathResolver::handle_path(AST::Path& path, CASTIterator::PathMode mode)
{
    TRACE_FUNCTION_F("path = " << path << ", m_module_path = " << m_module_path);
 
    handle_path_int(path, mode);
       
    // Handle generic components of the path
    // - Done AFTER resoltion, as binding might introduce defaults (which may not have been resolved)
    for( auto& ent : path.nodes() )
    {
        for( auto& arg : ent.args() )
        {
            handle_type(arg);
        }
    }
}
void CPathResolver::handle_path_int(AST::Path& path, CASTIterator::PathMode mode)
{ 
    // Convert to absolute
    switch( path.type() )
    {
    case AST::Path::ABSOLUTE:
        DEBUG("Absolute - binding");
        INDENT();
        // Already absolute, our job is done
        // - However, if the path isn't bound, bind it
        if( !path.binding().is_bound() ) {
            path.resolve(m_crate);
        }
        else {
            DEBUG("- Path " << path << " already bound");
        }
        UNINDENT();
        break;
    case AST::Path::RELATIVE: {
        assert(path.size() > 0);
        DEBUG("Relative, local");
        
        // If there's a single node, and we're in expresion mode, look for a variable
        // Otherwise, search for a type
        bool is_trivial_path = (path.size() == 1 && path[0].args().size() == 0);
        
        LocalItem::Type search_type = (is_trivial_path && mode == MODE_EXPR ? LocalItem::VAR : LocalItem::TYPE);
        auto local_o = lookup_local( search_type, path[0].name() );
        if( local_o.is_some() )
        {
            auto local = local_o.unwrap();
            DEBUG("Local hit: " << path[0].name() << " = " << local);
            
            switch(mode)
            {
            // Local variable?
            // - TODO: What would happen if MODE_EXPR but path isn't a single ident?
            case MODE_EXPR:
                if( local.type == LocalItem::VAR )
                {
                    if( !is_trivial_path )
                        throw ParseError::Todo("TODO: MODE_EXPR, but not a single identifer, what do?");
                    DEBUG("Local variable " << path[0].name());
                    path = AST::Path(AST::Path::TagLocal(), path[0].name());
                    return ;
                }
                if( is_trivial_path )
                    throw ParseError::Generic("Type used in expression context?");
            // Type parameter
            case MODE_TYPE:
                {
                    // Convert to a UFCS path
                    DEBUG("Local type");
                    // - "<Type as _>::nodes"
                    auto np = AST::Path(AST::Path::TagUfcs(), local.tr, TypeRef());
                    np.add_tailing(path);
                    path = ::std::move(np);
                    DEBUG("path = " << path);
                    handle_path_ufcs(path, mode);
                }
                return ;
            // Binding is valid
            case MODE_BIND:
                //break ;
                throw ParseError::Todo("TODO: MODE_BIND, but local hit, what do?");
            }
        
            // TODO: What about sub-types and methods on type params?
            // - Invalid afaik, instead Trait::method() is used
        }
        
        
        if( path.nodes()[0].name() == "super" )
        {
            // Unwrap a single named node from the module path, and search that path
            // - Requires resolving that path to a module to pass to lookup_path_in_module
            AST::Path   local_path = m_module_path;
            local_path.nodes().pop_back();
            local_path.resolve(m_crate);
            DEBUG("'super' path is relative to " << local_path);
            path.nodes().erase( path.nodes().begin() );   // delete the 'super' node
            const AST::Module& mod = local_path.binding().bound_module();
            if( lookup_path_in_module(m_crate, mod, local_path, path) )
                return ;
            // this should always be an error, as 'super' paths are never MaybeBind
        }
        else
        {
           // Search backwards up the stack of anon modules
            if( m_module_stack.size() )
            {
                AST::Path   local_path = m_module_path;
                for(unsigned int i = 0; i < m_module_stack.size(); i ++)
                    local_path.nodes().push_back( AST::PathNode( FMT("#" << m_module_stack[i].first), {} ) );
                
                for(unsigned int i = m_module_stack.size(); i--; )
                {
                    if( lookup_path_in_module(m_crate, *m_module_stack[i].second, local_path, path) ) {
                        // Success!
                        return ;
                    }
                    local_path.nodes().pop_back();
                }
            }
            // Search current module, if found return with no error
            if( lookup_path_in_module(m_crate, *m_module, m_module_path, path) )
            {
                return;
            }
        }
        
        DEBUG("no matches found for path = " << path);
        assert( path.is_relative() );
        if( mode != MODE_BIND )
            throw ParseError::Generic("CPathResolver::handle_path - Name resolution failed");
        break; }
    case AST::Path::LOCAL:
        // Don't touch locals, they're already known
        break;
    case AST::Path::UFCS:
        handle_path_ufcs(path, mode);
        break;
    }
}
void CPathResolver::handle_path_ufcs(AST::Path& path, CASTIterator::PathMode mode)
{
    assert( path.type() == AST::Path::UFCS );
    // 1. Handle sub-types
    handle_type(path.ufcs().at(0));
    handle_type(path.ufcs().at(1));
    // 2. Handle wildcard traits (locate in inherent impl, or from an in-scope trait)
    if( path.ufcs().at(1).is_wildcard() )
    {
        DEBUG("Searching for impls when trait is _");
        
        // Search applicable type parameters for known implementations
        
        // 1. Inherent
        AST::Impl*  impl_ptr;
        ::std::vector<TypeRef> params;
        if( m_crate.find_impl(AST::Path(), path.ufcs().at(0), &impl_ptr, &params) )
        {
            DEBUG("Found matching inherent impl");
            // - Mark as being from the inherent, and move along
            //  > TODO: What about if this item is actually from a trait (due to genric restrictions)
            path.ufcs().at(1) = TypeRef(TypeRef::TagInvalid());
        }
        else
        {            
            // Iterate all traits in scope, and find one that is impled for this type
            throw ParseError::Todo("CPathResolver::handle_path - UFCS, find trait");
        }
    }
    // 3. Call resolve to attempt binding
    path.resolve(m_crate);
}

void CPathResolver::handle_type(TypeRef& type)
{
    TRACE_FUNCTION_F("type = " << type);
    // PROBLEM: Recursion when evaluating Self that expands to UFCS mentioning Self
    //  > The inner Self shouldn't be touched, but it gets hit by this method, and sudden recursion
    //if( type.is_locked() )
    //{
    //}
    //else
    if( type.is_path() && type.path().is_trivial() )
    {
        const auto& name = type.path()[0].name();
        auto opt_local = lookup_local(LocalItem::TYPE, name);
         
        if( opt_local.is_some() )
        {
            type = opt_local.unwrap().tr;
        }
        else if( name == "Self" )
        {
            // If the name was "Self", but Self isn't already defined... then we need to make it an arg?
            throw CompileError::Generic( FMT("CPathResolver::handle_type - Unexpected 'Self'") );
            type = TypeRef(TypeRef::TagArg(), "Self");
        }
        else
        {
            // Not a type param, fall back to other checks
        }
    }
    else if( type.is_type_param() )
    {
        const auto& name = type.type_param();
        auto opt_local = lookup_local(LocalItem::TYPE, name);
        /*if( name == "Self" )
        {
            // Good as it is
        }
        else*/ if( opt_local.is_some() )
        {
            type = opt_local.unwrap().tr;
        }
        else
        {
            // Not a type param, fall back to other checks
            throw CompileError::Generic( FMT("CPathResolver::handle_type - Invalid parameter '" << name << "'") );
        }
    }
    else
    {
        // No change
    }
    DEBUG("type = " << type);
    
    //if( type.is_type_param() && type.type_param() == "Self" )
    //{
    //    auto l = lookup_local(LocalItem::TYPE, "Self");
    //    if( l.is_some() )
    //    {
    //        type = l.unwrap().tr;
    //        DEBUG("Replacing Self with " << type);
    //        // TODO: Can this recurse?
    //        handle_type(type);
    //        return ;
    //    }
    //}
    CASTIterator::handle_type(type);
    DEBUG("res = " << type);
}
void CPathResolver::handle_expr(AST::ExprNode& node)
{
    CResolvePaths_NodeVisitor   nv(*this);
    node.visit(nv);
}

void CPathResolver::handle_pattern(AST::Pattern& pat, const TypeRef& type_hint)
{
    TRACE_FUNCTION_F("pat = " << pat);
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
                // TODO: Undo anon modules until item is found
                DEBUG("Absolutised path " << imp.data << " into " << newpath);
                imp.data = ::std::move(newpath);
            }
        }
        
        // Run resolution on import
        imp.data.resolve(crate, false);
        DEBUG("Resolved import : " << imp.data);
        
        // If wildcard, make sure it's sane
        if( imp.name == "" )
        {
            switch(imp.data.binding().type())
            {
            case AST::PathBinding::UNBOUND:
                throw ParseError::BugCheck("path unbound after calling .resolve()");
            case AST::PathBinding::MODULE:
                break;
            case AST::PathBinding::ENUM:
                break;
            
            default:
                throw ParseError::Generic("Wildcard imports are only allowed on modules and enums");
            }
        }
    }
    
    for( auto& new_imp : new_imports )
    {
        if( not new_imp.binding().is_bound() ) {
            new_imp.resolve(crate, false);
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
        // - Disable expectation of type parameters
        imp.data.resolve(crate, false);
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
