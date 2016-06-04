/*
 */
#include "expr.hpp"
#include "ast.hpp"

namespace AST {

void Expr::visit_nodes(NodeVisitor& v)
{
    if( m_node )
    {
        m_node->visit(v);
    }
}
void Expr::visit_nodes(NodeVisitor& v) const
{
    if( m_node )
    {
        assert(v.is_const());
        //const_cast<const ExprNode*>(m_node.get())->visit(v);
        m_node->visit(v);
    }
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
    else _(ExprNode_Flow)
    else _(ExprNode_LetBinding)
    else _(ExprNode_Assign)
    else _(ExprNode_CallPath)
    else _(ExprNode_CallMethod)
    else _(ExprNode_CallObject)
    else _(ExprNode_Match)
    else _(ExprNode_Loop)
    else _(ExprNode_If)
    else _(ExprNode_IfLet)
    else _(ExprNode_Integer)
    else _(ExprNode_Closure)
    else _(ExprNode_StructLiteral)
    else _(ExprNode_Array)
    else _(ExprNode_Tuple)
    else _(ExprNode_NamedValue)
    else _(ExprNode_Field)
    else _(ExprNode_Deref)
    else _(ExprNode_Cast)
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

#define NODE(class, serialise, _print, _clone)\
    void class::visit(NodeVisitor& nv) { nv.visit(*this); } \
    void class::print(::std::ostream& os) const _print \
    ::std::unique_ptr<ExprNode> class::clone() const _clone \
    SERIALISE_TYPE_S(class, serialise)
#define OPT_CLONE(node) (node.get() ? node->clone() : ::AST::ExprNodeP())

namespace {
    static inline ExprNodeP mk_exprnodep(const Position& pos, AST::ExprNode* en) {
        en->set_pos(pos);
        return ExprNodeP(en);
    }
    #define NEWNODE(type, ...)  mk_exprnodep(get_pos(), new type(__VA_ARGS__))
}

NODE(ExprNode_Block, {
    s.item(m_nodes);
},{
    os << "{";
    for(const auto& n : m_nodes)
        os << *n << ";";
    os << "}";
},{
    ::std::vector<ExprNodeP>    nodes;
    for(const auto& n : m_nodes)
        nodes.push_back( n->clone() );
    if( m_local_mod )
        TODO(get_pos(), "Handle cloning ExprNode_Block with a module");
    return NEWNODE(ExprNode_Block, mv$(nodes), nullptr);
})

NODE(ExprNode_Macro, {
    s.item(m_name);
    s.item(m_ident);
    s.item(m_tokens);
},{
    os << m_name << "!";
    if( m_ident.size() > 0 )
    {
        os << " " << m_ident << " ";
    }
    os << "(" << ")";
},{
    return NEWNODE(ExprNode_Macro, m_name, m_ident, m_tokens.clone());
})

void operator%(::Serialiser& s, const ExprNode_Flow::Type t) {
    switch(t)
    {
    #define _(v)    case ExprNode_Flow::v: s << #v; return
    _(RETURN);
    _(BREAK);
    _(CONTINUE);
    #undef _
    }
}
void operator%(::Deserialiser& s, ExprNode_Flow::Type& t) {
    ::std::string   n;
    s.item(n);
    if(0)   ;
    #define _(v)    else if(n == #v) t = ExprNode_Flow::v
    _(RETURN);
    _(BREAK);
    _(CONTINUE);
    #undef _
    else
        throw ::std::runtime_error("");
}
NODE(ExprNode_Flow, {
    s % m_type;
    s.item(m_target);
    s.item(m_value);
},{
    switch(m_type)
    {
    case RETURN:    os << "return"; break;
    case BREAK:     os << "break"; break;
    case CONTINUE:  os << "continue"; break;
    }
    os << " " << *m_value;
},{
    return NEWNODE(ExprNode_Flow, m_type, m_target, m_value->clone());
})


NODE(ExprNode_LetBinding, {
    s.item(m_pat);
    s.item(m_type);
    s.item(m_value);
},{
    os << "let " << m_pat << ": " << m_type << " = " << *m_value;
},{
    return NEWNODE(ExprNode_LetBinding, m_pat.clone(), TypeRef(m_type), OPT_CLONE(m_value));
})

NODE(ExprNode_Assign, {
    s.item(m_slot);
    s.item(m_value);
},{
    os << *m_slot << " = " << *m_value;
},{
    return NEWNODE(ExprNode_Assign, m_op, m_slot->clone(), m_value->clone());
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
},{
    ::std::vector<ExprNodeP>    args;
    for(const auto& a : m_args) {
        args.push_back( a->clone() );
    }
    return NEWNODE(ExprNode_CallPath, AST::Path(m_path), mv$(args));
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
},{
    ::std::vector<ExprNodeP>    args;
    for(const auto& a : m_args) {
        args.push_back( a->clone() );
    }
    return NEWNODE(ExprNode_CallMethod, m_val->clone(), m_method, mv$(args));
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
},{
    ::std::vector<ExprNodeP>    args;
    for(const auto& a : m_args) {
        args.push_back( a->clone() );
    }
    return NEWNODE(ExprNode_CallObject, m_val->clone(), mv$(args));
})

void operator%(::Serialiser& s, const ExprNode_Loop::Type t) {
    switch(t)
    {
    #define _(v)    case ExprNode_Loop::v: s << #v; return
    _(LOOP);
    _(WHILE);
    _(WHILELET);
    _(FOR);
    #undef _
    }
}
void operator%(::Deserialiser& s, ExprNode_Loop::Type& t) {
    ::std::string   n;
    s.item(n);
    if(0)   ;
    #define _(v)    else if(n == #v) t = ExprNode_Loop::v
    _(LOOP);
    _(WHILE);
    _(WHILELET);
    _(FOR);
    #undef _
    else
        throw ::std::runtime_error("");
}
NODE(ExprNode_Loop, {
    s % m_type;
    s.item(m_label);
    s.item(m_pattern);
    s.item(m_cond);
    s.item(m_code);
},{
    os << "LOOP [" << m_label << "] " << m_pattern << " in/= " << *m_cond << " " << *m_code;
},{
    return NEWNODE(ExprNode_Loop, m_label, m_type, m_pattern.clone(), OPT_CLONE(m_cond), m_code->clone());
})

SERIALISE_TYPE_A(ExprNode_Match_Arm::, "ExprNode_Match_Arm", {
    s.item(m_patterns);
    s.item(m_cond);
    s.item(m_code);
});
NODE(ExprNode_Match, {
    s.item(m_val);
    s.item(m_arms);
},{
    os << "match ("<<*m_val<<") {";
    for(const auto& arm : m_arms)
    {
        for( const auto& pat : arm.m_patterns )
            os << " " << pat;
        os << " if " << *arm.m_cond;
        
        os << " => " << *arm.m_code << ",";
    }
    os << "}";
},{
    ::std::vector< ExprNode_Match_Arm>  arms;
    for(const auto& arm : m_arms) {
        ::std::vector< AST::Pattern>    patterns;
        for( const auto& pat : arm.m_patterns ) {
            patterns.push_back( pat.clone() );
        }
        arms.push_back( ExprNode_Match_Arm( mv$(patterns), OPT_CLONE(arm.m_cond), arm.m_code->clone() ) );
        arms.back().m_attrs = arm.m_attrs.clone();
    }
    return NEWNODE(ExprNode_Match, m_val->clone(), mv$(arms));
})

NODE(ExprNode_If, {
    s.item(m_cond);
    s.item(m_true);
    s.item(m_false);
},{
    os << "if " << *m_cond << " { " << *m_true << " } else { " << *m_false << " }";
},{
    return NEWNODE(ExprNode_If, m_cond->clone(), m_true->clone(), OPT_CLONE(m_false));
})
NODE(ExprNode_IfLet, {
    s.item(m_pattern);
    s.item(m_value);
    s.item(m_true);
    s.item(m_false);
},{
    os << "if let " << m_pattern << " = (" << *m_value << ") { " << *m_true << " } else { " << *m_false << " }";
},{
    return NEWNODE(ExprNode_IfLet, m_pattern.clone(), m_value->clone(), m_true->clone(), OPT_CLONE(m_false));
})

NODE(ExprNode_Integer, {
    s % m_datatype;
    s.item(m_value);
},{
    os << m_value;
},{
    return NEWNODE(ExprNode_Integer, m_value, m_datatype);
})
NODE(ExprNode_Float, {
    s % m_datatype;
    s.item(m_value);
},{
    os << m_value;
},{
    return NEWNODE(ExprNode_Float, m_value, m_datatype);
})
NODE(ExprNode_Bool, {
    s.item(m_value);
},{
    os << m_value;
},{
    return NEWNODE(ExprNode_Bool, m_value);
})
NODE(ExprNode_String, {
    s.item(m_value);
},{
    os << "\"" << m_value << "\"";
},{
    return NEWNODE(ExprNode_String, m_value);
})
NODE(ExprNode_ByteString, {
    s.item(m_value);
},{
    os << "b\"" << m_value << "\"";
},{
    return NEWNODE(ExprNode_ByteString, m_value);
})

NODE(ExprNode_Closure, {
    s.item(m_args);
    s.item(m_return);
    s.item(m_code);
},{
    os << "/* todo: closure */";
},{
    ExprNode_Closure::args_t    args;
    for(const auto& a : m_args) {
        args.push_back( ::std::make_pair(a.first.clone(), TypeRef(a.second)) );
    }
    return NEWNODE(ExprNode_Closure, mv$(args), TypeRef(m_return), m_code->clone());
});

NODE(ExprNode_StructLiteral, {
    s.item(m_path);
    s.item(m_base_value);
    s.item(m_values);
},{
    os << "/* todo: sl */";
},{
    ExprNode_StructLiteral::t_values    vals;
    
    for(const auto& v : m_values) {
        vals.push_back( ::std::make_pair(v.first, v.second->clone()) );
    }
    
    return NEWNODE(ExprNode_StructLiteral, AST::Path(m_path), OPT_CLONE(m_base_value), mv$(vals) );
})

NODE(ExprNode_Array, {
    s.item(m_size);
    s.item(m_values);
},{
    os << "[";
    if( m_size.get() )
        os << *m_values[0] << "; " << *m_size;
    else
        for(const auto& a : m_values)
            os << *a << ",";
    os << "]";
},{
    if( m_size.get() )
    {
        return NEWNODE(ExprNode_Array, m_values[0]->clone(), m_size->clone());
    }
    else
    {
        ::std::vector<ExprNodeP>    nodes;
        for(const auto& n : m_values)
            nodes.push_back( n->clone() );
        return NEWNODE(ExprNode_Array, mv$(nodes));
    }
})

NODE(ExprNode_Tuple, {
    s.item(m_values);
},{
    os << "(";
    for(const auto& a : m_values) {
        os << *a << ",";
    }
    os << ")";
},{
    ::std::vector<ExprNodeP>    nodes;
    for(const auto& n : m_values)
        nodes.push_back( n->clone() );
    return NEWNODE(ExprNode_Tuple, mv$(nodes));
})

NODE(ExprNode_NamedValue, {
    s.item(m_path);
},{
    os << m_path;
},{
    return NEWNODE(ExprNode_NamedValue, AST::Path(m_path));
})

NODE(ExprNode_Field, {
    s.item(m_obj);
    s.item(m_name);
},{
    os << "(" << *m_obj << ")." << m_name;
},{
    return NEWNODE(ExprNode_Field, m_obj->clone(), m_name);
})

NODE(ExprNode_Index, {
    s.item(m_obj);
    s.item(m_idx);
},{
    os << "(" << *m_obj << ")[" << *m_idx << "]";
},{
    return NEWNODE(ExprNode_Index, m_obj->clone(), m_idx->clone());
})

NODE(ExprNode_Deref, {
    s.item(m_value);
},{
    os << "*(" << *m_value << ")";
},{
    return NEWNODE(ExprNode_Deref, m_value->clone());
});

NODE(ExprNode_Cast, {
    s.item(m_value);
    s.item(m_type);
},{
    os << "(" << *m_value << " as " << m_type << ")";
},{
    return NEWNODE(ExprNode_Cast, m_value->clone(), TypeRef(m_type));
})

void operator%(::Serialiser& s, const ExprNode_BinOp::Type& t) {
    switch(t)
    {
    #define _(v)    case ExprNode_BinOp::v: s << #v; return
    _(CMPEQU);
    _(CMPNEQU);
    _(CMPLT);
    _(CMPLTE);
    _(CMPGT);
    _(CMPGTE);
    _(RANGE);
    _(RANGE_INC);
    _(BOOLAND);
    _(BOOLOR);
    _(BITAND);
    _(BITOR);
    _(BITXOR);
    _(SHL);
    _(SHR);
    _(MULTIPLY);
    _(DIVIDE);
    _(MODULO);
    _(ADD);
    _(SUB);
    _(PLACE_IN);
    #undef _
    }
}
void operator%(::Deserialiser& s, ExprNode_BinOp::Type& t) {
    ::std::string   n;
    s.item(n);
    if(0)   ;
    #define _(v)    else if(n == #v) t = ExprNode_BinOp::v
    _(CMPEQU);
    _(CMPNEQU);
    _(CMPLT);
    _(CMPLTE);
    _(CMPGT);
    _(CMPGTE);
    _(RANGE);
    _(RANGE_INC);
    _(BOOLAND);
    _(BOOLOR);
    _(BITAND);
    _(BITOR);
    _(BITXOR);
    _(SHL);
    _(SHR);
    _(MULTIPLY);
    _(DIVIDE);
    _(MODULO);
    _(ADD);
    _(SUB);
    #undef _
    else
        throw ::std::runtime_error("");
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
    case CMPLT:     os << "<";  break;
    case CMPLTE:    os << "<="; break;
    case CMPGT:     os << ">";  break;
    case CMPGTE:    os << ">="; break;
    case BOOLAND:   os << "&&"; break;
    case BOOLOR:    os << "||"; break;
    case BITAND:    os << "&"; break;
    case BITOR:     os << "|"; break;
    case BITXOR:    os << "^"; break;
    case SHR:    os << ">>"; break;
    case SHL:    os << "<<"; break;
    case MULTIPLY: os << "*"; break;
    case DIVIDE:   os << "/"; break;
    case MODULO:   os << "%"; break;
    case ADD:   os << "+"; break;
    case SUB:   os << "-"; break;
    case RANGE:   os << ".."; break;
    case RANGE_INC:   os << "..."; break;
    case PLACE_IN:  os << "<-"; break;
    }
    os << " " << *m_right << ")";
},{
    return NEWNODE(ExprNode_BinOp, m_type, OPT_CLONE(m_left), OPT_CLONE(m_right));
})

void operator%(::Serialiser& s, const ExprNode_UniOp::Type t) {
    switch(t)
    {
    #define _(v)    case ExprNode_UniOp::v: s << #v; return;
    _(NEGATE)
    _(INVERT)
    _(BOX)
    _(REF)
    _(REFMUT)
    _(QMARK)
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
    _(REFMUT)
    _(QMARK)
    #undef _
    else
        throw ::std::runtime_error( FMT("No uniop type for '" << n << "'") );
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
    case REFMUT: os << "(&mut "; break;
    case QMARK: os << "(" << *m_value << "?)"; return;
    }
    os << *m_value << ")";
},{
    return NEWNODE(ExprNode_UniOp, m_type, m_value->clone());
})


#define NV(type, actions)\
    void NodeVisitorDef::visit(type& node) { DEBUG("DEF - "#type); actions }
//  void NodeVisitorDef::visit(const type& node) { DEBUG("DEF - "#type" (const)"); actions }

NV(ExprNode_Block, {
    INDENT();
    for( auto& child : node.m_nodes )
        visit(child);
    UNINDENT();
})
NV(ExprNode_Macro,
{
    BUG(node.get_pos(), "Hit unexpanded macro in expression");
})
NV(ExprNode_Flow,
{
    visit(node.m_value);
})
NV(ExprNode_LetBinding,
{
    // TODO: Handle recurse into Let pattern?
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
NV(ExprNode_Loop,
{
    INDENT();
    visit(node.m_cond);
    visit(node.m_code);
    UNINDENT();
})
NV(ExprNode_Match,
{
    INDENT();
    visit(node.m_val);
    for( auto& arm : node.m_arms )
    {
        visit(arm.m_cond);
        visit(arm.m_code);
    }
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
NV(ExprNode_IfLet,
{
    INDENT();
    visit(node.m_value);
    visit(node.m_true);
    visit(node.m_false);
    UNINDENT();
})

NV(ExprNode_Integer, {(void)node;})
NV(ExprNode_Float, {(void)node;})
NV(ExprNode_Bool, {(void)node;})
NV(ExprNode_String, {(void)node;})
NV(ExprNode_ByteString, {(void)node;})

NV(ExprNode_Closure,
{
    visit(node.m_code);
});
NV(ExprNode_StructLiteral,
{
    visit(node.m_base_value);
    for( auto& val : node.m_values )
        visit(val.second);
})
NV(ExprNode_Array,
{
    visit(node.m_size);
    for( auto& val : node.m_values )
        visit(val);
})
NV(ExprNode_Tuple,
{
    for( auto& val : node.m_values )
        visit(val);
})
NV(ExprNode_NamedValue,
{
    (void)node;
    // LEAF
})

NV(ExprNode_Field,
{
    visit(node.m_obj);
})
NV(ExprNode_Index,
{
    visit(node.m_obj);
    visit(node.m_idx);
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

