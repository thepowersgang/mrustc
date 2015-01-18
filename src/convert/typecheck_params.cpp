/*
 */
#include <main_bindings.hpp>
#include "ast_iterate.hpp"
#include "../common.hpp"
#include <stdexcept>

// === PROTOTYPES ===
class CGenericParamChecker:
    public CASTIterator
{
     int    m_within_expr = 0;
public:
    virtual void handle_path(AST::Path& path, CASTIterator::PathMode pm) override;
    virtual void handle_expr(AST::ExprNode& root) override;
};

class CNodeVisitor:
    public AST::NodeVisitor
{
    CGenericParamChecker&   m_pc;
public:
    CNodeVisitor(CGenericParamChecker& pc):
        m_pc(pc)
    {}
};

/// Check that the passed set of parameters match the requiremens for a generic item
///
/// \param info Generic item information (param names and bounds)
/// \param types Type parameters being passed to the generic item
/// \param allow_infer  Allow inferrence (mutates \a types with conditions from \a info)
bool check_generic_params(const AST::TypeParams& info, ::std::vector<TypeRef>& types, bool allow_infer = false)
{
    const auto& params = info.params();
    if( types.size() > params.size() )
    {
        throw ::std::runtime_error(FMT("Too many generic params ("<<types.size()<<" passed, expecting "<< params.size()<<")"));
    }
    else if( types.size() < params.size() )
    {
        if( allow_infer )
        {
            while( types.size() < params.size() )
            {
                types.push_back( TypeRef() );
            }
        }
        else
        {
            throw ::std::runtime_error(FMT("Too few generic params, ("<<types.size()<<" passed, expecting "<<params.size()<<")"));
        }
    }
}

// === CODE ===
void CGenericParamChecker::handle_path(AST::Path& path, CASTIterator::PathMode pm)
{
    AST::PathNode& last_node = path[path.size()-1];
    switch(path.binding_type())
    {
    case AST::Path::TRAIT:
        // Outside of expressions, param types must match perfectly
        if( m_within_expr == 0 ) {
            try {
                check_generic_params(path.bound_trait().params(), last_node.args());
            }
            catch( const ::std::exception& e )
            {
                throw ::std::runtime_error( FMT("Checking '" << path << "', threw : " << e.what()) );
            }
        }
        break;
    }
}

void CGenericParamChecker::handle_expr(AST::ExprNode& root)
{
    m_within_expr += 1;
    // Do nothing (this iterator shouldn't recurse into expressions)
    CNodeVisitor    nv(*this);
    root.visit(nv);
    m_within_expr -= 1;
}

/// Typecheck generic parameters (ensure that they match all generic bounds)
void Typecheck_GenericParams(AST::Crate& crate)
{
    DEBUG(" --- ");
    CGenericParamChecker    chk;
    chk.handle_module(AST::Path({}), crate.root_module());
}

