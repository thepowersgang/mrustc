/*
 */
#ifndef AST_EXPR_INCLUDED
#define AST_EXPR_INCLUDED

#include <ostream>
#include <memory>   // unique_ptr
#include <vector>

#include "../parse/tokentree.hpp"
#include "../types.hpp"
#include "pattern.hpp"

namespace AST {

using ::std::unique_ptr;

class NodeVisitor;

class ExprNode:
    public Serialisable
{
    TypeRef m_res_type;
public:
    virtual ~ExprNode() = 0;
    
    virtual void visit(NodeVisitor& nv) = 0;
    //virtual void visit(NodeVisitor& nv) const = 0;
    virtual void print(::std::ostream& os) const = 0;
    
    TypeRef& get_res_type() { return m_res_type; }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const ExprNode& node);
    static ::std::unique_ptr<ExprNode> from_deserialiser(Deserialiser& d);
};

#define NODE_METHODS()  \
	virtual void visit(NodeVisitor& nv) override;\
	virtual void print(::std::ostream& os) const override; \
	SERIALISABLE_PROTOTYPES();/* \
	virtual void visit(NodeVisitor& nv) const override;*/

struct ExprNode_Block:
    public ExprNode
{
	bool m_is_unsafe;
    ::std::vector< ::std::unique_ptr<ExprNode> >    m_nodes;

    ExprNode_Block():
		m_is_unsafe(false)
	{}
    ExprNode_Block(::std::vector< ::std::unique_ptr<ExprNode> >&& nodes):
		m_is_unsafe(false),
        m_nodes( move(nodes) )
    {
    }
    virtual ~ExprNode_Block() override;
    
	void set_unsafe() { m_is_unsafe = true; }
	
    NODE_METHODS();
};

struct ExprNode_Macro:
    public ExprNode
{
    ::std::string   m_name;
    ::TokenTree m_tokens;
    
    ExprNode_Macro() {}
    ExprNode_Macro(::std::string name, ::TokenTree&& tokens):
        m_name(name),
        m_tokens( move(tokens) )
    {}
    
    NODE_METHODS();
};

// Return a value
struct ExprNode_Return:
    public ExprNode
{
    unique_ptr<ExprNode>    m_value;

    ExprNode_Return() {}
    ExprNode_Return(unique_ptr<ExprNode>&& value):
        m_value( move(value) )
    {
    }
    
    NODE_METHODS();
};
struct ExprNode_Const:
    public ExprNode
{
    ::std::string m_name;
    TypeRef m_type;
    unique_ptr<ExprNode>    m_value;

    ExprNode_Const() {}
    ExprNode_Const(::std::string name, TypeRef type, unique_ptr<ExprNode>&& value):
        m_name( move(name) ),
        m_type( move(type) ),
        m_value( move(value) )
    {
    }
    
    NODE_METHODS();
};
struct ExprNode_LetBinding:
    public ExprNode
{
    Pattern m_pat;
    TypeRef m_type;
    unique_ptr<ExprNode>    m_value;

    ExprNode_LetBinding() {}
    ExprNode_LetBinding(Pattern pat, TypeRef type, unique_ptr<ExprNode>&& value):
        m_pat( move(pat) ),
        m_type( move(type) ),
        m_value( move(value) )
    {
    }
    
    NODE_METHODS();
};
struct ExprNode_Assign:
    public ExprNode
{
    enum Operation {
        NONE,
        ADD,
        SUB,
    } m_op;
    unique_ptr<ExprNode>    m_slot;
    unique_ptr<ExprNode>    m_value;

    ExprNode_Assign(): m_op(NONE) {}
    ExprNode_Assign(Operation op, unique_ptr<ExprNode>&& slot, unique_ptr<ExprNode>&& value):
        m_op(op),
        m_slot( move(slot) ),
        m_value( move(value) )
    {
    }
    
    NODE_METHODS();
};
struct ExprNode_CallPath:
    public ExprNode
{
    Path    m_path;
    ::std::vector<unique_ptr<ExprNode>> m_args;

    ExprNode_CallPath() {}
    ExprNode_CallPath(Path&& path, ::std::vector<unique_ptr<ExprNode>>&& args):
        m_path( move(path) ),
        m_args( move(args) )
    {
    }
    
    NODE_METHODS();
};
struct ExprNode_CallMethod:
    public ExprNode
{
    unique_ptr<ExprNode>    m_val;
    PathNode    m_method;
    ::std::vector<unique_ptr<ExprNode>> m_args;

    ExprNode_CallMethod() {}
    ExprNode_CallMethod(unique_ptr<ExprNode>&& obj, PathNode&& method, ::std::vector<unique_ptr<ExprNode>>&& args):
        m_val( move(obj) ),
        m_method( move(method) ),
        m_args( move(args) )
    {
    }
    
    NODE_METHODS();
};
// Call an object (Fn/FnMut/FnOnce)
struct ExprNode_CallObject:
    public ExprNode
{
    unique_ptr<ExprNode>    m_val;
    ::std::vector<unique_ptr<ExprNode>> m_args;

    ExprNode_CallObject() {}
    ExprNode_CallObject(unique_ptr<ExprNode>&& val, ::std::vector< unique_ptr<ExprNode> >&& args):
        m_val( move(val) ),
        m_args( move(args) )
    {
    }
    NODE_METHODS();
};

struct ExprNode_Match:
    public ExprNode
{
    struct Arm:
        public Serialisable
    {
        ::std::vector<Pattern>  m_patterns;
        unique_ptr<ExprNode>    m_cond;
        
        unique_ptr<ExprNode>    m_code;
    
        SERIALISABLE_PROTOTYPES();
    };
    
    unique_ptr<ExprNode>    m_val;
    ::std::vector<Arm>  m_arms;

    ExprNode_Match() {}
    ExprNode_Match(unique_ptr<ExprNode> val, ::std::vector<Arm> arms):
        m_val( ::std::move(val) ),
        m_arms( ::std::move(arms) )
    {
    }
    NODE_METHODS();
};

struct ExprNode_If:
    public ExprNode
{
    unique_ptr<ExprNode>    m_cond;
    unique_ptr<ExprNode>    m_true;
    unique_ptr<ExprNode>    m_false;

    ExprNode_If() {}
    ExprNode_If(unique_ptr<ExprNode>&& cond, unique_ptr<ExprNode>&& true_code, unique_ptr<ExprNode>&& false_code):
        m_cond( ::std::move(cond) ),
        m_true( ::std::move(true_code) ),
        m_false( ::std::move(false_code) )
    {
    }
    NODE_METHODS();
};
struct ExprNode_IfLet:
    public ExprNode
{
    AST::Pattern    m_pattern;
    unique_ptr<ExprNode>    m_value;
    unique_ptr<ExprNode>    m_true;
    unique_ptr<ExprNode>    m_false;

    ExprNode_IfLet() {}
    ExprNode_IfLet(AST::Pattern pattern, unique_ptr<ExprNode>&& cond, unique_ptr<ExprNode>&& true_code, unique_ptr<ExprNode>&& false_code):
        m_pattern( ::std::move(pattern) ),
        m_value( ::std::move(cond) ),
        m_true( ::std::move(true_code) ),
        m_false( ::std::move(false_code) )
    {
    }
    NODE_METHODS();
};
// Literal integer
struct ExprNode_Integer:
    public ExprNode
{
    enum eCoreType  m_datatype;
    uint64_t    m_value;

    ExprNode_Integer() {}
    ExprNode_Integer(uint64_t value, enum eCoreType datatype):
        m_datatype(datatype),
        m_value(value)
    {
    }
    
    NODE_METHODS();
};
// Literal float
struct ExprNode_Float:
    public ExprNode
{
    enum eCoreType  m_datatype;
    double  m_value;

    ExprNode_Float() {}
    ExprNode_Float(double value, enum eCoreType datatype):
        m_datatype(datatype),
        m_value(value)
    {
    }
    
    NODE_METHODS();
};
// Literal boolean
struct ExprNode_Bool:
    public ExprNode
{
    bool    m_value;

    ExprNode_Bool() {}
    ExprNode_Bool(bool value):
        m_value(value)
    {
    }
    
    NODE_METHODS();
};
// Literal structure
struct ExprNode_StructLiteral:
    public ExprNode
{
    typedef ::std::vector< ::std::pair< ::std::string, unique_ptr<ExprNode> > > t_values;
    Path    m_path;
    unique_ptr<ExprNode>    m_base_value;
    t_values    m_values;

    ExprNode_StructLiteral() {}
    ExprNode_StructLiteral(Path path, unique_ptr<ExprNode>&& base_value, t_values&& values ):
        m_path( move(path) ),
        m_base_value( move(base_value) ),
        m_values( move(values) )
    {}
    
    NODE_METHODS();
};
// Tuple
struct ExprNode_Tuple:
    public ExprNode
{
    ::std::vector< unique_ptr<ExprNode> >   m_values;
    
    ExprNode_Tuple() {}
    ExprNode_Tuple(::std::vector< unique_ptr<ExprNode> > vals):
        m_values( ::std::move(vals) )
    {}
    
    NODE_METHODS();
};
// Variable / Constant
struct ExprNode_NamedValue:
    public ExprNode
{
    Path    m_path;

    ExprNode_NamedValue() {}
    ExprNode_NamedValue(Path&& path):
        m_path( ::std::move(path) )
    {
    }
    NODE_METHODS();
};
// Field dereference
struct ExprNode_Field:
    public ExprNode
{
    ::std::unique_ptr<ExprNode> m_obj;
    ::std::string   m_name;

    ExprNode_Field() {}
    ExprNode_Field(::std::unique_ptr<ExprNode>&& obj, ::std::string&& name):
        m_obj( ::std::move(obj) ),
        m_name( ::std::move(name) )
    {
    }
    NODE_METHODS();
};

// Pointer dereference
struct ExprNode_Deref:
    public ExprNode
{
    ::std::unique_ptr<ExprNode>    m_value;
    
    ExprNode_Deref() {}
    ExprNode_Deref(::std::unique_ptr<ExprNode> value):
        m_value( ::std::move(value) )
    {
    }
    
    NODE_METHODS();
};

// Type cast ('as')
struct ExprNode_Cast:
    public ExprNode
{
    unique_ptr<ExprNode>    m_value;
    TypeRef m_type;

    ExprNode_Cast() {}
    ExprNode_Cast(unique_ptr<ExprNode>&& value, TypeRef&& dst_type):
        m_value( move(value) ),
        m_type( move(dst_type) )
    {
    }
    NODE_METHODS();
};

// Binary operation
struct ExprNode_BinOp:
    public ExprNode
{
    enum Type {
        CMPEQU,
        CMPNEQU,
		CMPLT,
		CMPLTE,
		CMPGT,
		CMPGTE,
        BOOLAND,
        BOOLOR,

        BITAND,
        BITOR,
        BITXOR,

        SHL,
        SHR,
	
		MULTIPLY,
		DIVIDE,
		MODULO,
		ADD,
		SUB,
    };

    Type    m_type;
    ::std::unique_ptr<ExprNode> m_left;
    ::std::unique_ptr<ExprNode> m_right;

    ExprNode_BinOp() {}
    ExprNode_BinOp(Type type, ::std::unique_ptr<ExprNode> left, ::std::unique_ptr<ExprNode> right):
        m_type(type),
        m_left( ::std::move(left) ),
        m_right( ::std::move(right) )
    {
    }
    
    NODE_METHODS();
};

struct ExprNode_UniOp:
    public ExprNode
{
    enum Type {
        REF,
        BOX,
        INVERT,
        NEGATE,
    };
    
    enum Type   m_type;
    ::std::unique_ptr<ExprNode> m_value;

    ExprNode_UniOp() {}
    ExprNode_UniOp(Type type, ::std::unique_ptr<ExprNode> value):
        m_type(type),
        m_value( ::std::move(value) )
    {
    }
    
    NODE_METHODS();
};

class NodeVisitor
{
public:
    inline void visit(const unique_ptr<ExprNode>& cnode) {
        if(cnode.get())
            cnode->visit(*this);
    }
	virtual bool is_const() const { return false; }
  
    #define NT(nt) \
		virtual void visit(nt& node) = 0/*; \
		virtual void visit(const nt& node) = 0*/
	NT(ExprNode_Block);  
    NT(ExprNode_Macro);
    NT(ExprNode_Return);
    NT(ExprNode_Const);
    NT(ExprNode_LetBinding);
    NT(ExprNode_Assign);
    NT(ExprNode_CallPath);
    NT(ExprNode_CallMethod);
    NT(ExprNode_CallObject);
    NT(ExprNode_Match);
    NT(ExprNode_If);
    NT(ExprNode_IfLet);
    
    NT(ExprNode_Integer);
    NT(ExprNode_Float);
    NT(ExprNode_Bool);
    NT(ExprNode_StructLiteral);
    NT(ExprNode_Tuple);
    NT(ExprNode_NamedValue);
    
    NT(ExprNode_Field);
    NT(ExprNode_Deref);
    NT(ExprNode_Cast);
    NT(ExprNode_BinOp);
    NT(ExprNode_UniOp);
	#undef NT
};
class NodeVisitorDef:
	public NodeVisitor
{
public:
    inline void visit(const unique_ptr<ExprNode>& cnode) {
        if(cnode.get())
            cnode->visit(*this);
    }
    #define NT(nt) \
		virtual void visit(nt& node) override;/* \
		virtual void visit(const nt& node) override*/
	NT(ExprNode_Block);  
    NT(ExprNode_Macro);
    NT(ExprNode_Return);
    NT(ExprNode_Const);
    NT(ExprNode_LetBinding);
    NT(ExprNode_Assign);
    NT(ExprNode_CallPath);
    NT(ExprNode_CallMethod);
    NT(ExprNode_CallObject);
    NT(ExprNode_Match);
    NT(ExprNode_If);
    NT(ExprNode_IfLet);
    
    NT(ExprNode_Integer);
    NT(ExprNode_Float);
    NT(ExprNode_Bool);
    NT(ExprNode_StructLiteral);
    NT(ExprNode_Tuple);
    NT(ExprNode_NamedValue);
    
    NT(ExprNode_Field);
    NT(ExprNode_Deref);
    NT(ExprNode_Cast);
    NT(ExprNode_BinOp);
    NT(ExprNode_UniOp);
	#undef NT
};

class Expr:
    public Serialisable
{
    ::std::shared_ptr<ExprNode> m_node;
public:
    Expr(unique_ptr<ExprNode> node):
        m_node(node.release())
    {
    }
    Expr(ExprNode* node):
        m_node(node)
    {
    }
    Expr():
        m_node(nullptr)
    {
    }

    bool is_valid() const { return m_node.get() != nullptr; }
    ExprNode& node() { assert(m_node.get()); return *m_node; }
    const ExprNode& node() const { assert(m_node.get()); return *m_node; }
    ::std::shared_ptr<ExprNode> take_node() { assert(m_node.get()); return ::std::move(m_node); }
    void visit_nodes(NodeVisitor& v);
    void visit_nodes(NodeVisitor& v) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const Expr& pat);
    
    SERIALISABLE_PROTOTYPES();
};

}

#endif

