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
public:
    virtual void handle_function(AST::Path path, AST::Function& fcn) override;
    // - Ignore all non-function items on this pass
    virtual void handle_enum(AST::Path path, AST::Enum& ) override {}
    virtual void handle_struct(AST::Path path, AST::Struct& str) override {}
    virtual void handle_alias(AST::Path path, AST::TypeAlias& ) override {}
};
class CNodeVisitor:
    public AST::NodeVisitor
{
    CTypeChecker&   m_tc;
public:
    CNodeVisitor(CTypeChecker& tc):
        m_tc(tc)
    {}
    
    void visit(AST::ExprNode_LetBinding& node) override;
};


void CTypeChecker::handle_function(AST::Path path, AST::Function& fcn)
{
    CNodeVisitor    nv(*this);
    if( fcn.code().is_valid() )
    {
        fcn.code().visit_nodes(nv);
    }
}

void CNodeVisitor::visit(AST::ExprNode_LetBinding& node)
{
    // TODO: 
}

void Typecheck_Expr(AST::Crate& crate)
{
    DEBUG(" >>>");
    CTypeChecker    tc;
    tc.handle_module(AST::Path({}), crate.root_module());
    DEBUG(" <<<");
}

