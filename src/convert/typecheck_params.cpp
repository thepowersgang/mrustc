/*
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
    ::std::vector<const AST::TypeParams*>   m_params_stack;
public:
    virtual void handle_path(AST::Path& path, CASTIterator::PathMode pm) override;
    virtual void handle_expr(AST::ExprNode& root) override;
    virtual void start_scope() override;
    virtual void end_scope() override;
    virtual void handle_params(AST::TypeParams& params) override;
    
private:
    bool has_impl_for_param(const ::std::string name, const TypeRef& trait) const;
    bool has_impl(const TypeRef& type, const TypeRef& trait) const;
    void check_generic_params(const AST::TypeParams& info, ::std::vector<TypeRef>& types, bool allow_infer = false);
};

class CNodeVisitor:
    public AST::NodeVisitor
{
    CGenericParamChecker&   m_pc;
public:
    CNodeVisitor(CGenericParamChecker& pc):
        m_pc(pc)
    {}
};

// === CODE ===
bool CGenericParamChecker::has_impl_for_param(const ::std::string name, const TypeRef& trait) const
{
    const AST::TypeParams*  tps = nullptr;
    // Locate params set that contains the passed name
    for( const auto ptr : m_params_stack )
    {
        if( ptr )
        {
            for( const auto& p : ptr->params() )
            {
                if(p.name() == name) {
                    tps = ptr;
                    break ;
                }
            }
        }
    }
    
    if( !tps )
    {
        throw ::std::runtime_error(FMT("Param '"<<name<<"' isn't in scope"));
    }
    
    // Search bound list for the passed trait
    for( const auto& bound : tps->bounds() )
    {
        if( bound.is_trait() && bound.name() == name )
        {
            DEBUG("bound.type() {" << bound.type() << "} == trait {" << trait << "}");
            if( bound.type() == trait )
                return true;
        }
    }
    
    // TODO: Search for generic ("impl<T: Trait2> Trait1 for T") that fits bounds
    
    DEBUG("No match in generics, returning failure");
    return false;
}
bool CGenericParamChecker::has_impl(const TypeRef& type, const TypeRef& trait) const
{
    DEBUG("(type = " << type << ", trait = " << trait << ")");
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
        throw ::std::runtime_error( FMT("TODO: Search for impls on " << type << " for the trait") );
    }
    return false;
}

/// Check that the passed set of parameters match the requiremens for a generic item
///
/// \param info Generic item information (param names and bounds)
/// \param types Type parameters being passed to the generic item
/// \param allow_infer  Allow inferrence (mutates \a types with conditions from \a info)
void CGenericParamChecker::check_generic_params(const AST::TypeParams& info, ::std::vector<TypeRef>& types, bool allow_infer)
{
    DEBUG("(info = " << info << ", types = {" << types << "}");
    // TODO: Need to correctly handle lifetime params here, they should be in a different list
    const auto& params = info.params();
    
    {
        for(const auto& p : params)
            assert(p.is_type());
    }
    
    if( types.size() > params.size() )
    {
        throw ::std::runtime_error(FMT("Too many generic params ("<<types.size()<<" passed, expecting "<< params.size()<<")"));
    }
    else if( types.size() < params.size() )
    {
        if( allow_infer )
        {
            while( types.size() < params.size() )
            {
                types.push_back( TypeRef() );
            }
        }
        else
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
        if( type.is_wildcard() )
        {
            // Type is a wildcard - this can match any condition
            if( allow_infer )
            {
                // Apply conditions for this param to the type
                // TODO: Requires supporting type "ranges" on the TypeRef
                // Should also check for conflicting requirements (negative bounds?)
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
            for( const auto& bound : info.bounds() )
            {
                if( bound.is_trait() && bound.name() == param )
                {
                    const auto& trait = bound.type();
                    // Check if 'type' impls 'trait'
                    if( !has_impl(type, trait) )
                    {
                        throw ::std::runtime_error( FMT("No matching impl of "<<trait<<" for "<<type));
                    }
                }
            }
        }
    }
}

void CGenericParamChecker::handle_path(AST::Path& path, CASTIterator::PathMode pm)
{
    DEBUG("path = " << path);
    AST::PathNode& last_node = path[path.size()-1];
    const AST::TypeParams* params = nullptr;
    switch(path.binding_type())
    {
    case AST::Path::MODULE:
        DEBUG("WTF - Module path, isn't this invalid at this stage?");
        break;
    case AST::Path::TRAIT:
        params = &path.bound_trait().params();
        if(0)
    case AST::Path::STRUCT:
        params = &path.bound_struct().params();
        if(0)
    case AST::Path::ENUM:
        params = &path.bound_enum().params();
        if(0)
    case AST::Path::FUNCTION:
        params = &path.bound_func().params();
        
        try {
            check_generic_params(*params, last_node.args(), (m_within_expr > 0));
        }
        catch( const ::std::exception& e )
        {
            throw ::std::runtime_error( FMT("Checking '" << path << "', threw : " << e.what()) );
        }
        break;
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
    m_params_stack.push_back(nullptr);
}
void CGenericParamChecker::end_scope()
{
    assert( m_params_stack.size() > 0 );
    while( m_params_stack.back() != nullptr )
        m_params_stack.pop_back();
}
void CGenericParamChecker::handle_params(AST::TypeParams& params)
{
    m_params_stack.push_back( &params );
}

/// Typecheck generic parameters (ensure that they match all generic bounds)
void Typecheck_GenericParams(AST::Crate& crate)
{
    DEBUG(" >>> ");
    CGenericParamChecker    chk;
    chk.handle_module(AST::Path({}), crate.root_module());
    DEBUG(" <<< ");
}

