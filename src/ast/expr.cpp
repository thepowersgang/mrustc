/*
 */
#include "expr.hpp"

namespace AST {

void Expr::visit_nodes(NodeVisitor& v)
{
	assert(!!m_node);
    m_node->visit(v);
}
void Expr::visit_nodes(NodeVisitor& v) const
{
	assert(!!m_node);
	assert(v.is_const());
    //const_cast<const ExprNode*>(m_node.get())->visit(v);
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
    else _(ExprNode_UniOp)
    else
        throw ::std::runtime_error("Unknown node type " + tag);
    #undef _
    
    ptr->deserialise(d);
    d.end_object(tag.c_str());
    return ::std::unique_ptr<ExprNode>(ptr);
}
ExprNode::~ExprNode() {
}

#define NODE(class, serialise, _print)\
	void class::visit(NodeVisitor& nv) { nv.visit(*this); } \
	/*void class::visit(NodeVisitor& nv) const { nv.visit(*this); }*/ \
	void class::print(::std::ostream& os) const _print \
	SERIALISE_TYPE_S(class, serialise) \

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
	_(MULTIPLY);
	_(DIVIDE);
	_(MODULO);
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
	case MULTIPLY: os << "*"; break;
	case DIVIDE:   os << "/"; break;
	case MODULO:   os << "%"; break;
    }
    os << " " << *m_right << ")";
})

void operator%(::Serialiser& s, const ExprNode_UniOp::Type t) {
    switch(t)
    {
    #define _(v)    case ExprNode_UniOp::v: s << #v; return;
    _(NEGATE)
    _(INVERT)
    _(BOX)
    _(REF)
    #undef _
    }
}
void operator%(::Deserialiser& s, enum ExprNode_UniOp::Type& t) {
    ::std::string   n;
    s.item(n);
    if(1)   ;
    #define _(v)    else if(n == #v) t = ExprNode_UniOp::v;
    _(NEGATE)
    _(INVERT)
    _(BOX)
    _(REF)
    else
        throw ::std::runtime_error( FMT("No uniop type for '" << n << "'") );
    #undef _
}
NODE(ExprNode_UniOp, {
    s % m_type;
    s.item(m_value);
},{
    switch(m_type)
    {
    case NEGATE: os << "(-"; break;
    case INVERT: os << "(!"; break;
    case BOX: os << "(box "; break;
    case REF: os << "(&"; break;
    }
    os << *m_value << ")";
})


#define NV(type, actions)\
	void NodeVisitorDef::visit(type& node) { DEBUG("DEF - "#type); actions }
//	void NodeVisitorDef::visit(const type& node) { DEBUG("DEF - "#type" (const)"); actions }

NV(ExprNode_Block, {
    INDENT();
    for( auto& child : node.m_nodes )
        visit(child);
    UNINDENT();
})
NV(ExprNode_Macro,
{
    DEBUG("TODO: Macro");
})
NV(ExprNode_Return,
{
    visit(node.m_value);
})
NV(ExprNode_LetBinding,
{
    // TODO: Handle recurse into Let pattern
    visit(node.m_value);
})
NV(ExprNode_Assign,
{
    INDENT();
    visit(node.m_slot);
    visit(node.m_value);
    UNINDENT();
})
NV(ExprNode_CallPath,
{
    INDENT();
    for( auto& arg : node.m_args )
        visit(arg);
    UNINDENT();
})
NV(ExprNode_CallMethod,
{
    INDENT();
    visit(node.m_val);
    for( auto& arg : node.m_args )
        visit(arg);
    UNINDENT();
})
NV(ExprNode_CallObject,
{
    INDENT();
    visit(node.m_val);
    for( auto& arg : node.m_args )
        visit(arg);
    UNINDENT();
})
NV(ExprNode_Match,
{
    INDENT();
    visit(node.m_val);
    for( auto& arm : node.m_arms )
        visit(arm.second);
    UNINDENT();
})
NV(ExprNode_If,
{
    INDENT();
    visit(node.m_cond);
    visit(node.m_true);
    visit(node.m_false);
    UNINDENT();
})

NV(ExprNode_Integer,
{
    // LEAF
})
NV(ExprNode_StructLiteral,
{
    visit(node.m_base_value);
    for( auto& val : node.m_values )
        visit(val.second);
})
NV(ExprNode_Tuple,
{
    for( auto& val : node.m_values )
        visit(val);
})
NV(ExprNode_NamedValue,
{
    // LEAF
})

NV(ExprNode_Field,
{
    visit(node.m_obj);
})
NV(ExprNode_Deref,
{
    visit(node.m_value);
})
NV(ExprNode_Cast,
{
    visit(node.m_value);
})
NV(ExprNode_BinOp,
{
    visit(node.m_left);
    visit(node.m_right);
})
NV(ExprNode_UniOp,
{
    visit(node.m_value);
})
#undef NV


};

