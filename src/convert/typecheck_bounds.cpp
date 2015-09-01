/*
 */
#include <main_bindings.hpp>
#include "ast_iterate.hpp"
#include "../common.hpp"
#include <stdexcept>

class CGenericBoundChecker:
    public CASTIterator
{
    void check_is_trait(const AST::Path& trait) const;
public:
    virtual void handle_expr(AST::ExprNode& root) override;
    virtual void handle_params(AST::TypeParams& params) override;
};

void CGenericBoundChecker::handle_expr(AST::ExprNode& root)
{
    // Do nothing (this iterator shouldn't recurse into expressions)
}

void CGenericBoundChecker::check_is_trait(const AST::Path& trait) const {
    assert( !trait.binding().is_Unbound() );
    DEBUG("trait = " << trait);
    if( trait.binding().is_Trait() )
    {
        //throw CompileError::BoundNotTrait( bound.lex_scope(), bound.param(), trait );
        throw ::std::runtime_error(FMT("TODO - Bound " << trait << " not a trait : " << trait.binding()));
    }
    else {
        DEBUG("Bound is a trait, good");
    }
}

void CGenericBoundChecker::handle_params(AST::TypeParams& params)
{
    for(auto& bound : params.bounds())
    {
        TU_MATCH(AST::GenericBound, (bound), (ent),
        (Lifetime, {}),
        (TypeLifetime, {}),
        (IsTrait, this->check_is_trait(ent.trait);),
        (MaybeTrait, this->check_is_trait(ent.trait);),
        (NotTrait, this->check_is_trait(ent.trait);),
        (Equality, {})
        )
    }
}

/// Typecheck generic bounds (where clauses)
void Typecheck_GenericBounds(AST::Crate& crate)
{
    DEBUG(" --- ");
    CGenericBoundChecker    chk;
    chk.handle_module(AST::Path({}), crate.root_module());
}

