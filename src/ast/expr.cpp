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
    if( pat.m_node.get() )
        return os << *pat.m_node;
    else
        return os << "/* null */";
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

::std::ostream& operator<<(::std::ostream& os, const ExprNode& node)
{
    if( &node != nullptr ) {
        node.print(os);
    }
    else {
        os << "/* NULLPTR */";
    }
    return os;
}
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
    else _(ExprNode_Deref)
    else _(ExprNode_Cast)
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

#define NODE(class, serialise, _print) void class::visit(NodeVisitor& nv) { nv.visit(*this); } SERIALISE_TYPE_S(class, serialise) void class::print(::std::ostream& os) const _print

ExprNode_Block::~ExprNode_Block() {
}
NODE(ExprNode_Block, {
    s.item(m_nodes);
},{
    os << "{";
    for(const auto& n : m_nodes)
        os << *n << ";";
    os << "}";
})

NODE(ExprNode_Macro, {
    s.item(m_name);
    //s.item(m_tokens);
},{
    os << m_name << "!(" << ")";
})

NODE(ExprNode_Return, {
    s.item(m_value);
},{
    os << "return " << *m_value;
})

NODE(ExprNode_LetBinding, {
    s.item(m_pat);
    s.item(m_value);
},{
    os << "let " << m_pat << ": " << m_type << " = " << *m_value;
})

NODE(ExprNode_Assign, {
    s.item(m_slot);
    s.item(m_value);
},{
    os << *m_slot << " = " << *m_value;
})

NODE(ExprNode_CallPath, {
    s.item(m_path);
    s.item(m_args);
},{
    os << m_path << "(";
    for(const auto& a : m_args) {
        os << *a << ",";
    }
    os << ")";
})

NODE(ExprNode_CallMethod, {
    s.item(m_val);
    s.item(m_method);
    s.item(m_args);
},{
    os << "(" << *m_val << ")." << m_method << "(";
    for(const auto& a : m_args) {
        os << *a << ",";
    }
    os << ")";
})

NODE(ExprNode_CallObject, {
    s.item(m_val);
    s.item(m_args);
},{
    os << "(" << *m_val << ")(";
    for(const auto& a : m_args) {
        os << *a << ",";
    }
    os << ")";
})

NODE(ExprNode_Match, {
    s.item(m_val);
    s.item(m_arms);
},{
    os << "match ("<<*m_val<<") {";
    for(const auto& arm : m_arms)
    {
        os << " " << arm.first << " => " << *arm.second << ",";
    }
    os << "}";
})

NODE(ExprNode_If, {
    s.item(m_cond);
    s.item(m_true);
    s.item(m_false);
},{
    os << "if " << *m_cond << " { " << *m_true << " } else { " << *m_false << " }";
})

NODE(ExprNode_Integer, {
    s % m_datatype;
    s.item(m_value);
},{
    os << m_value;
})

NODE(ExprNode_StructLiteral, {
    s.item(m_path);
    s.item(m_base_value);
    s.item(m_values);
},{
    os << "/* todo: sl */";
})

NODE(ExprNode_Tuple, {
    s.item(m_values);
},{
    os << "(";
    for(const auto& a : m_values) {
        os << *a << ",";
    }
    os << ")";
})

NODE(ExprNode_NamedValue, {
    s.item(m_path);
},{
    os << m_path;
})

NODE(ExprNode_Field, {
    s.item(m_obj);
    s.item(m_name);
},{
    os << "(" << *m_obj << ")." << m_name;
})

NODE(ExprNode_Deref, {
    s.item(m_value);
},{
    os << "*(" << *m_value << ")";
});

NODE(ExprNode_Cast, {
    s.item(m_value);
    s.item(m_type);
},{
    os << "(" << *m_value << " as " << m_type << ")";
})

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
NODE(ExprNode_BinOp, {
    s % m_type;
    s.item(m_left);
    s.item(m_right);
},{
    os << "(" << *m_left << " ";
    switch(m_type)
    {
    case CMPEQU:    os << "=="; break;
    case CMPNEQU:   os << "!="; break;
    case BITAND:    os << "&"; break;
    case BITOR:     os << "|"; break;
    case BITXOR:    os << "^"; break;
    case SHR:    os << ">>"; break;
    case SHL:    os << "<<"; break;
    }
    os << " " << *m_right << ")";
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
void NodeVisitor::visit(ExprNode_Deref& node) 
{
    DEBUG("DEF - ExprNode_Deref");
    visit(node.m_value);
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

