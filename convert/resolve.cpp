
#include "../ast/ast.hpp"

// Path resolution checking
void ResolvePaths(AST::Crate& crate);
void ResolvePaths_HandleFunction(const AST::Crate& crate, AST::Function& fcn);

class CResolvePaths_NodeVisitor:
    public AST::NodeVisitor
{
    const AST::Crate& m_crate;
public:
    CResolvePaths_NodeVisitor(const AST::Crate& crate):
        m_crate(crate)
    {
    }

    void visit(AST::ExprNode::TagNamedValue, AST::ExprNode& node) {
        // TODO: Convert into a real absolute path
    }
};

void ResolvePaths_HandleFunction(const AST::Crate& crate, AST::Function& fcn)
{
    fcn.code().visit_nodes( CResolvePaths_NodeVisitor(crate) );

    ResolvePaths_Type(fcn.rettype());

    FOREACH(arg, fcn.args())
    {
        ResolvePaths_Type(arg.type());
    }
}

void ResolvePaths(AST::Crate& crate)
{
    crate.iterate_functions(ResolvePaths_HandleFunction);
}
