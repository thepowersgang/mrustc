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
    struct Scope {
        unsigned int    module_idx;
        AST::Module *module;    // can be NULL
        AST::Path   module_path;
        ::std::vector< ::std::string >  locals;
        
        ::std::vector< ::std::pair<AST::Path, const AST::Trait&> >    traits;
    };
    ::std::vector<Scope>    m_scope_stack;
    ::std::vector< TypeRef >  m_self_type;
    
    friend class CResolvePaths_NodeVisitor;
    
public:
    CPathResolver(const AST::Crate& crate);

    void handle_params(AST::TypeParams& params) override;

    virtual void handle_path(AST::Path& path, CASTIterator::PathMode mode) override;
    void handle_path_ufcs(AST::Path& path, CASTIterator::PathMode mode);
    bool find_trait_item(const AST::Path& path, AST::Trait& trait, const ::std::string& item_name, bool& out_is_method, AST::Path& out_trait_path);
    virtual void handle_type(TypeRef& type) override;
    virtual void handle_expr(AST::ExprNode& node) override;
    
    virtual void handle_pattern(AST::Pattern& pat, const TypeRef& type_hint) override;
    virtual void handle_module(AST::Path path, AST::Module& mod) override;
    virtual void handle_trait(AST::Path path, AST::Trait& trait) override;
    virtual void handle_function(AST::Path path, AST::Function& fcn) override;

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
        assert(m_scope_stack.size() > 0);
        m_scope_stack.back().locals.push_back( ::std::move(name) );
    }
    virtual void local_use(::std::string name, AST::Path path) override {
        assert( !path.binding().is_Unbound() );
        if( path.binding().is_Trait() ) {
            m_scope_stack.back().traits.push_back( ::std::pair<AST::Path, const AST::Trait&>(path, *path.binding().as_Trait().trait_) );
        }
    }
    virtual void end_scope() override;
    
    ::rust::option<const LocalItem&> lookup_local(LocalItem::Type type, const ::std::string& name) const;
    
    bool find_local_item(AST::Path& path, const ::std::string& name, bool allow_variables);
    //bool find_local_item(AST::Path& path, bool allow_variables);
    bool find_mod_item(AST::Path& path, const ::std::string& name);
    bool find_self_mod_item(AST::Path& path, const ::std::string& name);
    bool find_super_mod_item(AST::Path& path, const ::std::string& name);
    bool find_type_param(const ::std::string& name);
    
    // TODO: Handle a block and obtain the local module (if any)
private:
    void handle_path_int(AST::Path& path, CASTIterator::PathMode mode);
    
    ::std::vector< ::std::pair<AST::Path, const AST::Trait&> > inscope_traits() const
    {
        ::std::vector< ::std::pair<AST::Path, const AST::Trait&> >    ret;
        for( auto it = m_scope_stack.rbegin(); it != m_scope_stack.rend(); ++it )
        {
            for( const auto& t : it->traits ) {
                DEBUG("t = " << t.first);
                //assert(t.first.binding().is_Trait());
                ret.push_back(t);
            }
        }
        return ret;
    }


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
            AST::Module* parent_mod_p = m_res.m_module;
            for(const auto& e : m_res.m_scope_stack)
                if(e.module != nullptr)
                    parent_mod_p = e.module;
            AST::Module& parent_mod = *parent_mod_p;
            auto idx = parent_mod.add_anon_module( node.m_inner_mod.get() );
            
            // Obtain the path
            AST::Path   local_path = m_res.m_module_path;
            for(const auto& e : m_res.m_scope_stack ) {
                if( e.module != nullptr ) {
                    local_path.nodes().push_back( AST::PathNode( FMT("#" << e.module_idx), {} ) );
                }
            }
            local_path.nodes().push_back( AST::PathNode(FMT("#" << idx), {}) );

            // And add to the list of modules to use in lookup
            m_res.m_scope_stack.push_back( {idx, node.m_inner_mod.get(), local_path, {}} );
            
            // Do use resolution on this module
            // TODO: When is more advanced resolution done?
            ResolvePaths_HandleModule_Use(m_res.m_crate, m_res.m_scope_stack.back().module_path, *node.m_inner_mod);
        }
        else {
            m_res.m_scope_stack.push_back( {0, nullptr, AST::Path(), {}} );
        }
        AST::NodeVisitorDef::visit(node);
        // Once done, pop the module
        m_res.m_scope_stack.pop_back();
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
            DEBUG("- ExprNode_Closure: pat=" << param.first << ", ty=" << param.second);
            m_res.handle_type(param.second);
            m_res.handle_pattern(param.first, param.second);
        }
        DEBUG("- ExprNode_Closure: rt=" << node.m_return);
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
    m_scope_stack.push_back( {0, nullptr, AST::Path(), {}} );
    m_locals.push_back( LocalItem() );
}
void CPathResolver::end_scope()
{
    m_scope_stack.pop_back( );
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
bool lookup_path_in_module(const AST::Crate& crate, const AST::Module& module, const AST::Path& mod_path, AST::Path& path, const ::std::string& name, bool is_leaf)
{
    TRACE_FUNCTION_F("mod_path="<<mod_path);
    // - Allow leaf nodes if path is a single node, don't skip private wildcard imports
    auto item = module.find_item(name, is_leaf, false);
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
        return false; }
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
}
bool lookup_path_in_module(const AST::Crate& crate, const AST::Module& module, const AST::Path& mod_path, AST::Path& path) {
    return lookup_path_in_module(crate, module, mod_path, path, path[0].name(), path.size() == 1);
}

/// Perform path resolution within a generic definition block
void CPathResolver::handle_params(AST::TypeParams& params)
{
    // Parameters
    DEBUG("params");
    for( auto& param : params.ty_params() )
    {
        // - Resolve the default type
        handle_type(param.get_default());
        // - Register each param as a type name within this scope
        local_type( param.name(), TypeRef(TypeRef::TagArg(), param.name(), params) );
    }
    DEBUG("Bounds");
    for( auto& bound : params.bounds() )
    {
        TU_MATCH(AST::GenericBound, (bound), (ent),
        (Lifetime,
            {}
            ),
        (TypeLifetime,
            handle_type(ent.type);
            ),
        (IsTrait,
            handle_type(ent.type);
            m_self_type.push_back( TypeRef() );
            handle_path(ent.trait, MODE_TYPE);
            m_self_type.pop_back();
            ),
        (MaybeTrait,
            handle_type(ent.type);
            m_self_type.push_back( TypeRef() );
            handle_path(ent.trait, MODE_TYPE);
            m_self_type.pop_back();
            ),
        (NotTrait,
            handle_type(ent.type);
            m_self_type.push_back( TypeRef() );
            handle_path(ent.trait, MODE_TYPE);
            m_self_type.pop_back();
            ),
        (Equality,
            handle_type(ent.type);
            handle_type(ent.replacement);
            )
        )
    }
}

/// Resolve names within a path
void CPathResolver::handle_path(AST::Path& path, CASTIterator::PathMode mode)
{
    TRACE_FUNCTION_F("path = " << path << ", m_module_path = " << m_module_path);
 
    handle_path_int(path, mode);
    
    // Handle generic components of the path
    // - Done AFTER resoltion, as binding might introduce defaults (which may not have been resolved)
    TU_MATCH(AST::Path::Class, (path.m_class), (info),
    (Invalid),
    (Local),
    (Relative,
        for( auto& ent : info.nodes )
            for( auto& arg : ent.args() )
                handle_type(arg);
        ),
    (Self,
        for( auto& ent : info.nodes )
            for( auto& arg : ent.args() )
                handle_type(arg);
        ),
    (Super,
        for( auto& ent : info.nodes )
            for( auto& arg : ent.args() )
                handle_type(arg);
        ),
    (Absolute,
        for( auto& ent : info.nodes )
            for( auto& arg : ent.args() )
                handle_type(arg);
        ),
    (UFCS,
        handle_type(*info.type);
        handle_type(*info.trait);
        for( auto& ent : info.nodes )
            for( auto& arg : ent.args() )
                handle_type(arg);
        )
    )
}
void CPathResolver::handle_path_int(AST::Path& path, CASTIterator::PathMode mode)
{ 
    // Convert to absolute
    // - This means converting all partial forms (i.e. not UFCS, Variable, or Absolute)
    switch( path.class_tag() )
    {
    case AST::Path::Class::Invalid:
        assert( !path.m_class.is_Invalid() );
        return;
    // --- Already absolute forms
    // > Absolute: Resolve
    case AST::Path::Class::Absolute:
        DEBUG("Absolute - binding");
        INDENT();
        // Already absolute, our job is done
        // - However, if the path isn't bound, bind it
        if( path.binding().is_Unbound() ) {
            path.resolve(m_crate);
        }
        else {
            DEBUG("- Path " << path << " already bound");
        }
        UNINDENT();
        break;
    // > UFCS: Expand the types
    case AST::Path::Class::UFCS:
        handle_path_ufcs(path, mode);
        break;
    // > Variable: (wait, how is this known already?)
    // - 'self'
    case AST::Path::Class::Local:
        if( !path.binding().is_Unbound() )
        {
            DEBUG("- Path " << path << " already bound");
        }
        else
        {
            const auto& info = path.m_class.as_Local();
            // 1. Check for local items
            if( this->find_local_item(path, info.name, (mode == CASTIterator::MODE_EXPR)) ) {
                path.resolve(m_crate);
                break ;
            }
            else {
                // No match, fall through
            }
            // 2. Type parameters (ONLY when in type mode)
            if( mode == CASTIterator::MODE_TYPE ) {
                throw ::std::runtime_error("TODO: Local in CPathResolver::handle_path_int type param");
            }
            // 3. Module items
            if( this->find_mod_item(path, info.name) ) {
                path.resolve(m_crate);
                break;
            }
            else {
            }
            
            DEBUG("no matches found for path = " << path);
            if( mode != MODE_BIND )
                throw ParseError::Generic("CPathResolver::handle_path - Name resolution failed (Local)");
            return ;
        }
        break;
    
    case AST::Path::Class::Relative:
        // 1. function scopes (variables and local items)
        // > Return values: name or path
        {
            bool allow_variables = (mode == CASTIterator::MODE_EXPR && path.is_trivial());
            if( this->find_local_item(path, path[0].name(), allow_variables) ) {
                path.resolve(m_crate);
                break ;
            }
            else {
                // No match, fall through
            }
        }
        
        // 2. Type parameters
        // - Should probably check if this is expression mode, bare types are invalid there
        // NOTES:
        // - If the path is bare (i.e. there are no more nodes), then ensure that the mode is TYPE
        // - If there are more nodes, replace with a UFCS block
        {
            auto tp = this->find_type_param(path[0].name());
            if( tp != false /*nullptr*/ )
            {
                if(path.size() > 1) {
                    // Repalce with UFCS
                    auto newpath = AST::Path(AST::Path::TagUfcs(), TypeRef(TypeRef::TagArg(), path[0].name()), TypeRef());
                    newpath.add_tailing(path);
                    path = mv$(newpath);
                    handle_path_ufcs(path, mode);
                }
                else {
                    // Mark as local
                    // - TODO: Not being trivial is an error, not a bug
                    assert( path.is_trivial() );
                    path = AST::Path(AST::Path::TagLocal(), path[0].name());
                    // - TODO: Need to bind this to the source parameter block
                }
                break;
            }
        }
        
        // 3. current module
        {
            if( this->find_mod_item(path, path[0].name()) ) {
                path.resolve(m_crate);
                break;
            }
            else {
            }
        }
        
        // *. No match? I give up
        DEBUG("no matches found for path = " << path);
        if( mode != MODE_BIND )
            throw ParseError::Generic("CPathResolver::handle_path - Name resolution failed");
        return ;
    
    // Module relative
    case AST::Path::Class::Self:{
        if( this->find_self_mod_item(path, path[0].name()) ) {
            break;
        }
        else {
        }
        DEBUG("no matches found for path = " << path);
        if( mode != MODE_BIND )
            throw ParseError::Generic("CPathResolver::handle_path - Name resolution failed");
        break; }
    // Parent module relative
    case AST::Path::Class::Super:{
        if( this->find_super_mod_item(path, path[0].name()) ) {
            break;
        }
        else {
        }
        DEBUG("no matches found for path = " << path);
        if( mode != MODE_BIND )
            throw ParseError::Generic("CPathResolver::handle_path - Name resolution failed");
        break; }
    }
    
    // TODO: Are there any reasons not to be bound at this point?
    //assert( !path.binding().is_Unbound() );
}
void CPathResolver::handle_path_ufcs(AST::Path& path, CASTIterator::PathMode mode)
{
    assert(path.m_class.is_UFCS());
    auto& info = path.m_class.as_UFCS();
    TRACE_FUNCTION_F("info={< " << *info.type << " as " << *info.trait << ">::" << info.nodes << "}");
    const ::std::string&    item_name = info.nodes[0].name();
    // 1. Handle sub-types
    handle_type(*info.type);
    handle_type(*info.trait);
    // 2. Handle wildcard traits (locate in inherent impl, or from an in-scope trait)
    if( info.trait->is_wildcard() )
    {
        DEBUG("Searching for impls when trait is _ (trait = " << *info.trait << ")");
        
        // Search applicable type parameters for known implementations
        
        // 1. Inherent
        //AST::Impl*  impl_ptr;
        ::std::vector<TypeRef> params;
        if( info.type->is_type_param() && info.type->type_param() == "Self" )
        {
            // TODO: What is "Self" here? May want to use `GenericBound`s to replace Self with the actual type when possible.
            //       In which case, Self will refer to "implementor of this trait".
            // - Look up applicable traits for this type, using bounds (basically same as next)
            throw ParseError::Todo("CPathResolver::handle_path_ufcs - Handle '<Self as _>::...'");
        }
        else if( info.type->is_type_param() )
        {
            DEBUG("Checking applicable generic bounds");
            const auto& tp = *info.type->type_params_ptr();
            assert(&tp != nullptr);
            bool success = false;
            
            // Enumerate bounds
            for( const auto& bound : tp.bounds() )
            {
                DEBUG("bound = " << bound);
                TU_MATCH_DEF(AST::GenericBound, (bound), (ent),
                (),
                (IsTrait,
                    if( ent.type == *info.type ) {
                        auto& t = *const_cast<AST::Trait*>(ent.trait.binding().as_Trait().trait_);
                        DEBUG("Type match, t.params() = " << t.params());
                        bool is_method;
                        AST::Path   found_trait_path;
                        DEBUG("find_trait_item(" << ent.trait << /*", t=" << t <<*/ ", item_name = " << item_name);
                        if( this->find_trait_item(ent.trait, t, item_name,  is_method, found_trait_path) )
                        {
                            if( is_method ) {
                                if( info.nodes.size() != 1 )
                                    throw ParseError::Generic("CPathResolver::handle_path_ufcs - Sub-nodes to method");
                            }
                            else {
                                if( info.nodes.size() != 1 )
                                    throw ParseError::Todo("CPathResolver::handle_path_ufcs - Sub nodes on associated type");
                            }
                            *info.trait = TypeRef( mv$(found_trait_path) );
                            success = true;
                            break ;
                        }
                    }
                    else {
                        DEBUG("Type mismatch " << ent.type << " != " << *info.type);
                    }
                    )
                )
            }
            
            if( !success )
                throw ParseError::Todo( FMT("CPathResolver::handle_path_ufcs - UFCS, find trait for generic matching '" << item_name << "'") );
            // - re-handle, to ensure that the bound is resolved
            handle_type(*info.trait);
        }
        else
        {
            // Iterate all inherent impls
            for( auto impl : m_crate.find_inherent_impls(*info.type) ) {
                IF_OPTION_SOME(item, impl.find_named_item(item_name), {
                    DEBUG("Found matching inherent impl");
                    *info.trait = TypeRef(TypeRef::TagInvalid());
                    //path.set_binding(item);
                    path.resolve(m_crate);
                    return ;
                })
            }
            // Iterate all traits in scope, and find one that is implemented for this type
            // - TODO: Iterate traits to find match for <Type as _>
            for( const auto& trait_ref : this->inscope_traits() )
            {
                const auto& trait_p = trait_ref.first;
                const auto& trait = trait_ref.second;
                bool is_fcn;
                if( trait.has_named_item(item_name, is_fcn) ) {
                    IF_OPTION_SOME(impl, m_crate.find_impl( trait_p, *info.type ), {
                        *info.trait = TypeRef( trait_p );
                        //auto item = impl.find_named_item(item_name).unwrap();
                        //path.set_binding(item);
                        path.resolve(m_crate);
                        return ;
                    })
                }
            }

            throw ParseError::Todo("CPathResolver::handle_path_ufcs - UFCS, find trait");
        }
    }
    // 3. Call resolve to attempt binding
    path.resolve(m_crate);
}

bool CPathResolver::find_trait_item(const AST::Path& path, AST::Trait& trait, const ::std::string& item_name, bool& out_is_method, AST::Path& out_trait_path)
{
    {
        const auto& fcns = trait.functions();
        //DEBUG("fcns = " << fcns);
        auto it = ::std::find_if( fcns.begin(), fcns.end(), [&](const AST::Item<AST::Function>& a) { DEBUG("fcn " << a.name); return a.name == item_name; } );
        if( it != fcns.end() ) {
            // Found it.
            out_is_method = true;
            DEBUG("&out_trait_path = " << &out_trait_path);
            out_trait_path = AST::Path(path);
            return true;
        }
    }
    {
        const auto& types = trait.types();
        auto it = ::std::find_if( types.begin(), types.end(), [&](const AST::Item<AST::TypeAlias>& a) { DEBUG("type " << a.name); return a.name == item_name; } );
        if( it != types.end() ) {
            // Found it.
            out_is_method = false;
            out_trait_path = AST::Path(path);
            return true;
        }
    }
    
    for( auto& st : trait.supertraits() ) {
        assert(st.is_bound());
        if( this->find_trait_item(st, *const_cast<AST::Trait*>(st.binding().as_Trait().trait_), item_name,  out_is_method, out_trait_path) )
            return true;
    }
    
    return false;
}

bool CPathResolver::find_local_item(AST::Path& path, const ::std::string& name, bool allow_variables)
{
    TRACE_FUNCTION_F("path="<<path<<", allow_variables="<<allow_variables);
    // Search current scopes for a name
    // - This should search both the expression stack
    // - and the scope's module (if any)
    for(auto it = m_scope_stack.rbegin(); it != m_scope_stack.rend(); ++it)
    {
        const auto& s = *it;
        if( allow_variables )
        {
            for( auto it2 = s.locals.rbegin(); it2 != s.locals.rend(); ++it2 )
            {
                if( *it2 == name ) {
                    path = AST::Path(AST::Path::TagLocal(), name);
                    path.bind_variable(0);
                    return true;
                }
            }
        }
        if( s.module != nullptr )
        {
            DEBUG("- Looking in sub-module '" << s.module_path << "'");
            if( lookup_path_in_module(m_crate, *s.module, s.module_path, path, name, path.is_trivial()) )
                return true;
        }
    }
    return false;
}
bool CPathResolver::find_mod_item(AST::Path& path, const ::std::string& name) {
    const AST::Module* mod = m_module;
    do {
        if( lookup_path_in_module(m_crate, *mod, m_module_path, path, name, path.size()==1) )
            return true;
        if( mod->name() == "" )
            throw ParseError::Todo("Handle anon modules when resoling unqualified relative paths");
    } while( mod->name() == "" );
    return false;
}
bool CPathResolver::find_self_mod_item(AST::Path& path, const ::std::string& name) {
    if( m_module->name() == "" )
        throw ParseError::Todo("Correct handling of 'self' in anon modules");
    
    return lookup_path_in_module(m_crate, *m_module, m_module_path, path, name, path.size()==1);
}
bool CPathResolver::find_super_mod_item(AST::Path& path, const ::std::string& name) {
    if( m_module->name() == "" )
        throw ParseError::Todo("Correct handling of 'super' in anon modules");
    
    // 1. Construct path to parent module
    AST::Path   super_path = m_module_path;
    super_path.nodes().pop_back();
    assert( super_path.nodes().size() > 0 );
    if( super_path.nodes().back().name()[0] == '#' )
        throw ParseError::Todo("Correct handling of 'super' in anon modules (parent is anon)");
    // 2. Resolve that path
    super_path.resolve(m_crate);
    // 3. Call lookup_path_in_module
    return lookup_path_in_module(m_crate, *super_path.binding().as_Module().module_, super_path, path,  name, path.size()==1);
}
bool CPathResolver::find_type_param(const ::std::string& name) {
    for( auto it = m_locals.end(); it -- != m_locals.begin(); )
    {
        if( it->type == LocalItem::TYPE ) {
            if( it->name == name ) {
                return true;
            }
        }
    }
    return false;
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
            ERROR(type.path().span(), E0000, "Unexpected 'Self'");
            
            TypeRef nt = TypeRef(TypeRef::TagArg(), "Self");
            nt.set_span(type.span());
            type = nt;
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
            // - TODO: Allow replacing this with the real Self (e.g. in an impl block)
            // - NEED to annotate with the relevant impl block, and with other bounds
            //  > So that you can handle 'where Self: Sized' etc
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
        AST::Path newpath = AST::Path(AST::Path::TagRelative(), { AST::PathNode(name) });
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
    ::std::vector<Scope>    saved = mv$(m_scope_stack);
    // NOTE: Assigning here is safe, as the CASTIterator handle_module iterates submodules as the last action
    m_module = &mod;
    m_module_path = AST::Path(path);
    CASTIterator::handle_module(mv$(path), mod);
    m_scope_stack = mv$(saved);
}
void CPathResolver::handle_trait(AST::Path path, AST::Trait& trait)
{
    // Handle local 
    for( auto& st : trait.supertraits() ) {
        handle_path(st, MODE_TYPE);
    }
    m_scope_stack.back().traits.push_back( ::std::pair<AST::Path, const AST::Trait&>(path, trait) );
}
void CPathResolver::handle_function(AST::Path path, AST::Function& fcn)
{
    m_scope_stack.push_back( {0, nullptr, AST::Path(), {}} );
    CASTIterator::handle_function(::std::move(path), fcn);
    m_scope_stack.pop_back();
}

void ResolvePaths_HandleModule_Use(const AST::Crate& crate, const AST::Path& modpath, AST::Module& mod)
{
    TRACE_FUNCTION_F("modpath = " << modpath);
    ::std::vector<AST::Path>    new_imports;
    for( auto& imp : mod.imports() )
    {
        AST::Path& p = imp.data;
        DEBUG("p = " << p);
        switch( p.class_tag() )
        {
        case AST::Path::Class::Absolute:
            // - No action
            break;
        // 'super' - Add parent path
        // - TODO: Handle nested modules correctly.
        case AST::Path::Class::Super: {
            if( modpath.size() < 1 )
                throw ParseError::Generic("Encountered 'super' at crate root");
            auto newpath = modpath;
            newpath.nodes().pop_back();
            newpath += p;
            DEBUG("Absolutised path " << p << " into " << newpath);
            p = ::std::move(newpath);
            break; }
        // 'self' - Add parent path
        // - TODO: Handle nested modules correctly.
        case AST::Path::Class::Self: {
            auto newpath = modpath + p;
            // TODO: Undo anon modules until item is found
            DEBUG("Absolutised path " << p << " into " << newpath);
            p = ::std::move(newpath);
            break; }
        // Any other class is an error
        case AST::Path::Class::Relative:
            throw ParseError::Generic( FMT("Encountered relative path in 'use': " << p) );
        default:
            throw ParseError::Generic( FMT("Invalid path type encounted in 'use' : " << p.class_tag() << " " << p) );
        }
        
        // Run resolution on import
        imp.data.resolve(crate, false);
        DEBUG("Resolved import : " << imp.data);
        
        // If wildcard, make sure it's sane
        if( imp.name == "" )
        {
            TU_MATCH_DEF(AST::PathBinding, (imp.data.binding()), (info),
            (
                throw ParseError::Generic("Wildcard imports are only allowed on modules and enums");
                ),
            (Unbound,
                throw ParseError::BugCheck("path unbound after calling .resolve()");
                ),
            (Module, (void)0;),
            (Enum, (void)0;)
            )
        }
    }
    
    for( auto& new_imp : new_imports )
    {
        if( new_imp.binding().is_Unbound() ) {
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
    ResolvePaths_HandleModule_Use(crate, AST::Path("", {}), crate.root_module());
    UNINDENT();
    
    // Then do path resolution on all other items
    CPathResolver	pr(crate);
    DEBUG(" ---");
    pr.handle_module(AST::Path("", {}), crate.root_module());
    DEBUG(" <<<");
}
