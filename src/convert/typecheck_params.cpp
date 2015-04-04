/*
/// Typecheck generic parameters (ensure that they match all generic bounds)
 */
#include <main_bindings.hpp>
#include "ast_iterate.hpp"
#include "../common.hpp"
#include <stdexcept>

// === PROTOTYPES ===
class CGenericParamChecker:
    public CASTIterator
{
     int    m_within_expr = 0;
    struct LocalType {
        const ::std::string name;
        const AST::TypeParams*  source_params;  // if nullptr, use fixed_type
        TypeRef fixed_type;
    
        LocalType():
            name("")
        {}
        LocalType(const ::std::string& n, const AST::TypeParams* tps):
            name(n), source_params(tps)
        {}
        LocalType(const ::std::string& n, TypeRef ty):
            name(n), source_params(nullptr), fixed_type( ::std::move(ty) )
        {}
    
        bool is_separator() const { return name == ""; }
    };
    // name == "" indicates edge of a scope
    ::std::vector<LocalType>    m_types_stack;
    
    AST::Crate& m_crate;
public:
    CGenericParamChecker(AST::Crate& c):
        m_crate(c)
    {
    }
    
    virtual void handle_path(AST::Path& path, CASTIterator::PathMode pm) override;
    virtual void handle_expr(AST::ExprNode& root) override;
    void start_scope() override;
    void local_type(::std::string name, TypeRef type) override;
    void end_scope() override;
    virtual void handle_params(AST::TypeParams& params) override;
    
private:
    bool has_impl_for_param(const ::std::string name, const AST::Path& trait) const;
    bool has_impl(const TypeRef& type, const AST::Path& trait) const;
    void check_generic_params(const AST::TypeParams& info, ::std::vector<TypeRef>& types, TypeRef self_type, bool allow_infer = false);
    
    const LocalType* find_type_by_name(const ::std::string& name) const;
};

class CNodeVisitor:
    public AST::NodeVisitorDef
{
    CGenericParamChecker&   m_pc;
public:
    CNodeVisitor(CGenericParamChecker& pc):
        m_pc(pc)
    {}
};

// === CODE ===
bool CGenericParamChecker::has_impl_for_param(const ::std::string name, const AST::Path& trait) const
{
    TRACE_FUNCTION_F("name = " << name << ", trait = " << trait);
    const AST::TypeParams*  tps = nullptr;
    // Locate params set that contains the passed name
    for( const auto lt : m_types_stack )
    {
        if( lt.name == name )
        {
            if( lt.source_params != nullptr ) {
                tps = lt.source_params;
            }
            else {
                DEBUG("Type name '" << name << "' isn't a param");
                return has_impl(lt.fixed_type, trait);
            }
            //break ;
        }
    }
    
    if( !tps )
    {
        throw ::std::runtime_error(FMT("Param '"<<name<<"' isn't in scope"));
    }
    
    // Search bound list for the passed trait
    for( const auto& bound : tps->bounds() )
    {
        DEBUG("bound = " << bound);
        if( bound.is_trait() && bound.test().is_type_param() && bound.test().type_param() == name )
        {
            DEBUG("bound.type() {" << bound.bound() << "} == trait {" << trait << "}");
            if( bound.bound() == trait )
                return true;
        }
    }
    
    // TODO: Search for generic ("impl<T: Trait2> Trait1 for T") that fits bounds
    
    DEBUG("No match in generics, returning failure");
    return false;
}

bool CGenericParamChecker::has_impl(const TypeRef& type, const AST::Path& trait) const
{
    TRACE_FUNCTION_F("type = " << type << ", trait = " << trait);
    if( type.is_type_param() )
    {
        // TODO: Search current scope (requires access to CGenericParamChecker) for this type,
        // and search the bounds for this trait
        // - Also accept bounded generic impls (somehow)
        if( has_impl_for_param(type.type_param(), trait) )
        {
            return true;
        }
    }
    else
    {
        // Search all known impls of this trait (TODO: keep a list at the crate level) for a match to this type
        auto i = m_crate.find_impl(trait, type);
        if( i.is_none() ) {
            DEBUG("- Nope");
            return false;
        }
        
        return true;
    }
    return false;
}

/// Check that the passed set of parameters match the requiremens for a generic item
///
/// \param info Generic item information (param names and bounds)
/// \param types Type parameters being passed to the generic item
/// \param allow_infer  Allow inferrence (mutates \a types with conditions from \a info)
void CGenericParamChecker::check_generic_params(const AST::TypeParams& info, ::std::vector<TypeRef>& types, TypeRef self_type, bool allow_infer)
{
    TRACE_FUNCTION_F("info = " << info << ", types = {" << types << "}");
    // TODO: Need to correctly handle lifetime params here, they should be in a different list
    const auto& params = info.ty_params();
    
    if( types.size() > params.size() )
    {
        throw ::std::runtime_error(FMT("Too many generic params ("<<types.size()<<" passed, expecting "<< params.size()<<")"));
    }
    else if( types.size() < params.size() )
    {
        // Fill with defaults
        while( types.size() < params.size() && params[types.size()].get_default() != TypeRef() )
            types.push_back( params[types.size()].get_default() );
        
        // And (optionally) infer
        if( allow_infer )
        {
            while( types.size() < params.size() )
            {
                types.push_back( TypeRef() );
            }
        }
        else if( types.size() < params.size() )
        {
            throw ::std::runtime_error(FMT("Too few generic params, ("<<types.size()<<" passed, expecting "<<params.size()<<")"));
        }
    }
    else
    {
        // Counts are good, time to validate types
    }
   
    for( unsigned int i = 0; i < types.size(); i ++ )
    {
        auto& type = types[i];
        auto& param = params[i].name();
        DEBUG("#" << i << " - checking type " << type << " against " << param); 
        if( type.is_wildcard() )
        {
            // Type is a wildcard - this can match any condition
            if( allow_infer )
            {
                // Apply conditions for this param to the type
                // TODO: Requires supporting type "ranges" on the TypeRef
                // Should also check for conflicting requirements (negative bounds?)
                DEBUG("TODO: Type inferrence");
            }
            else
            {
                // This is an error, as inferrence is not currently allowed
                throw ::std::runtime_error(FMT("Type of '_' present for param " << param << " when inferrence isn't allowed"));
            }
        }
        else
        {
            // Not a wildcard!
            // Check that the type fits the bounds applied to it
            TypeRef param_type(TypeRef::TagArg(), param);
        }
    }
    
    for( const auto& bound : info.bounds() )
    {
        if( bound.is_trait() )
        {
            auto ra_fcn = [&](const char *a){
                if( strcmp(a, "Self") == 0 ) {
                    if( self_type == TypeRef() )
                        throw CompileError::Generic("Unexpected use of 'Self' in bounds");
                    return self_type;
                }
                return types.at(info.find_name(a));
                };
            auto bound_type = bound.test();
            bound_type.resolve_args(ra_fcn);
            
            auto trait = bound.bound();
            trait.resolve_args(ra_fcn);
            
            // Check if 'type' impls 'trait'
            if( !has_impl(bound_type, trait) )
            {
                throw ::std::runtime_error( FMT("No matching impl of "<<trait<<" for "<<bound_type));
            }
        }
    }
}

void CGenericParamChecker::handle_path(AST::Path& path, CASTIterator::PathMode pm)
{
    TRACE_FUNCTION_F("path = " << path);
    AST::PathNode& last_node = path[path.size()-1];
    const AST::TypeParams* params = nullptr;
    switch(path.binding().type())
    {
    case AST::PathBinding::UNBOUND:
        throw ::std::runtime_error("CGenericParamChecker::handle_path - Unbound path");
    case AST::PathBinding::MODULE:
        DEBUG("WTF - Module path, isn't this invalid at this stage?");
        break;
    
    case AST::PathBinding::TRAIT:
        params = &path.binding().bound_trait().params();
        if(0)
    case AST::PathBinding::STRUCT:
        params = &path.binding().bound_struct().params();
        if(0)
    case AST::PathBinding::ENUM:
        params = &path.binding().bound_enum().params();
        
        {
            auto lt = find_type_by_name("Self");
            TypeRef self_type;  // =TypeRef(TypeRef::TagPath(), path)
            if( lt )
                self_type = lt->fixed_type;
            check_generic_params(*params, last_node.args(), self_type, (m_within_expr > 0));
        }
        break;
    case AST::PathBinding::ALIAS:
        params = &path.binding().bound_alias().params();
        if(0)
    case AST::PathBinding::FUNCTION:
        params = &path.binding().bound_func().params();
        
        check_generic_params(*params, last_node.args(), TypeRef(), (m_within_expr > 0));
        break;
    default:
        throw ::std::runtime_error("Unknown path type in CGenericParamChecker::handle_path");
    }
}

void CGenericParamChecker::handle_expr(AST::ExprNode& root)
{
    m_within_expr += 1;
    // Do nothing (this iterator shouldn't recurse into expressions)
    CNodeVisitor    nv(*this);
    root.visit(nv);
    m_within_expr -= 1;
}

void CGenericParamChecker::start_scope()
{
    m_types_stack.push_back( LocalType() );
}
void CGenericParamChecker::local_type(::std::string name, TypeRef type)
{
    DEBUG("name = " << name << ", type = " << type);
    m_types_stack.push_back( LocalType(::std::move(name), ::std::move(type)) );
}
void CGenericParamChecker::end_scope()
{
    assert( m_types_stack.size() > 0 );
    while( !m_types_stack.back().is_separator() )
        m_types_stack.pop_back();
    // pop the separator
    m_types_stack.pop_back();
}
void CGenericParamChecker::handle_params(AST::TypeParams& params)
{
    DEBUG("params = " << params);
    for( const auto& p : params.ty_params())
        m_types_stack.push_back( LocalType(p.name(), &params) );
}

const CGenericParamChecker::LocalType* CGenericParamChecker::find_type_by_name(const ::std::string& name) const
{
    for( unsigned int i = m_types_stack.size(); i --; )
    {
        if( m_types_stack[i].name == name )
            return &m_types_stack[i];
    }
    return nullptr;
}

/// Typecheck generic parameters (ensure that they match all generic bounds)
void Typecheck_GenericParams(AST::Crate& crate)
{
    DEBUG(" >>> ");
    CGenericParamChecker    chk(crate);
    chk.handle_module(AST::Path({}), crate.root_module());
    DEBUG(" <<< ");
}

