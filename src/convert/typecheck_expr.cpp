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
    };
    
    ::std::vector<Scope>    m_scopes;
    
protected:
    TypeRef& get_local(const char* name);
    void lookup_method(const TypeRef& type, const char* name);
public:
    virtual void start_scope() override;
    virtual void local_variable(bool is_mut, ::std::string name, const TypeRef& type) override;
    virtual void end_scope() override;
    
    virtual void handle_function(AST::Path path, AST::Function& fcn) override;
    // - Ignore all non-function items on this pass
    virtual void handle_enum(AST::Path path, AST::Enum& ) override {}
    virtual void handle_struct(AST::Path path, AST::Struct& str) override {}
    virtual void handle_alias(AST::Path path, AST::TypeAlias& ) override {}
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
    
    virtual void visit(AST::ExprNode_CallMethod& node) override;
    virtual void visit(AST::ExprNode_CallPath& node) override;
};

void CTypeChecker::start_scope() 
{
    m_scopes.push_back( Scope() );
}
void CTypeChecker::local_variable(bool is_mut, ::std::string name, const TypeRef& type) 
{
    m_scopes.back().vars.push_back( make_tuple(is_mut, name, TypeRef(type)) );
}
void CTypeChecker::end_scope() 
{
    m_scopes.pop_back();
}

TypeRef& CTypeChecker::get_local(const char* name)
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
    throw ::std::runtime_error(FMT("get_local - name " << name << " not found"));
}

void CTypeChecker::handle_function(AST::Path path, AST::Function& fcn)
{
    DEBUG("(path = " << path << ")");
    start_scope();
    
    handle_params(fcn.params());

    handle_type(fcn.rettype());
    
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
        TypeRef& local_type = m_tc.get_local( p[0].name().c_str() );
        node.get_res_type().merge_with( local_type );
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
    DEBUG("- type = " << type);
    if( type.is_wildcard() )
    {
        // No idea (yet)
        // - TODO: Support case where a trait is known
    }
    else
    {
        // - Search for a method on this type
        //const Function& fcn = type.lookup_method(node.m_method.name());
    }
}
void CTC_NodeVisitor::visit(AST::ExprNode_CallPath& node)
{
    DEBUG("ExprNode_CallPath - " << node.m_path);
    for( auto& arg : node.m_args )
    {
        AST::NodeVisitor::visit(arg);
    }
    
    if(node.m_path.binding_type() == AST::Path::FUNCTION) {
        const AST::Function& fcn = node.m_path.bound_func();
    }
    else if(node.m_path.binding_type() == AST::Path::ENUM_VAR) {
        const AST::Enum& emn = node.m_path.bound_enum();
        // We know the enum, but it might have type params, need to handle that case
    }
    else 
    {
        throw ::std::runtime_error("CallPath on non-function");
    }
}

void Typecheck_Expr(AST::Crate& crate)
{
    DEBUG(" >>>");
    CTypeChecker    tc;
    tc.handle_module(AST::Path({}), crate.root_module());
    DEBUG(" <<<");
}

