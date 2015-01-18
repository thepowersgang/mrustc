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
    virtual void handle_function(AST::Path& path, AST::Function& fcn) override;
};


void CTypeChecker::handle_function(AST::Path& path, AST::Function& fcn)
{
    
}

void Typecheck_Expr(AST::Crate& crate)
{
    DEBUG(" >>>");
    CTypeChecker    tc;
    tc.handle_module(AST::Path({}), crate.root_module());
    DEBUG(" <<<");
}

