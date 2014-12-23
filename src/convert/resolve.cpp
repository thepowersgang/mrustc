/*
 */
#include "../common.hpp"
#include "../ast/ast.hpp"
#include "../parse/parseerror.hpp"

class CPathResolver
{
public:
    CPathResolver(const AST::Crate& crate, AST::Function& fcn);
    
    void resolve_type(TypeRef& type);
    
    void handle_function(AST::Function& fcn);
};

// Path resolution checking
void ResolvePaths(AST::Crate& crate);
void ResolvePaths_HandleFunction(const AST::Crate& crate, AST::Function& fcn);

class CResolvePaths_NodeVisitor:
    public AST::NodeVisitor
{
    const CPathResolver&    m_res;
public:
    CResolvePaths_NodeVisitor(const CPathResolver& res):
        m_res(res)
    {
    }

    void visit(AST::ExprNode::TagNamedValue, AST::ExprNode& node) {
        // TODO: Convert into a real absolute path
        throw ParseError::Todo("CResolvePaths_NodeVisitor::visit(TagNamedValue)");
    }
};

void CPathResolver::resolve_type(TypeRef& type)
{
    // TODO: Convert type into absolute
    throw ParseError::Todo("ResolvePaths_Type");
}

void CPathResolver::handle_function(AST::Function& fcn)
{
    fcn.code().visit_nodes( CResolvePaths_NodeVisitor(*this) );

    resolve_type(fcn.rettype());

    FOREACH_M(AST::Function::Arglist, arg, fcn.args())
    {
        resolve_type(arg->second);
    }
}

void ResolvePaths(AST::Crate& crate)
{
    crate.iterate_functions(ResolvePaths_HandleFunction);
}
