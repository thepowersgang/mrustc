/*
* MRustC - Rust Compiler
* - By John Hodge (Mutabah/thePowersGang)
*
* expand/rustc_box.cpp
* - 
*/
#include <synext.hpp>
#include <ast/generics.hpp>
#include <ast/ast.hpp>


// #[rustc_box] - Marks the `Box::new` inner constructor
class CHandler_RustBox:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Post; }

    void handle(const Span& sp, const AST::Attribute& mi, AST::Crate& crate, ::AST::ExprNodeP& expr) const override {
        auto* n = dynamic_cast<AST::ExprNode_CallPath*>(expr.get());
        ASSERT_BUG(expr->span(), n, "");
        ASSERT_BUG(expr->span(), n->m_args.size() == 1, "");
        auto val = std::move(n->m_args[0]);
        auto span = n->span();
        expr.reset(new AST::ExprNode_UniOp(AST::ExprNode_UniOp::BOX, std::move(val)));
        expr->set_span(span);
    }
};
STATIC_DECORATOR("rustc_box", CHandler_RustBox);
