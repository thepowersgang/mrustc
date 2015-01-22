/*
 */
#include <main_bindings.hpp>
#include "ast_iterate.hpp"
#include "../common.hpp"
#include <stdexcept>

// === PROTOTYPES ===
class CTypeChecker:
    public CASTIterator
{
    friend class CTC_NodeVisitor;
    
    struct Scope {
        ::std::vector< ::std::tuple<bool, ::std::string, TypeRef> >   vars;
        ::std::vector< ::std::tuple< ::std::string, TypeRef> >  types;
        ::std::map< ::std::string, TypeRef >    params;
    };
    
    AST::Crate& m_crate;
    ::std::vector<Scope>    m_scopes;
    
public:
    CTypeChecker(AST::Crate& crate):
        m_crate(crate)
    {}
    
    virtual void start_scope() override;
    virtual void local_variable(bool is_mut, ::std::string name, const TypeRef& type) override;
    virtual void local_type(::std::string name, TypeRef type) override;
    virtual void end_scope() override;
    
    virtual void handle_params(AST::TypeParams& params) override;
    
    virtual void handle_function(AST::Path path, AST::Function& fcn) override;
    // - Ignore all non-function items on this pass
    virtual void handle_enum(AST::Path path, AST::Enum& ) override {}
    virtual void handle_struct(AST::Path path, AST::Struct& str) override {}
    virtual void handle_alias(AST::Path path, AST::TypeAlias& ) override {}

private:
    TypeRef& get_local_var(const char* name);
    const TypeRef& get_local_type(const char* name);
    const TypeRef& get_type_param(const char* name);
    void lookup_method(const TypeRef& type, const char* name);
};
class CTC_NodeVisitor:
    public AST::NodeVisitor
{
    CTypeChecker&   m_tc;
public:
    CTC_NodeVisitor(CTypeChecker& tc):
        m_tc(tc)
    {}
    
    virtual void visit(AST::ExprNode_NamedValue& node) override;
    
    virtual void visit(AST::ExprNode_LetBinding& node) override;
    virtual void visit(AST::ExprNode_Assign& node) override;
    
    virtual void visit(AST::ExprNode_Match& node) override;
    
    virtual void visit(AST::ExprNode_Field& node) override;
    virtual void visit(AST::ExprNode_Cast& node) override;
    
    virtual void visit(AST::ExprNode_CallMethod& node) override;
    virtual void visit(AST::ExprNode_CallPath& node) override;
};

void CTypeChecker::start_scope() 
{
    m_scopes.push_back( Scope() );
}
void CTypeChecker::local_variable(bool is_mut, ::std::string name, const TypeRef& type) 
{
    DEBUG("is_mut=" << is_mut << " name=" << name << " type=" << type);
    m_scopes.back().vars.push_back( make_tuple(is_mut, name, TypeRef(type)) );
}
void CTypeChecker::local_type(::std::string name, TypeRef type)
{
    DEBUG("name=" << name << " type=" << type);
    m_scopes.back().types.push_back( make_tuple(name, ::std::move(type)) );
}
void CTypeChecker::end_scope() 
{
    m_scopes.pop_back();
}

void CTypeChecker::handle_params(AST::TypeParams& params)
{
    ::std::map<std::string,TypeRef>  trs;
    
    for( const auto& param : params.params() )
    {
        trs.insert( make_pair(param.name(), TypeRef()) );
    }
    
    for( const auto& bound : params.bounds() )
    {
        int i = params.find_name(bound.name().c_str());
        assert(i >= 0);
        trs[bound.name()].add_trait( bound.type() );
    }
    
    assert(m_scopes.back().params.size() == 0);
    m_scopes.back().params = trs;
}

TypeRef& CTypeChecker::get_local_var(const char* name)
{
    for( auto it = m_scopes.end(); it-- != m_scopes.begin(); )
    {
        for( auto it2 = it->vars.end(); it2-- != it->vars.begin(); )
        {
            if( name == ::std::get<1>(*it2) )
            {
                return ::std::get<2>(*it2);
            }
        }
    }
    throw ::std::runtime_error(FMT("get_local_var - name " << name << " not found"));
}
const TypeRef& CTypeChecker::get_local_type(const char* name)
{
    for( auto it = m_scopes.end(); it-- != m_scopes.begin(); )
    {
        for( auto it2 = it->types.end(); it2-- != it->types.begin(); )
        {
            if( name == ::std::get<0>(*it2) )
            {
                return ::std::get<1>(*it2);
            }
        }
    }
    throw ::std::runtime_error(FMT("get_local_type - name " << name << " not found"));
}
const TypeRef& CTypeChecker::get_type_param(const char* name)
{
    DEBUG("name = " << name);
    for( auto it = m_scopes.end(); it-- != m_scopes.begin(); )
    {
        DEBUG("- params = " << it->params);
        auto ent = it->params.find(name);
        if( ent != it->params.end() )
        {
            DEBUG("> match " << ent->second);
            return ent->second;
        }
    }
    throw ::std::runtime_error(FMT("get_type_param - name " << name << " not found"));
}

void CTypeChecker::handle_function(AST::Path path, AST::Function& fcn)
{
    DEBUG("(path = " << path << ")");
    start_scope();
    
    handle_params(fcn.params());

    handle_type(fcn.rettype());
    
    switch(fcn.fcn_class())
    {
    case AST::Function::CLASS_UNBOUND:
        break;
    case AST::Function::CLASS_REFMETHOD:
        local_variable(false, "self", TypeRef(TypeRef::TagReference(), false, get_local_type("Self")));
        break;
    case AST::Function::CLASS_MUTMETHOD:
        local_variable(false, "self", TypeRef(TypeRef::TagReference(), true, get_local_type("Self")));
        break;
    case AST::Function::CLASS_VALMETHOD:
        local_variable(true, "self", TypeRef(get_local_type("Self")));
        break;
    }
    
    for( auto& arg : fcn.args() )
    {
        handle_type(arg.second);
        AST::Pattern    pat(AST::Pattern::TagBind(), arg.first);
        handle_pattern( pat, arg.second );
    }

    CTC_NodeVisitor    nv(*this);
    if( fcn.code().is_valid() )
    {
        fcn.code().node().get_res_type() = fcn.rettype();
        fcn.code().visit_nodes(nv);
    }
    
    end_scope();
}

void CTypeChecker::lookup_method(const TypeRef& type, const char* name)
{
    DEBUG("(type = " << type << ", name = " << name << ")");
    // 1. Look for inherent methods on the type
    // 2. Iterate all in-scope traits, and locate an impl of that trait for this type
    //for(auto traitptr : m_scope_traits )
    //{
    //    if( !traitptr ) continue;
    //    const auto& trait = *traitptr;
    //    if( trait.has_method(name) && trait.find_impl(type) )
    //    {
    //    }
    //}
}

/// Named value - leaf
void CTC_NodeVisitor::visit(AST::ExprNode_NamedValue& node)
{
    DEBUG("ExprNode_NamedValue - " << node.m_path);
    const AST::Path&    p = node.m_path;
    if( p.is_absolute() )
    {
        // grab bound item
        switch(p.binding_type())
        {
        case AST::Path::STATIC:
            node.get_res_type() = p.bound_static().type();
            break;
        case AST::Path::ENUM_VAR: {
            const AST::Enum& enm = p.bound_enum();
            auto idx = p.bound_idx();
            // Enum variant:
            // - Check that this variant takes no arguments
            if( !enm.variants()[idx].second.is_unit() )
                throw ::std::runtime_error( FMT("Used a non-unit variant as a raw value - " << enm.variants()[idx].second));
            // - Set output type to the enum (wildcard params, not default)
            AST::Path tp = p;
            tp.nodes().pop_back();
            AST::PathNode& pn = tp.nodes().back();
            unsigned int num_params = enm.params().n_params();
            if(pn.args().size() > num_params)
                throw ::std::runtime_error( FMT("Too many arguments to enum variant - " << p) );
            while(pn.args().size() < num_params)
                pn.args().push_back( TypeRef() );
            node.get_res_type() = TypeRef(tp);
            break; }
        default:
            throw ::std::runtime_error( FMT("Unknown binding type on named value : "<<p) );
        }
    }
    else
    {
        TypeRef& local_type = m_tc.get_local_var( p[0].name().c_str() );
        node.get_res_type().merge_with( local_type );
        DEBUG("res type = " << node.get_res_type());
        local_type = node.get_res_type();
    }
}

void CTC_NodeVisitor::visit(AST::ExprNode_LetBinding& node)
{
    DEBUG("ExprNode_LetBinding");
    
    node.get_res_type() = TypeRef(TypeRef::TagUnit());
    
    // Evaluate value
    AST::NodeVisitor::visit(node.m_value);
    
    const TypeRef&  bind_type = node.m_type;
    const TypeRef&  val_type = node.m_value->get_res_type();
    
    // Obtain resultant type from value
    // Compare to binding type
    // - If both concrete, but different : error
    if( bind_type.is_concrete() && val_type.is_concrete() )
    {
        if( bind_type != val_type ) {
            throw ::std::runtime_error( FMT("Type mismatch on let, expected " << bind_type << ", got " << val_type) );
        }
    }
    // - If neither concrete, merge requirements of both
    else
    {
        node.m_type.merge_with( val_type );
        node.m_value->get_res_type() = node.m_type;
    }
    
    m_tc.handle_pattern(node.m_pat, node.m_type);
}

void CTC_NodeVisitor::visit(AST::ExprNode_Assign& node)
{
    node.get_res_type() = TypeRef(TypeRef::TagUnit());
    AST::NodeVisitor::visit(node.m_slot);
    AST::NodeVisitor::visit(node.m_value);
}

void CTC_NodeVisitor::visit(AST::ExprNode_Match& node)
{
    DEBUG("ExprNode_Match");
    AST::NodeVisitor::visit(node.m_val);
    
    for( auto& arm : node.m_arms )
    {
        m_tc.start_scope();
        m_tc.handle_pattern(arm.first, node.m_val->get_res_type());
        AST::NodeVisitor::visit(arm.second);
        m_tc.end_scope();
    }
}

void CTC_NodeVisitor::visit(AST::ExprNode_Field& node)
{
    DEBUG("ExprNode_Field " << node.m_name);
    
    AST::NodeVisitor::visit(node.m_obj);
    
    TypeRef* tr = &node.m_obj->get_res_type();
    DEBUG("ExprNode_Field - tr = " << *tr);
    if( tr->is_concrete() )
    {
        // Must be a structure type (what about associated items?)
        unsigned int deref_count = 0;
        while( tr->is_reference() )
        {
            tr = &tr->sub_types()[0];
            DEBUG("ExprNode_Field - ref deref to " << *tr);
            deref_count ++;
        }
        if( !tr->is_path() )
        {
            throw ::std::runtime_error("ExprNode_Field - Type not a path");
        }
        
        // TODO Move this logic to types.cpp?
        const AST::Path& p = tr->path();
        switch( p.binding_type() )
        {
        case AST::Path::STRUCT: {
            const AST::PathNode& lastnode = p.nodes().back();
            AST::Struct& s = const_cast<AST::Struct&>( p.bound_struct() );
            node.get_res_type().merge_with( s.get_field_type(node.m_name.c_str(), lastnode.args()) );
            break; }
        default:
            throw ::std::runtime_error("TODO: Get field from non-structure");
        }
        DEBUG("deref_count = " << deref_count);
        for( unsigned i = 0; i < deref_count; i ++ )
        {
            node.m_obj = ::std::unique_ptr<AST::ExprNode>(new AST::ExprNode_Deref( ::std::move(node.m_obj) ));
        }
    }
    else
    {
        DEBUG("ExprNode_Field - Type not concrete, can't get field");
    }
}

void CTC_NodeVisitor::visit(AST::ExprNode_Cast& node)
{
    DEBUG("ExprNode_Cast " << node.m_type);
    
    AST::NodeVisitor::visit(node.m_value);

    node.get_res_type().merge_with( node.m_type );
}

void CTC_NodeVisitor::visit(AST::ExprNode_CallMethod& node)
{
    DEBUG("ExprNode_CallMethod " << node.m_method);
    
    AST::NodeVisitor::visit(node.m_val);
    
    for( auto& arg : node.m_args )
    {
        AST::NodeVisitor::visit(arg);
    }
    
    // Locate method
    const TypeRef& type = node.m_val->get_res_type();
    DEBUG("CallMethod - type = " << type);
    if( type.is_wildcard() )
    {
        // No idea (yet)
        // - TODO: Support case where a trait is known
        throw ::std::runtime_error("Unknown type in CallMethod");
    }
    else if( type.is_type_param() )
    {
        const char *name = type.type_param().c_str();
        // Find this name in the current set of type params
        const TypeRef& p_type = m_tc.get_type_param(name);
        // Iterate bounds on type param
        TypeRef ret_type;
        for( const auto& t : p_type.traits() )
        {
            DEBUG("- Trait " << t.path());
            const AST::Trait& trait = t.path().bound_trait();
            // - Find method on one of them
            for( const auto& m : trait.functions() )
            {
                DEBUG(" > method: " << m.name << " search: " << node.m_method.name());
                if( m.name == node.m_method.name() )
                {
                    DEBUG(" > Found method");
                    if( m.data.params().n_params() )
                    {
                        throw ::std::runtime_error("TODO: Call method with params");
                    }
                    ret_type = m.data.rettype();
                }
            }
        }
        if( ret_type.is_wildcard() )
        {
            throw ::std::runtime_error("Couldn't find method");
        }
    }
    else
    {
        // Replace generic references in 'type' (copying the type) with 
        //   '_: Bounds' (allowing method lookup to succeed)
        TypeRef ltype = type;
        ltype.resolve_args( [&](const char* name) {
                return m_tc.get_type_param(name);
            } );
        // - Search for a method on this type
        //   TODO: Requires passing knowledge of in-scope traits (or trying traits)
        AST::Function& fcn = m_tc.m_crate.lookup_method(ltype, node.m_method.name().c_str());
        if( fcn.params().n_params() != node.m_method.args().size() )
        {
            throw ::std::runtime_error("CallMethod with param count mismatch");
        }
        if( fcn.params().n_params() )
        {
            throw ::std::runtime_error("TODO: CallMethod with params");
        }
        node.get_res_type().merge_with( fcn.rettype() );
    }
}
void CTC_NodeVisitor::visit(AST::ExprNode_CallPath& node)
{
    DEBUG("ExprNode_CallPath - " << node.m_path);
    ::std::vector<TypeRef> argtypes;
    for( auto& arg : node.m_args )
    {
        AST::NodeVisitor::visit(arg);
        argtypes.push_back( arg->get_res_type() );
    }
    
    if(node.m_path.binding_type() == AST::Path::FUNCTION)
    {
        const AST::Function& fcn = node.m_path.bound_func();
        
        if( fcn.params().n_params() > 0 )
        {
            throw ::std::runtime_error("CallPath - TODO: Params on functions");
        }
        
        DEBUG("ExprNode_CallPath - rt = " << fcn.rettype());
        node.get_res_type().merge_with( fcn.rettype() );
    }
    else if(node.m_path.binding_type() == AST::Path::ENUM_VAR)
    {
        const AST::Enum& enm = node.m_path.bound_enum();
        const unsigned int idx = node.m_path.bound_idx();
        const auto& var = enm.variants().at(idx);
        
        const auto& params = enm.params();
        // We know the enum, but it might have type params, need to handle that case
        
        if( params.n_params() > 0 )
        {
            // 1. Obtain the pattern set from the path (should it be pre-marked with _ types?)
            auto& path_args = node.m_path[node.m_path.size()-2].args();
            while( path_args.size() < params.n_params() )
                path_args.push_back( TypeRef() );
            DEBUG("path_args = [" << path_args << "]");
            // 2. Create a pattern from the argument types and the format of the variant
            DEBUG("argtypes = [" << argtypes << "]");
            ::std::vector<TypeRef>  item_args(enm.params().n_params());
            DEBUG("variant type = " << var.second << "");
            var.second.match_args(
                TypeRef(TypeRef::TagTuple(), argtypes),
                [&](const char *name, const TypeRef& t) {
                    DEBUG("Binding " << name << " to type " << t);
                    int idx = params.find_name(name);
                    if( idx == -1 ) {
                        throw ::std::runtime_error(FMT("Can't find generic " << name));
                    }
                    item_args.at(idx).merge_with( t );
                });
            DEBUG("item_args = [" << item_args << "]");
            // 3. Merge the two sets of arguments
            for( unsigned int i = 0; i < path_args.size(); i ++ )
            {
                path_args[i].merge_with( item_args[i] );
            }
            DEBUG("new path_args = [" << path_args << "]");
        }
    
        AST::Path   p = node.m_path;
        p.nodes().pop_back();
        TypeRef ty( ::std::move(p) );
        
        DEBUG("ExprNode_CallPath - enum t = " << ty);
        node.get_res_type().merge_with(ty);
    }
    else 
    {
        throw ::std::runtime_error("CallPath on non-function");
    }
}

void Typecheck_Expr(AST::Crate& crate)
{
    DEBUG(" >>>");
    CTypeChecker    tc(crate);
    tc.handle_module(AST::Path({}), crate.root_module());
    DEBUG(" <<<");
}

