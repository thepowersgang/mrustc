/*
 */
#include "../common.hpp"
#include "../ast/ast.hpp"
#include "../parse/parseerror.hpp"

class CPathResolver
{
    const AST::Crate&   m_crate;
    const AST::Module&  m_module;
    
public:
    CPathResolver(const AST::Crate& crate, const AST::Module& mod);
    
    void resolve_type(TypeRef& type);
    
    void handle_function(AST::Function& fcn);
};

// Path resolution checking
void ResolvePaths(AST::Crate& crate);

class CResolvePaths_NodeVisitor:
    public AST::NodeVisitor
{
    const CPathResolver&    m_res;
public:
    CResolvePaths_NodeVisitor(const CPathResolver& res):
        m_res(res)
    {
    }

    void visit(AST::ExprNode_NamedValue& node) {
        // TODO: Convert into a real absolute path
        throw ParseError::Todo("CResolvePaths_NodeVisitor::visit(TagNamedValue)");
    }
};

CPathResolver::CPathResolver(const AST::Crate& crate, const AST::Module& mod):
    m_crate(crate),
    m_module(mod)
{
}

void CPathResolver::resolve_type(TypeRef& type)
{
    // TODO: Convert type into absolute
    throw ParseError::Todo("ResolvePaths_Type");
}

void CPathResolver::handle_function(AST::Function& fcn)
{
    CResolvePaths_NodeVisitor   node_visitor(*this);
    
    fcn.code().visit_nodes( node_visitor );

    resolve_type(fcn.rettype());

    FOREACH_M(AST::Function::Arglist, arg, fcn.args())
    {
        resolve_type(arg->second);
    }
}

void ResolvePaths_HandleFunction(const AST::Crate& crate, const AST::Module& mod, AST::Function& fcn)
{
	CPathResolver	pr(crate, mod);
	pr.handle_function(fcn);
}

void ResolvePaths(AST::Crate& crate)
{
    crate.iterate_functions(ResolvePaths_HandleFunction);
}
