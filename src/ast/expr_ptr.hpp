/*
  * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/expr_ptr.hpp
 * - Pointer type wrapping AST::ExprNode (prevents need to know the full definition)
 */
#pragma once
#include <memory>

namespace AST {

class ExprNode;
class NodeVisitor;

extern ::std::ostream& operator<<(::std::ostream& os, const ExprNode& node);

class ExprNodeP
{
    ExprNode*   m_ptr;
public:
    ~ExprNodeP();
    ExprNodeP(): m_ptr(nullptr) {}
    ExprNodeP(ExprNode* node): m_ptr(node) {}
    ExprNodeP(std::unique_ptr<ExprNode> node);//: m_ptr(node.release()) {}

    ExprNodeP(ExprNodeP&& x): m_ptr(x.m_ptr) { x.m_ptr = nullptr; }
    ExprNodeP(const ExprNodeP& x) = delete;
    ExprNodeP& operator=(ExprNodeP&& x) { this->~ExprNodeP(); this->m_ptr = x.m_ptr; x.m_ptr = nullptr; return *this; }
    ExprNodeP& operator=(const ExprNodeP& x) = delete;

    operator bool() const { return is_valid(); }
    bool is_valid() const { return m_ptr != nullptr; }

    ExprNode& operator*() { return *m_ptr; }
    const ExprNode& operator*() const { return *m_ptr; }
    ExprNode* operator->() { return m_ptr; }
    const ExprNode* operator->() const { return m_ptr; }

    ExprNode* get() { return m_ptr; }
    const ExprNode* get() const { return m_ptr; }

    ExprNode* release() { auto rv = m_ptr; m_ptr = nullptr; return rv; }
    void reset(ExprNode* n = nullptr) { this->~ExprNodeP(); m_ptr = n; }
};

class Expr
{
    ::std::shared_ptr<ExprNode> m_node;
public:
    Expr(ExprNodeP node);
    Expr(ExprNode* node);
    Expr();

    operator bool() const { return is_valid(); }
    bool is_valid() const { return m_node.get() != nullptr; }
    const ExprNode& node() const { assert(m_node.get()); return *m_node; }
          ExprNode& node()       { assert(m_node.get()); return *m_node; }
    ::std::shared_ptr<ExprNode> take_node() { assert(m_node.get()); return ::std::move(m_node); }
    void visit_nodes(NodeVisitor& v);
    void visit_nodes(NodeVisitor& v) const;

    Expr clone() const;

    friend ::std::ostream& operator<<(::std::ostream& os, const Expr& pat);
};

}
