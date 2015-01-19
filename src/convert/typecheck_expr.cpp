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
protected:
    void lookup_method(const TypeRef& type, const char* name);
public:
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
    
    virtual void visit(AST::ExprNode_LetBinding& node) override;
    
    virtual void visit(AST::ExprNode_Match& node) override;
    
    virtual void visit(AST::ExprNode_CallMethod& node) override;
};


void CTypeChecker::handle_function(AST::Path path, AST::Function& fcn)
{
    DEBUG("(path = " << path << ")");
    CTC_NodeVisitor    nv(*this);
    if( fcn.code().is_valid() )
    {
        fcn.code().visit_nodes(nv);
    }
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

void CTC_NodeVisitor::visit(AST::ExprNode_LetBinding& node)
{
    DEBUG("ExprNode_LetBinding");
    
    // Evaluate value
    AST::NodeVisitor::visit(node.m_value);
    
    const TypeRef&  bind_type = node.m_type;
    const TypeRef&  val_type = node.m_value->get_res_type();
    
    // Obtain resultant type from value
    // Compare to binding type
    // - If both concrete, but different : error
    if( !bind_type.is_wildcard() && !val_type.is_wildcard() )
    {
        if( bind_type != val_type ) {
            throw ::std::runtime_error( FMT("Type mismatch on let, expected " << bind_type << ", got " << val_type) );
        }
    }
    // - If value type concrete, but binding not : set binding to value
    else if( !val_type.is_wildcard() )
    {
        node.m_type = val_type;
    }
    // - If binding concrete, but value not : reverse propagate type (set result type of value node to binding type)
    else if( !bind_type.is_wildcard() )
    {
        // TODO: Check that value's current bind type's requirements fit this type
        node.m_value->get_res_type() = bind_type;
    }
    // - If neither concrete, merge requirements of both
    else
    {
        throw ::std::runtime_error("TODO - Merge type requirements");
    }
    
    m_tc.handle_pattern(node.m_pat, node.m_type);
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
    DEBUG("ExprNode_CallMethod");
    
    AST::NodeVisitor::visit(node.m_val);
    
    for( auto& arg : node.m_args )
    {
        AST::NodeVisitor::visit(arg);
    }
    
    // Locate method
    const TypeRef& type = node.m_val->get_res_type();
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

void Typecheck_Expr(AST::Crate& crate)
{
    DEBUG(" >>>");
    CTypeChecker    tc;
    tc.handle_module(AST::Path({}), crate.root_module());
    DEBUG(" <<<");
}

