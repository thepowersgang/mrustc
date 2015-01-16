/*
 */
#include "expr.hpp"

namespace AST {

void Expr::visit_nodes(NodeVisitor& v)
{
    m_node->visit(v);
}
::std::ostream& operator<<(::std::ostream& os, const Expr& pat)
{
    os << "Expr(TODO)";
    return os;
}

ExprNode::~ExprNode() {
}

ExprNode_Block::~ExprNode_Block() {
}
void ExprNode_Block::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Return::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_LetBinding::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Assign::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_CallPath::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_CallMethod::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_CallObject::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Match::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_If::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Integer::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_StructLiteral::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Tuple::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_NamedValue::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Field::visit(NodeVisitor& nv) {
    nv.visit(*this);
}

void ExprNode_Cast::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
void ExprNode_BinOp::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
};

