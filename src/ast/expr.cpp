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
SERIALISE_TYPE(Expr::, "Expr", {
    s.item(m_node);
},{
    bool tmp;
    s.item(tmp);
    if( tmp )
        m_node = ExprNode::from_deserialiser(s);
    else
        m_node.reset();
});

::std::unique_ptr<ExprNode> ExprNode::from_deserialiser(Deserialiser& d) {
    ::std::string tag = d.start_object();
    
    DEBUG("tag = " << tag);
    ExprNode*   ptr = nullptr;
         if(tag == "ExprNode_Block")    ptr = new ExprNode_Block;
    else if(tag == "ExprNode_Macro")    ptr = new ExprNode_Macro;
    else
        throw ::std::runtime_error("Unknown node type " + tag);
    
    ptr->deserialise(d);
    d.end_object(tag.c_str());
    return ::std::unique_ptr<ExprNode>(ptr);
}
ExprNode::~ExprNode() {
}

ExprNode_Block::~ExprNode_Block() {
}
void ExprNode_Block::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_Block, {
    s.item(m_nodes);
})

void ExprNode_Macro::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_Macro, {
    s.item(m_name);
    //s.item(m_tokens);
})

void ExprNode_Return::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_Return, {
    s.item(m_value);
})

void ExprNode_LetBinding::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_LetBinding, {
    s.item(m_pat);
    s.item(m_value);
})

void ExprNode_Assign::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_Assign, {
    s.item(m_slot);
    s.item(m_value);
})

void ExprNode_CallPath::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_CallPath, {
    s.item(m_path);
    s.item(m_args);
})

void ExprNode_CallMethod::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_CallMethod, {
    s.item(m_val);
    s.item(m_method);
    s.item(m_args);
})

void ExprNode_CallObject::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_CallObject, {
    s.item(m_val);
    s.item(m_args);
})

void ExprNode_Match::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_Match, {
    s.item(m_val);
    s.item(m_arms);
})

void ExprNode_If::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_If, {
    s.item(m_cond);
    s.item(m_true);
    s.item(m_false);
})

void ExprNode_Integer::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_Integer, {
    s % m_datatype;
    s.item(m_value);
})

void ExprNode_StructLiteral::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_StructLiteral, {
    s.item(m_path);
    s.item(m_base_value);
    s.item(m_values);
})

void ExprNode_Tuple::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_Tuple, {
    s.item(m_values);
})

void ExprNode_NamedValue::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_NamedValue, {
    s.item(m_path);
})

void ExprNode_Field::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_Field, {
    s.item(m_obj);
    s.item(m_name);
})

void ExprNode_Cast::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
SERIALISE_TYPE_S(ExprNode_Cast, {
    s.item(m_value);
    s.item(m_type);
})

void ExprNode_BinOp::visit(NodeVisitor& nv) {
    nv.visit(*this);
}
void operator%(::Serialiser& s, const ExprNode_BinOp::Type t) {
    switch(t)
    {
    #define _(v)    case ExprNode_BinOp::v: s << #v; return
    _(CMPEQU);
    _(CMPNEQU);
    _(BITAND);
    _(BITOR);
    _(BITXOR);
    _(SHL);
    _(SHR);
    #undef _
    }
}
void operator%(::Deserialiser& s, ExprNode_BinOp::Type& t) {
    ::std::string   n;
    s.item(n);
    #define _(v)    if(n == #v) t = ExprNode_BinOp::v
         _(CMPEQU);
    else _(CMPNEQU);
    else
        throw ::std::runtime_error("");
    #undef _
}
SERIALISE_TYPE_S(ExprNode_BinOp, {
    s % m_type;
    s.item(m_left);
    s.item(m_right);
})

};

