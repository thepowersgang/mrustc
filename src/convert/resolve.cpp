/*
 * "mrustc" Rust->C converter
 * - By John Hodge (Mutabah / thePowersGang)
 *
 * convert/resolve.cpp
 * - Resolve names into absolute format
 * 
 * - Converts all paths into a canonical format (absolute, local, or UFCS)
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
    const AST::Crate&   m_crate;
    AST::Module*  m_module;
    AST::Path m_module_path;
    
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
    ::std::vector< LocalItem >  m_locals;
    
    struct Scope {
        unsigned int    module_idx;
        AST::Module *module;    // can be NULL
        AST::Path   module_path;
        ::std::vector< ::std::string >  locals;
        
        ::std::vector< ::std::pair<AST::Path, const AST::Trait&> >    traits;
    };
    ::std::vector<Scope>    m_scope_stack;
    
    
    TAGGED_UNION(SelfType, None,
        (None, ()),
        (Type, (
            TypeRef type;
            )),
        (Trait, (
            AST::Path   path;
            const AST::Trait* trait;
            ))
        );
    ::std::vector<SelfType>  m_self_type;
    
    friend class CResolvePaths_NodeVisitor;
    
public:
    bool m_second_pass = false;

    CPathResolver(const AST::Crate& crate);

    void handle_params(AST::TypeParams& params) override;

    virtual void handle_path(AST::Path& path, CASTIterator::PathMode mode) override;
    void handle_path_abs(const Span& span, AST::Path& path, CASTIterator::PathMode mode);
    void handle_path_abs__into_ufcs(const Span& span, AST::Path& path, unsigned slice_from, unsigned split_point);
    void handle_path_ufcs(const Span& span, AST::Path& path, CASTIterator::PathMode mode);
    void handle_path_rel(const Span& span, AST::Path& path, CASTIterator::PathMode mode);
    bool find_trait_item(const Span& span, const AST::Path& path, AST::Trait& trait, const ::std::string& item_name, bool& out_is_method, AST::Path& out_trait_path);
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
    
    bool find_local_item(const Span& span, AST::Path& path, const ::std::string& name, bool allow_variables);
    //bool find_local_item(AST::Path& path, bool allow_variables);
    bool find_mod_item(const Span& span, AST::Path& path, const ::std::string& name);
    bool find_self_mod_item(const Span& span, AST::Path& path, const ::std::string& name);
    bool find_super_mod_item(const Span& span, AST::Path& path, const ::std::string& name);
    bool find_type_param(const ::std::string& name);
    
    virtual void push_self() override {
        m_self_type.push_back( SelfType::make_None({}) );
    }
    virtual void push_self(AST::Path path, const AST::Trait& trait) override {
        m_self_type.push_back( SelfType::make_Trait( {path, &trait} ) );
    }
    virtual void push_self(TypeRef real_type) override {
        // 'Self' must be resolved because ...
        //if( real_type.is_path() && real_type.path().binding().is_Unbound() ) {
        //    assert( !"Unbound path passed to push_self" );
        //}
        m_self_type.push_back( SelfType::make_Type( {real_type} ) );
    }
    virtual void pop_self() override {
        m_self_type.pop_back();
    }
    
    // TODO: Handle a block and obtain the local module (if any)
private:
    void handle_path_int(const Span& span, AST::Path& path, CASTIterator::PathMode mode);
    
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

/// Do simple path resolution on this path
/// - Simple involves only doing item name lookups, no checks of generic params at all or handling UFCS
void resolve_path(const Span& span, const AST::Crate& root_crate, AST::Path& path)
{
    if( !path.is_absolute() ) {
        BUG(span, "Calling resolve_path on non-absolute path - " << path);
    }
    const AST::Module* mod = &root_crate.root_module();
    for(const auto& node : path.nodes() ) {
        if( node.args().size() > 0 ) {
            throw ParseError::Generic("Unexpected generic params in use path");
        }
        if( mod == nullptr ) {
            if( path.binding().is_Enum() ) {
                auto& enm = *path.binding().as_Enum().enum_;
                for(const auto& variant : enm.variants()) {
                    if( variant.m_name == node.name() ) {
                        path.bind_enum_var(enm, node.name());
                        break;
                    }
                }
                if( path.binding().is_Enum() ) {
                    throw ParseError::Generic( FMT("Unable to find component '" << node.name() << "' of import " << path << " (enum)") );
                }
                break;
            }
            else {
                throw ParseError::Generic("Extra path components after final item");
            }
        }
        auto item = mod->find_item(node.name());
        //while( item.is_None() && node.name()[0] == '#' ) {
        //}
        // HACK: Not actually a normal TU, but it fits the same pattern
        TU_MATCH(AST::Module::ItemRef, (item), (i),
        (None,
            throw ParseError::Generic( FMT("Unable to find component '" << node.name() << "' of import " << path) );
            ),
        (Module,
            mod = &i;
            ),
        (Crate,
            mod = &root_crate.get_root_module(i);
            ),
        (TypeAlias,
            path.bind_type_alias(i);
            mod = nullptr;
            ),
        (Function,
            path.bind_function(i);
            mod = nullptr;
            ),
        (Trait,
            path.bind_trait(i);
            mod = nullptr;
            ),
        (Struct,
            path.bind_struct(i);
            mod = nullptr;
            ),
        (Enum,
            // - Importing an enum item will be handled in the nullptr check above
            path.bind_enum(i);
            mod = nullptr;
            ),
        (Static,
            path.bind_static(i);
            mod = nullptr;
            ),
        (Use,
            if(i.name == "") {
                throw ParseError::Todo("Handle resolving to wildcard use in use resolution");
            }
            else {
                // Restart lookup using new path
            }
            )
        )
    }
    if( mod != nullptr ) {
        path.bind_module(*mod);
    }
}

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
bool lookup_path_in_module(const Span& span, const AST::Crate& crate, const AST::Module& module, const AST::Path& mod_path, AST::Path& path, const ::std::string& name, bool is_leaf)
{
    TRACE_FUNCTION_F("mod_path="<<mod_path);
    // - Allow leaf nodes if path is a single node, don't skip private wildcard imports
    auto item = module.find_item(name, is_leaf, false);
    TU_MATCH_DEF(AST::Module::ItemRef, (item), (i),
    (
        path = mod_path + path;
        //path.resolve( crate );
        return true;
        ),
    (None,
        return false;
        ),
    (Use,
        const auto& imp = i;
        if( imp.name == "" )
        {
            DEBUG("Wildcard import found, " << imp.data << " + " << path);
            // Wildcard path, prefix entirely with the path
            path = imp.data + path;
            //path.resolve( crate );
            return true;
        }
        else
        {
            DEBUG("Named import found, " << imp.data << " + " << path << " [1..]");
            path = AST::Path::add_tailing(imp.data, path);
            //path.resolve( crate );
            return true;
        }
        return false;
        ),
    (Module,
        // Check name down?
        // Add current module path
        path = mod_path + path;
        //path.resolve( crate );
        return true;
        )
    )
    assert(!"");
}
bool lookup_path_in_module(const Span &span, const AST::Crate& crate, const AST::Module& module, const AST::Path& mod_path, AST::Path& path) {
    return lookup_path_in_module(span, crate, module, mod_path, path, path[0].name(), path.size() == 1);
}

/// Perform path resolution within a generic definition block
void CPathResolver::handle_params(AST::TypeParams& params)
{
    TRACE_FUNCTION;
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
        DEBUG("- Bound " << bound);

        TU_MATCH(AST::GenericBound, (bound), (ent),
        (Lifetime,
            {}
            ),
        (TypeLifetime,
            handle_type(ent.type);
            ),
        (IsTrait,
            handle_type(ent.type);
            // TODO: Should 'Self' in this trait be ent.type?

            //if(ent.type.is_path() && ent.type.path().binding().is_Unbound())
            //    BUG(span, "Unbound path after handle_type in handle_params - ent.type=" << ent.type);
            push_self(ent.type);
            handle_path(ent.trait, MODE_TYPE);
            pop_self();
            ),
        (MaybeTrait,
            handle_type(ent.type);
            push_self();
            handle_path(ent.trait, MODE_TYPE);
            pop_self();
            ),
        (NotTrait,
            handle_type(ent.type);
            push_self();
            handle_path(ent.trait, MODE_TYPE);
            pop_self();
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
    const Span span = Span();
    TRACE_FUNCTION_F("(path = " << path << ", mode = "<<mode<<"), m_module_path = " << m_module_path);
 
    handle_path_int(span, path, mode);
    
    // Handle generic components of the path
    // - Done AFTER resoltion, as binding might introduce defaults (which may not have been resolved)
    TU_MATCH(AST::Path::Class, (path.m_class), (info),
    (Invalid),
    (Local),
    (Relative,
        DEBUG("Relative path after handle_path_int - path=" << path);
        assert( !"Relative path after handle_path_int");
        ),
    (Self,
        assert( !"Relative (self) path after handle_path_int");
        ),
    (Super,
        assert( !"Relative (super) path after handle_path_int");
        ),
    (Absolute,
        if( path.binding().is_Unbound() ) {
            BUG(span, "Path wasn't bound after handle_path - path=" << path);
        }
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

void CPathResolver::handle_path_int(const Span& span, AST::Path& path, CASTIterator::PathMode mode)
{
    // Convert to absolute
    // - This means converting all partial forms (i.e. not UFCS, Variable, or Absolute)
    switch( path.class_tag() )
    {
    case AST::Path::Class::Invalid:
        // TODO: Throw an error
        assert( !path.m_class.is_Invalid() );
        return;
    // --- Already absolute forms
    // > Absolute: Resolve
    case AST::Path::Class::Absolute:
        DEBUG("Absolute - binding");
        INDENT();
        handle_path_abs(span, path, mode);
        // TODO: Move Path::resolve() to this file
        // Already absolute, our job is done
        // - However, if the path isn't bound, bind it
        if( path.binding().is_Unbound() ) {
            //path.resolve(m_crate);
        }
        else {
            DEBUG("- Path " << path << " already bound");
        }
        UNINDENT();
        break;
    // > UFCS: Resolve the type and trait
    case AST::Path::Class::UFCS:
        handle_path_ufcs(span, path, mode);
        break;
    // > Variable: (wait, how is this known already?)
    // - 'self', 'Self'
    case AST::Path::Class::Local:
        DEBUG("Check local");
        if( !path.binding().is_Unbound() )
        {
            DEBUG("- Path " << path << " already bound");
        }
        else
        {
            const auto& info = path.m_class.as_Local();
            // 1. Check for local items
            if( this->find_local_item(span, path, info.name, (mode == CASTIterator::MODE_EXPR)) ) {
                if( path.is_absolute() ) {
                    handle_path_abs(span, path, mode);
                }
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
            if( this->find_mod_item(span, path, info.name) ) {
                handle_path_abs(span, path, mode);
                break;
            }
            else {
            }
            
            DEBUG("no matches found for path = " << path);
            if( mode != MODE_BIND )
                throw ParseError::Generic("CPathResolver::handle_path - Name resolution failed (Local)");
            return ;
        }
        if(0)
    
    // Unannotated relative
    case AST::Path::Class::Relative:
        handle_path_rel(span, path, mode);
        if(0)
    
    // Module relative
    case AST::Path::Class::Self:
        {
            if( this->find_self_mod_item(span, path, path[0].name()) ) {
                // Fall
            }
            else {
                DEBUG("no matches found for path = " << path);
                if( mode != MODE_BIND )
                    throw ParseError::Generic("CPathResolver::handle_path - Name resolution failed");
            }
        }
        if(0)
    // Parent module relative
    // TODO: "super::" can be chained
    case AST::Path::Class::Super:
        {
            if( this->find_super_mod_item(span, path, path[0].name()) ) {
                // Fall
            }
            else {
                DEBUG("no matches found for path = " << path);
                if( mode != MODE_BIND )
                    throw ParseError::Generic("CPathResolver::handle_path - Name resolution failed");
            }
        }
        // Common for Relative, Self, and Super
        DEBUG("Post relative resolve - path=" << path);
        if( path.is_absolute() ) {
            handle_path_abs(span, path, mode);
        }
        else {
        }
        return ;
    }
    
    // TODO: Are there any reasons not to be bound at this point?
    //assert( !path.binding().is_Unbound() );
}

// Validates all components are able to be located, and converts Trait/Struct/Enum::Item into UFCS format
void CPathResolver::handle_path_abs(const Span& span, AST::Path& path, CASTIterator::PathMode mode)
{
    //bool expect_params = false;

    if( !path.m_class.is_Absolute() ) {
        BUG(span, "Non-absolute path passed to CPathResolver::handle_path_abs - path=" << path);
    }
    
    auto& nodes = path.m_class.as_Absolute().nodes;
    
    unsigned int slice_from = 0;    // Used when rewriting the path to be relative to its crate root
    
    // Iterate through nodes, starting at the root module of the specified crate
    // - Locate the referenced item, and fail if any component isn't found
    ::std::vector<const AST::Module*>    mod_stack;
    const AST::Module* mod = &this->m_crate.get_root_module(path.crate());
    for(unsigned int i = 0; i < nodes.size(); i ++ )
    {
        mod_stack.push_back(mod);
        const bool is_last = (i+1 == nodes.size());
        const AST::PathNode& node = nodes[i];
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
        
        auto item_ref = mod->find_item(node.name(), is_last);  // Only allow leaf nodes (functions and statics) if this is the last node
        TU_MATCH( AST::Module::ItemRef, (item_ref), (item),
        // Not found
        (None,
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
                DEBUG("Failed to locate item in nested, look upwards");
                
                continue ;
            }
            throw ParseError::Generic("Unable to find component '" + node.name() + "'");
            ),
        // Sub-module
        (Module,
            DEBUG("Sub-module : " << node.name());
            if( node.args().size() )
                throw ParseError::Generic("Generic params applied to module");
            mod = &item;
            ),
        // Crate
        (Crate,
            const ::std::string& crate_name = item;
            DEBUG("Extern crate '" << node.name() << "' = '" << crate_name << "'");
            if( node.args().size() )
                throw ParseError::Generic("Generic params applied to extern crate");
            path.set_crate( crate_name );
            slice_from = i+1;
            mod = &this->m_crate.get_root_module(crate_name);
            ),
        
        // Type Alias
        (TypeAlias,
            const auto& ta = item;
            DEBUG("Type alias <"<<ta.params()<<"> " << ta.type());
            //if( node.args().size() != ta.params().size() )
            //    throw ParseError::Generic("Param count mismatch when referencing type alias");
            // Make a copy of the path, replace params with it, then replace *this?
            // - Maybe leave that up to other code?
            if( is_last ) {
                //check_param_counts(ta.params(), expect_params, nodes[i]);
                // TODO: Bind / replace
                // - Replacing requires checking type params (well, at least the count)
                path.bind_type_alias(ta);
                goto ret;
            }
            else {
                this->handle_path_abs__into_ufcs(span, path, slice_from, i+1);
                // Explicit return - rebase slicing is already done
                return ;
            }
            ),
        
        // Function
        (Function,
            const auto& fn = item;
            DEBUG("Found function");
            if( is_last ) {
                //check_param_counts(fn.params(), expect_params, nodes[i]);
                path.bind_function(fn);
                goto ret;
            }
            else {
                throw ParseError::Generic("Import of function, too many extra nodes");
            }
            ),
        
        // Trait
        (Trait,
            const auto& t = item;
            DEBUG("Found trait");
            if( is_last ) {
                //check_param_counts(t.params(), expect_params, nodes[i]);
                path.bind_trait(t);
                goto ret;
            }
            else {
                this->handle_path_abs__into_ufcs(span, path, slice_from, i+1);
                // Explicit return - rebase slicing is already done
                return ;
            }
            ),
        
        // Struct
        (Struct,
            const auto& str = item;
            DEBUG("Found struct");
            if( is_last ) {
                //check_param_counts(str.params(), expect_params, nodes[i]);
                path.bind_struct(str, node.args());
                goto ret;
            }
            else {
                this->handle_path_abs__into_ufcs(span, path, slice_from, i+1);
                // Explicit return - rebase slicing is already done
                return ;
            }
            ),
        
        // Enum / enum variant
        (Enum,
            const auto& enm = item;
            DEBUG("Found enum");
            if( is_last ) {
                //check_param_counts(enm.params(), expect_params, nodes[i]);
                path.bind_enum(enm, node.args());
                goto ret;
            }
            else {
                this->handle_path_abs__into_ufcs(span, path, slice_from, i+1);
                // Explicit return - rebase slicing is already done
                return ;
            }
            ),
        
        (Static,
            const auto& st = item;
            DEBUG("Found static/const");
            if( is_last ) {
                if( node.args().size() )
                    throw ParseError::Generic("Unexpected generic params on static/const");
                path.bind_static(st);
                goto ret;
            }
            else {
                throw ParseError::Generic("Binding path to static, trailing nodes");
            }
            ),
        
        // Re-export
        (Use,
            const auto& imp = item;
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
            this->handle_path_abs(span, newpath, mode);
            
            path = mv$(newpath);
            return;
            )
        )
    }
    
    // We only reach here if the path points to a module
    path.bind_module( *mod );
ret:
    if( slice_from > 0 )
    {
        DEBUG("Removing " << slice_from << " nodes to rebase path to crate root");
        nodes.erase(nodes.begin(), nodes.begin()+slice_from);
    }
    return ;
}
// Starting from the `slice_from`th element, take until `split_point` as the type path, and the rest of the nodes as UFCS items
void CPathResolver::handle_path_abs__into_ufcs(const Span& span, AST::Path& path, unsigned slice_from, unsigned split_point)
{
    TRACE_FUNCTION_F("(path = " << path << ", slice_from=" << slice_from << ", split_point=" << split_point << ")");
    assert(slice_from < path.size());
    assert(slice_from < split_point);
    // Split point must be at most the last index
    assert(split_point < path.size());
    
    const auto& nodes = path.nodes();
    AST::Path   type_path(path.crate(), ::std::vector<AST::PathNode>( nodes.begin() + slice_from, nodes.begin() + split_point ));
    for(unsigned i = split_point; i < nodes.size(); i ++)
    {
        DEBUG("type_path = " << type_path << ", nodes[i] = " << nodes[i]);
        resolve_path(span, m_crate, type_path);
        // If the type path refers to a trait, put it in the trait location
        if(type_path.binding().is_Trait())
            type_path = AST::Path(AST::Path::TagUfcs(), TypeRef(), TypeRef(mv$(type_path)), {mv$(nodes[i])} );
        else
            type_path = AST::Path(AST::Path::TagUfcs(), TypeRef(mv$(type_path)), TypeRef(), {mv$(nodes[i])} );
    }
    
    path = mv$(type_path);
}


/// Handles path resolution for UFCS format paths (<Type as Trait>::Item)
void CPathResolver::handle_path_ufcs(const Span& span, AST::Path& path, CASTIterator::PathMode mode)
{
    assert(path.m_class.is_UFCS());
    auto& info = path.m_class.as_UFCS();
    TRACE_FUNCTION_F("info={< " << *info.type << " as " << *info.trait << ">::" << info.nodes << "}");
    // 1. Handle sub-types
    handle_type(*info.type);
    handle_type(*info.trait);
    
    // - Some quick assertions
    if( info.type->is_path() )
    {
        TU_MATCH_DEF(AST::PathBinding, (info.type->path().binding()), (i),
        (
            // Invalid
            BUG(span, "Invalid item class for type in path " << path);
            ),
        //(Unbound,
        //    ),
        //(Trait, ),
        (Struct, ),
        (Enum, )
        )
    }

    // 2. Handle wildcard traits (locate in inherent impl, or from an in-scope trait)
    // TODO: Disabled, as it requires having all traits (semi) path resolved, so that trait resolution works cleanly
    // - Impl heads and trait heads could be resolved in an earlier pass
    if( info.trait->is_wildcard() )
    {
        #if 0
        if( this->m_second_pass ) {
            const ::std::string&    item_name = info.nodes[0].name();
            DEBUG("Searching for matching trait for '"<<item_name<<"' on type " << *info.type);
            
            // Search applicable type parameters for known implementations
            
            // 1. Inherent
            //AST::Impl*  impl_ptr;
            ::std::vector<TypeRef> params;
            if( info.type->is_type_param() && info.type->type_param() == "Self" )
            {
                DEBUG("Checking Self trait and sub-traits");
                // TODO: What is "Self" here? May want to use `GenericBound`s to replace Self with the actual type when possible.
                //       In which case, Self will refer to "implementor of this trait".
                // - Look up applicable traits for this type, using bounds (basically same as next)
                assert( !m_self_type.empty() );
                assert( m_self_type.back().is_Trait() );
                AST::Path p = m_self_type.back().as_Trait().path;
                handle_path(p, MODE_TYPE);
                AST::Trait& t = *const_cast<AST::Trait*>(m_self_type.back().as_Trait().trait);
                
                bool is_method;
                AST::Path   found_trait_path;
                if( this->find_trait_item(span, p, t, item_name,  is_method, found_trait_path) ) {
                    if( is_method ) {
                        if( info.nodes.size() != 1 )
                            ERROR(path.span(), E0000, "CPathResolver::handle_path_ufcs - Sub-nodes to method");
                    }
                    else {
                        if( info.nodes.size() != 1 )
                            throw ParseError::Todo("CPathResolver::handle_path_ufcs - Sub nodes on associated type");
                    }
                    *info.trait = TypeRef( mv$(found_trait_path) );
                }
                else {
                    ERROR(path.span(), E0000, "Cannot find item '" << item_name << "' on Self");
                }
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
                            if( this->find_trait_item(span, ent.trait, t, item_name,  is_method, found_trait_path) )
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
                DEBUG("(maybe) known type");
                // Iterate all inherent impls
                for( auto impl : m_crate.find_inherent_impls(*info.type) ) {
                    IF_OPTION_SOME(item, impl.find_named_item(item_name), {
                        DEBUG("Found matching inherent impl");
                        *info.trait = TypeRef(TypeRef::TagInvalid());
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
                            return ;
                        })
                    }
                }

                throw ParseError::Todo( FMT("CPathResolver::handle_path_ufcs - UFCS, find trait, for type " << *info.type) );
            }
        }
        #endif
    }
    else {
        #if 0
        const auto& name = path.nodes()[0].name();
        // Trait is known, need to ensure that the named item exists
        assert(info.trait->path().binding().is_Trait());
        const auto &tr = *info.trait->path().binding().as_Trait().trait_;
        
        // TODO: Need to try super-traits AND applicable type impls
        // - E.g. "impl Any {"
        switch(mode)
        {
        case MODE_EXPR:
            for( const auto& fcn : tr.functions() )
            {
                DEBUG("fcn.name = " << fcn.name);
                if(fcn.name == name) {
                    path.bind_function(fcn.data);
                    break;
                }
            }
            //for( const auto& fcn : tr.constants() )
            //{
            //}
            break;
        case MODE_BIND:
            //for( const auto& fcn : tr.constants() )
            //{
            //}
            break;
        case MODE_TYPE:
            for( const auto& it : tr.types() )
            {
                if(it.name == name) {
                    path.bind_type_alias(it.data);
                    break;
                }
            }
            break;
        }
        if(mode == MODE_EXPR && path.binding().is_Unbound()) {
            // TODO: Locate an 'impl Trait' block for methods
        }
        if(path.binding().is_Unbound()) {
            ERROR(span, E0000, "Unable to locate item '" << name << "' in trait " << *info.trait << " (mode=" << mode << ")");
        }
        #endif
    }
}
void CPathResolver::handle_path_rel(const Span& span, AST::Path& path, CASTIterator::PathMode mode)
{
    // 1. function scopes (variables and local items)
    // > Return values: name or path
    {
        bool allow_variables = (mode == CASTIterator::MODE_EXPR && path.is_trivial());
        if( this->find_local_item(span, path, path[0].name(), allow_variables) ) {
            return ;
        }
    }
    
    // 2. Type parameters
    // - Should probably check if this is expression mode, bare types are invalid there
    // NOTES:
    // - If the path is bare (i.e. there are no more nodes), then ensure that the mode is TYPE
    // - If there are more nodes, replace with a UFCS block
    {
        auto tp = this->find_type_param(path[0].name());
        if( tp != false /*nullptr*/ || path[0].name() == "Self" )
        {
            if(path.size() > 1) {
                auto ty = TypeRef(TypeRef::TagArg(), path[0].name());
                //ty.set_span( path.span() );
                // Repalce with UFCS
                auto newpath = AST::Path(AST::Path::TagUfcs(), ty, TypeRef());
                newpath.add_tailing(path);
                path = mv$(newpath);
            }
            else {
                // Mark as local
                // - TODO: Not being trivial is an error, not a bug
                assert( path.is_trivial() );
                path = AST::Path(AST::Path::TagLocal(), path[0].name());
                // - TODO: Need to bind this to the source parameter block
            }
            return ;
        }
    }
    
    // 3. current module
    {
        if( this->find_mod_item(span, path, path[0].name()) ) {
            return ;
        }
        else {
        }
    }
    
    // 4. If there was no match above, and we're in bind mode, set the path to local and return
    DEBUG("no matches found for path = " << path);
    if( mode == MODE_BIND )
    {
        // If mode == MODE_BIND, must be trivial
        if( !path.is_trivial() )
            throw ParseError::Generic("CPathResolver::handle_path - Name resolution failed (non-trivial path failed to resolve in MODE_BIND)");
        path = AST::Path(AST::Path::TagLocal(), path[0].name());
        return ;
    }
    
    // 5. Otherwise, error
    ERROR(span, E0000, "CPathResolver::handle_path - Name resolution failed");
}

bool CPathResolver::find_trait_item(const Span& span, const AST::Path& path, AST::Trait& trait, const ::std::string& item_name, bool& out_is_method, AST::Path& out_trait_path)
{
    TRACE_FUNCTION_F("path=" << path << ", trait=..., item_name=" << item_name);
    {
        const auto& fcns = trait.functions();
        //DEBUG("fcns = " << fcns);
        auto it = ::std::find_if( fcns.begin(), fcns.end(), [&](const AST::Item<AST::Function>& a) { DEBUG("fcn " << a.name); return a.name == item_name; } );
        if( it != fcns.end() ) {
            // Found it.
            out_is_method = true;
            out_trait_path = AST::Path(path);
            DEBUG("Fcn, out_trait_path = " << out_trait_path);
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
            DEBUG("Ty, out_trait_path = " << out_trait_path << "  path=" << path);
            return true;
        }
    }
    
    for( auto& st : trait.supertraits() ) {
        if(!st.is_bound()) {
            handle_path(st, MODE_TYPE);
            //BUG(st.span(), "Supertrait path '"<<st<<"' of '"<<path<<"' is not bound");
        }
        AST::Trait& super_t = *const_cast<AST::Trait*>(st.binding().as_Trait().trait_);
        if( this->find_trait_item(span, st, super_t, item_name,  out_is_method, out_trait_path) ) {
            DEBUG("path = " << path << ", super_t.params() = " << super_t.params() << ", out_trait_path = " << out_trait_path);
            // 
            out_trait_path.resolve_args([&](const char* name) {
                int idx = trait.params().find_name(name);
                if(idx < 0)
                    ERROR(st.span(), E0000, "Parameter " << name << " not found");
                const auto& tr = path.nodes().back().args().at(idx);
                DEBUG("Replacing '" << name << "' with " << tr);
                return tr;
                });
            return true;
        }
    }
    
    return false;
}

bool CPathResolver::find_local_item(const Span& span, AST::Path& path, const ::std::string& name, bool allow_variables)
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
            if( lookup_path_in_module(span, m_crate, *s.module, s.module_path, path, name, path.is_trivial()) )
                return true;
        }
    }
    DEBUG("- find_local_item: Not found");
    return false;
}
bool CPathResolver::find_mod_item(const Span& span, AST::Path& path, const ::std::string& name) {
    const AST::Module* mod = m_module;
    do {
        if( lookup_path_in_module(span, m_crate, *mod, m_module_path, path, name, path.size()==1) )
            return true;
        if( mod->name() == "" )
            throw ParseError::Todo("Handle anon modules when resoling unqualified relative paths");
    } while( mod->name() == "" );
    return false;
}
bool CPathResolver::find_self_mod_item(const Span& span, AST::Path& path, const ::std::string& name) {
    if( m_module->name() == "" )
        throw ParseError::Todo("Correct handling of 'self' in anon modules");
    
    return lookup_path_in_module(span, m_crate, *m_module, m_module_path, path, name, path.size()==1);
}
bool CPathResolver::find_super_mod_item(const Span& span, AST::Path& path, const ::std::string& name) {
    if( m_module->name() == "" )
        throw ParseError::Todo("Correct handling of 'super' in anon modules");
    
    // 1. Construct path to parent module
    AST::Path   super_path = m_module_path;
    super_path.nodes().pop_back();
    assert( super_path.nodes().size() > 0 );
    if( super_path.nodes().back().name()[0] == '#' )
        throw ParseError::Todo("Correct handling of 'super' in anon modules (parent is anon)");
    // 2. Resolve that path
    resolve_path(span, m_crate, super_path);
    // 3. Call lookup_path_in_module
    assert( super_path.binding().is_Module() );
    return lookup_path_in_module(span, m_crate, *super_path.binding().as_Module().module_, super_path, path,  name, path.size()==1);
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
            DEBUG("Found local '" << name << "' aliasing to " << opt_local.unwrap().tr);
            type = opt_local.unwrap().tr;
        }
        else if( name == "Self" )
        {
            // If the name was "Self", but Self isn't already defined... then we need to make it an arg?
            if( this->m_self_type.empty() || this->m_self_type.back().is_None() ) {
                ERROR(type.path().span(), E0000, "Unexpected 'Self'");
            }
            else {
                TU_MATCH(SelfType, (this->m_self_type.back()), (ent),
                (None,
                    assert(!"SelfType is None in CPathResolver::handle_type");
                    ),
                (Type,
                    DEBUG("Self type is " << ent.type);
                    type = ent.type;
                    return ;
                    ),
                (Trait,
                    // TODO: Need to have the trait encoded in the type. To avoid interpolation and bad replacement
                    // - Would also reduce the need to look at the m_self_type stack
                    type = TypeRef(TypeRef::TagArg(), "Self");
                    )
                )
            }
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
        if( name == "Self" )
        {
            if( m_self_type.empty() ) {
                ERROR(type.span(), E0000, "Self type not set");
            }
            TU_MATCH(SelfType, (m_self_type.back()), (ent),
            (None,
                ERROR(type.span(), E0000, "Self type not set");
                ),
            (Type,
                DEBUG("Self type is " << ent.type);
                type = ent.type;
                return ;
                ),
            (Trait,
                // Valid...
                )
            )
        }
        else if( opt_local.is_some() )
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
        if( newpath.is_relative() || newpath.is_trivial() )
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
    //path.resolve(m_crate);
    
    // Handle local 
    m_scope_stack.back().traits.push_back( ::std::pair<AST::Path, const AST::Trait&>(path, trait) );
    CASTIterator::handle_trait(path, trait);
}
void CPathResolver::handle_function(AST::Path path, AST::Function& fcn)
{
    m_scope_stack.push_back( {0, nullptr, AST::Path(), {}} );
    CASTIterator::handle_function(::std::move(path), fcn);
    m_scope_stack.pop_back();
}


void absolutise_path(const Span& span, const AST::Crate& crate, const AST::Path& modpath, AST::Path& path)
{
    // TODO: Should this code resolve encountered use statements into the real path?
    TU_MATCH(AST::Path::Class, (path.m_class), (info),
    (Absolute,
        // Nothing needs to be done
        ),
    (Super,
        // TODO: Super supports multiple instances
        auto newpath = modpath;
        newpath.nodes().pop_back();
        newpath += path;
        DEBUG("Absolutised path " << path << " into " << newpath);
        path = ::std::move(newpath);
        ),
    (Self,
        auto modpath_tmp = modpath;
        const AST::Module* mod_ptr = &mod;
        while( modpath_tmp.size() > 0 && modpath_tmp.nodes().back().name()[0] == '#' )
        {
            if( !mod_ptr->find_item(path.nodes()[0].name()).is_None() ) {
                break ;
            }
            modpath_tmp.nodes().pop_back();
            resolve_path(span, crate, modpath_tmp);
            DEBUG("modpath_tmp = " << modpath_tmp);
            assert( modpath_tmp.binding().is_Module() );
            mod_ptr = modpath_tmp.binding().as_Module().module_;
        }
        auto newpath = modpath_tmp + path;
        DEBUG("Absolutised path " << path << " into " << newpath);
        path = ::std::move(newpath);
        )
    (Relative,
        auto newpath = modpath + path;
        DEBUG("Absolutised path " << path << " into " << newpath);
        path = ::std::move(newpath);
        ),
    (UFCS,
        throw ParseError::Generic( FMT("Invalid path type encounted - UFCS " << path) );
        )
    )
}

void ResolvePaths_HandleModule_Use(const AST::Crate& crate, const AST::Path& modpath, AST::Module& mod)
{
    TRACE_FUNCTION_F("modpath = " << modpath);
    ::std::vector<AST::Path>    new_imports;
    for( auto& imp : mod.imports() )
    {
        const Span  span = Span();
        DEBUG("p = " << imp.data);
        absolutise_path(span, crate, modpath, imp.data);
        
        resolve_path(span, crate, imp.data);
        DEBUG("Resolved import : " << imp.data);
        
        // If wildcard, make sure it's sane
        if( imp.name == "" )
        {
            TU_MATCH_DEF(AST::PathBinding, (imp.data.binding()), (info),
            (
                throw ParseError::Generic("Wildcard imports are only allowed on modules and enums");
                ),
            (Unbound,
                throw ParseError::BugCheck("Wildcard import path unbound after calling .resolve()");
                ),
            (Module, (void)0;),
            (Enum, (void)0;)
            )
        }
    }
    
    for( auto& new_imp : new_imports )
    {
        //if( new_imp.binding().is_Unbound() ) {
        //    new_imp.resolve(crate, false);
        //}
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
        //type.path().resolve(crate);
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
        //imp.data.resolve(crate, false);
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

    // Resolve traits next, to ensure they're usable
    //ResolvePaths_HandleModule_Trait(crate, AST::Path("", {}), crate.root_module());
    // Resolve impl block headers
    //ResolvePaths_HandleModule_Impl(crate, AST::Path("", {}), crate.root_module());
    
    // Then do path resolution on all other items
    CPathResolver pr(crate);
    DEBUG(" ---");
    pr.handle_module(AST::Path("", {}), crate.root_module());
    DEBUG(" <<<");

    pr.m_second_pass = true;
    DEBUG(" ---");
    pr.handle_module(AST::Path("", {}), crate.root_module());
    DEBUG(" <<<");
}
