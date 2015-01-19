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
    return os << "Expr(TODO)";
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
    #define _(x)    if(tag == #x) ptr = new x;
         _(ExprNode_Block)
    else _(ExprNode_Macro)
    else _(ExprNode_Return)
    else _(ExprNode_LetBinding)
    else _(ExprNode_Assign)
    else _(ExprNode_CallPath)
    else _(ExprNode_CallMethod)
    else _(ExprNode_CallObject)
    else _(ExprNode_Match)
    else _(ExprNode_If)
    else _(ExprNode_Integer)
    else _(ExprNode_StructLiteral)
    else _(ExprNode_Tuple)
    else _(ExprNode_NamedValue)
    else _(ExprNode_Field)
    else _(ExprNode_CallPath)
    else _(ExprNode_BinOp)
    else
        throw ::std::runtime_error("Unknown node type " + tag);
    #undef _
    
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


void NodeVisitor::visit(ExprNode_Block& node)
{
    DEBUG("DEF - ExprNode_Block");
    INDENT();
    for( auto& child : node.m_nodes )
        visit(child);
    UNINDENT();
}
void NodeVisitor::visit(ExprNode_Macro& node)
{
    DEBUG("DEF - ExprNode_Macro");
}
void NodeVisitor::visit(ExprNode_Return& node) 
{
    DEBUG("DEF - ExprNode_Return");
    visit(node.m_value);
}
void NodeVisitor::visit(ExprNode_LetBinding& node) 
{
    DEBUG("DEF - ExprNode_LetBinding");
    // TODO: Handle recurse into Let pattern
    visit(node.m_value);
}
void NodeVisitor::visit(ExprNode_Assign& node) 
{
    DEBUG("DEF - ExprNode_Assign");
    INDENT();
    visit(node.m_slot);
    visit(node.m_value);
    UNINDENT();
}
void NodeVisitor::visit(ExprNode_CallPath& node) 
{
    DEBUG("DEF - ExprNode_CallPath");
    INDENT();
    for( auto& arg : node.m_args )
        visit(arg);
    UNINDENT();
}
void NodeVisitor::visit(ExprNode_CallMethod& node) 
{
    DEBUG("DEF - ExprNode_CallMethod");
    INDENT();
    visit(node.m_val);
    for( auto& arg : node.m_args )
        visit(arg);
    UNINDENT();
}
void NodeVisitor::visit(ExprNode_CallObject& node) 
{
    DEBUG("DEF - ExprNode_CallObject");
    INDENT();
    visit(node.m_val);
    for( auto& arg : node.m_args )
        visit(arg);
    UNINDENT();
}
void NodeVisitor::visit(ExprNode_Match& node) 
{
    DEBUG("DEF - ExprNode_Match");
    INDENT();
    visit(node.m_val);
    for( auto& arm : node.m_arms )
        visit(arm.second);
    UNINDENT();
}
void NodeVisitor::visit(ExprNode_If& node) 
{
    DEBUG("DEF - ExprNode_If");
    INDENT();
    visit(node.m_cond);
    visit(node.m_true);
    visit(node.m_false);
    UNINDENT();
}

void NodeVisitor::visit(ExprNode_Integer& node) 
{
    DEBUG("DEF - ExprNode_Integer");
    // LEAF
}
void NodeVisitor::visit(ExprNode_StructLiteral& node) 
{
    DEBUG("DEF - ExprNode_StructLiteral");
    visit(node.m_base_value);
    for( auto& val : node.m_values )
        visit(val.second);
}
void NodeVisitor::visit(ExprNode_Tuple& node) 
{
    DEBUG("DEF - ExprNode_Tuple");
    for( auto& val : node.m_values )
        visit(val);
}
void NodeVisitor::visit(ExprNode_NamedValue& node) 
{
    DEBUG("DEF - ExprNode_NamedValue");
    // LEAF
}

void NodeVisitor::visit(ExprNode_Field& node) 
{
    DEBUG("DEF - ExprNode_Field");
    visit(node.m_obj);
}
void NodeVisitor::visit(ExprNode_Cast& node) 
{
    DEBUG("DEF - ExprNode_Cast");
    visit(node.m_value);
}
void NodeVisitor::visit(ExprNode_BinOp& node) 
{
    DEBUG("DEF - ExprNode_BinOp");
    visit(node.m_left);
    visit(node.m_right);
}


};

