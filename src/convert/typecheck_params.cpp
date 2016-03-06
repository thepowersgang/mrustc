/*
/// Typecheck generic parameters (ensure that they match all generic bounds)
 */
#include <main_bindings.hpp>
#include "ast_iterate.hpp"
#include "../ast/crate.hpp"
#include "../common.hpp"
#include <stdexcept>

// === PROTOTYPES ===
class CGenericParamChecker:
    public CASTIterator
{
     int    m_within_expr = 0;
    struct LocalType {
        const ::std::string name;
        const AST::GenericParams*  source_params;  // if nullptr, use fixed_type
        TypeRef fixed_type;
    
        LocalType():
            name("")
        {}
        LocalType(const ::std::string& n, const AST::GenericParams* tps):
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
    virtual void handle_params(AST::GenericParams& params) override;
    
private:
    bool has_impl_for_param(const ::std::string name, const AST::Path& trait) const;
    bool has_impl(const TypeRef& type, const AST::Path& trait) const;
    void check_generic_params(const AST::GenericParams& info, ::std::vector<TypeRef>& types, TypeRef self_type, bool allow_infer = false);
    
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
    
    if( m_crate.is_trait_implicit(trait) )
        return true;
    
    const AST::GenericParams*  tps = nullptr;
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
        TU_IFLET( AST::GenericBound, bound, IsTrait, ent,
            if( ent.type.is_type_param() && ent.type.type_param() == name )
            {
                DEBUG("bound.type() {" << ent.trait << "} == trait {" << trait << "}");
                if( ent.trait == trait )
                    return true;
            }
            )
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
        {
            return true;
        }
    }
    else
    {
        // Search all known impls of this trait (TODO: keep a list at the crate level) for a match to this type
        if( m_crate.find_impl(trait, type, nullptr, nullptr) == false ) {
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
void CGenericParamChecker::check_generic_params(const AST::GenericParams& info, ::std::vector<TypeRef>& types, TypeRef self_type, bool allow_infer)
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
        TU_IFLET(AST::GenericBound, bound, IsTrait, ent,
            auto ra_fcn = [&](const char *a){
                if( strcmp(a, "Self") == 0 ) {
                    if( self_type == TypeRef(TypeRef::TagInvalid()) )
                        throw CompileError::Generic("Unexpected use of 'Self' in bounds");
                    return self_type;
                }
                return types.at(info.find_name(a));
                };
            auto bound_type = ent.type;
            bound_type.resolve_args(ra_fcn);
            
            auto trait = ent.trait;
            trait.resolve_args(ra_fcn);
            
            // Check if 'type' impls 'trait'
            if( !has_impl(bound_type, trait) )
            {
                throw ::std::runtime_error( FMT("No matching impl of "<<trait<<" for "<<bound_type));
            }
        )
    }
}

void CGenericParamChecker::handle_path(AST::Path& path, CASTIterator::PathMode pm)
{
    TRACE_FUNCTION_F("path = " << path);
    AST::PathNode& last_node = path[path.size()-1];
    
    auto comm = [&](const AST::GenericParams& params) {
            auto lt = find_type_by_name("Self");
            TypeRef self_type;  // =TypeRef(TypeRef::TagPath(), path)
            if( lt )
                self_type = lt->fixed_type;
            check_generic_params(params, last_node.args(), self_type, (m_within_expr > 0));
        };
    
    TU_MATCH( AST::PathBinding, (path.binding()), (info),
    (Unbound,
        throw CompileError::BugCheck( FMT("CGenericParamChecker::handle_path - Unbound path : " << path) );
        ),
    (Module,
        DEBUG("WTF - Module path, isn't this invalid at this stage?");
        ),
    (Trait,
        comm( info.trait_->params() );
        ),
    (Struct,
        comm( info.struct_->params() );
        ),
    (Enum,
        comm( info.enum_->params() );
        ),
    (TypeAlias,
        comm( info.alias_->params() );
        ),
    (Function,
        check_generic_params(info.func_->params(), last_node.args(), TypeRef(TypeRef::TagInvalid()), (m_within_expr > 0));
        ),
    
    (EnumVar,
        throw ::std::runtime_error("TODO: handle_path EnumVar");
        ),
    (Static,
        throw ::std::runtime_error("TODO: handle_path Static");
        ),
    (StructMethod,
        throw ::std::runtime_error("TODO: handle_path StructMethod");
        ),
    (TraitMethod,
        throw ::std::runtime_error("TODO: handle_path TraitMethod");
        ),
    (TypeParameter,
        throw ::std::runtime_error("TODO: handle_path TypeParameter");
        ),
    (Variable,
        throw ::std::runtime_error("TODO: handle_path Variable");
        )
    )
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
void CGenericParamChecker::handle_params(AST::GenericParams& params)
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
    chk.handle_module(AST::Path("", {}), crate.root_module());
    DEBUG(" <<< ");
}

