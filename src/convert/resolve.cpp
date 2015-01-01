/*
 */
#include "../common.hpp"
#include "../ast/ast.hpp"
#include "../parse/parseerror.hpp"

class CPathResolver
{
    const AST::Crate&   m_crate;
    const AST::Module&  m_module;
    ::std::vector< ::std::string >  m_locals;
    // TODO: Maintain a stack of variable scopes
    
public:
    CPathResolver(const AST::Crate& crate, const AST::Module& mod);

    void resolve_path(AST::Path& path, bool allow_variables) const;
    void resolve_type(TypeRef& type) const;
    
    void handle_function(AST::Function& fcn);
    void handle_pattern(AST::Pattern& pat);

    void push_scope() {
        m_locals.push_back( ::std::string() );
    }
    void pop_scope() {
        for( auto it = m_locals.end(); --it != m_locals.begin(); ) {
            if( *it == "" ) {
                m_locals.erase(it, m_locals.end());
                return ;
            }
        }
        m_locals.clear();
    }
};

// Path resolution checking
void ResolvePaths(AST::Crate& crate);

class CResolvePaths_NodeVisitor:
    public AST::NodeVisitor
{
    CPathResolver&    m_res;
public:
    CResolvePaths_NodeVisitor(CPathResolver& res):
        m_res(res)
    {
    }

    void visit(AST::ExprNode_NamedValue& node) {
        m_res.resolve_path(node.m_path, true);
    }
    
    void visit(AST::ExprNode_Match& node) {
        
        AST::NodeVisitor::visit(node.m_val);
        
        for( auto& arm : node.m_arms )
        {
            m_res.push_scope();
            m_res.handle_pattern(arm.first);
            AST::NodeVisitor::visit(arm.second);
            m_res.pop_scope();
        }
    }
};

CPathResolver::CPathResolver(const AST::Crate& crate, const AST::Module& mod):
    m_crate(crate),
    m_module(mod)
{
}

void CPathResolver::resolve_path(AST::Path& path, bool allow_variables) const
{
    // Handle generic components of the path
    for( auto& ent : path.nodes() )
    {
        for( auto& arg : ent.args() )
        {
            resolve_type(arg);
        }
    }
    
    // Convert to absolute
    if( !path.is_relative() )
    {
        // Already absolute, our job is done
    } 
    else
    {
        if( allow_variables && path.size() == 1 && path[0].args().size() == 0 )
        {
            // One non-generic component, look in the current function for a variable
            const ::std::string& var = path[0].name();
            DEBUG("varname = " << var);
            auto varslot = m_locals.end();
            for(auto slot = m_locals.begin(); slot != m_locals.end(); ++slot)
            {
                if( *slot == var ) {
                    varslot = slot;
                }
            }
            
            if( varslot != m_locals.end() )
            {
                DEBUG("Located slot");
                path = AST::Path(AST::Path::TagLocal(), var);
                return ;
            }
        }
        // Search relative to current module
        throw ParseError::Todo("CPathResolver::resolve_path()");
    }
}

void CPathResolver::resolve_type(TypeRef& type) const
{
    // TODO: Convert type into absolute
    throw ParseError::Todo("CPathResolver::resolve_type()");
}

void CPathResolver::handle_pattern(AST::Pattern& pat)
{
    DEBUG("pat = " << pat);
    throw ParseError::Todo("CPathResolver::handle_pattern()");
}

void CPathResolver::handle_function(AST::Function& fcn)
{
    CResolvePaths_NodeVisitor   node_visitor(*this);
    
    for( auto& arg : fcn.args() )
        m_locals.push_back(arg.first);
    
    fcn.code().visit_nodes( node_visitor );

    resolve_type(fcn.rettype());

    for( auto& arg : fcn.args() )
        resolve_type(arg.second);
    
    pop_scope();
    if( m_locals.size() != 0 )
        throw ParseError::BugCheck("m_locals.size() != 0");
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
