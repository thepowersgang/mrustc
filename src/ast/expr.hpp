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
public:
    virtual ~ExprNode() = 0;
    
    virtual void visit(NodeVisitor& nv) = 0;
    
    static ::std::unique_ptr<ExprNode> from_deserialiser(Deserialiser& d);
};

struct ExprNode_Block:
    public ExprNode
{
    ::std::vector< ::std::unique_ptr<ExprNode> >    m_nodes;

    ExprNode_Block() {}
    ExprNode_Block(::std::vector< ::std::unique_ptr<ExprNode> >&& nodes):
        m_nodes( move(nodes) )
    {
    }
    virtual ~ExprNode_Block() override;
    
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
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
    
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
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
    
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
};
struct ExprNode_LetBinding:
    public ExprNode
{
    Pattern m_pat;
    unique_ptr<ExprNode>    m_value;

    ExprNode_LetBinding() {}
    ExprNode_LetBinding(Pattern pat, unique_ptr<ExprNode>&& value):
        m_pat( move(pat) ),
        m_value( move(value) )
    {
    }
    
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
};
struct ExprNode_Assign:
    public ExprNode
{
    unique_ptr<ExprNode>    m_slot;
    unique_ptr<ExprNode>    m_value;

    ExprNode_Assign() {}
    ExprNode_Assign(unique_ptr<ExprNode>&& slot, unique_ptr<ExprNode>&& value):
        m_slot( move(slot) ),
        m_value( move(value) )
    {
    }
    
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
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
    
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
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
    
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
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
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
};

struct ExprNode_Match:
    public ExprNode
{
    typedef ::std::vector< ::std::pair<Pattern,unique_ptr<ExprNode> > > arm_t;
    unique_ptr<ExprNode>    m_val;
    arm_t   m_arms;

    ExprNode_Match() {}
    ExprNode_Match(unique_ptr<ExprNode>&& val, arm_t&& arms):
        m_val( ::std::move(val) ),
        m_arms( ::std::move(arms) )
    {
    }
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
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
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
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
    
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
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
    
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
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
    
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
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
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
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
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
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
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
};

// Binary operation
struct ExprNode_BinOp:
    public ExprNode
{
    enum Type {
        CMPEQU,
        CMPNEQU,

        BITAND,
        BITOR,
        BITXOR,

        SHL,
        SHR,
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
    
    virtual void visit(NodeVisitor& nv) override;

    SERIALISABLE_PROTOTYPES();
};

class NodeVisitor
{
public:
    void visit(const unique_ptr<ExprNode>& cnode) {
        if(cnode.get())
            cnode->visit(*this);
    }
    
    virtual void visit(ExprNode_Block& node) {
        for( auto& child : node.m_nodes )
            visit(child);
    }
    virtual void visit(ExprNode_Macro& node) {
    }
    virtual void visit(ExprNode_Return& node) {
        visit(node.m_value);
    }
    virtual void visit(ExprNode_LetBinding& node) {
        // TODO: Handle recurse into Let pattern
        visit(node.m_value);
    }
    virtual void visit(ExprNode_Assign& node) {
        visit(node.m_slot);
        visit(node.m_value);
    }
    virtual void visit(ExprNode_CallPath& node) {
        for( auto& arg : node.m_args )
            visit(arg);
    }
    virtual void visit(ExprNode_CallMethod& node) {
        visit(node.m_val);
        for( auto& arg : node.m_args )
            visit(arg);
    }
    virtual void visit(ExprNode_CallObject& node) {
        visit(node.m_val);
        for( auto& arg : node.m_args )
            visit(arg);
    }
    virtual void visit(ExprNode_Match& node) {
        visit(node.m_val);
        for( auto& arm : node.m_arms )
            visit(arm.second);
    }
    virtual void visit(ExprNode_If& node) {
        visit(node.m_cond);
        visit(node.m_true);
        visit(node.m_false);
    }
    
    virtual void visit(ExprNode_Integer& node) {
        // LEAF
    }
    virtual void visit(ExprNode_StructLiteral& node) {
        visit(node.m_base_value);
        for( auto& val : node.m_values )
            visit(val.second);
    }
    virtual void visit(ExprNode_Tuple& node) {
        for( auto& val : node.m_values )
            visit(val);
    }
    virtual void visit(ExprNode_NamedValue& node) {
        // LEAF
    }
    
    virtual void visit(ExprNode_Field& node) {
        visit(node.m_obj);
    }
    virtual void visit(ExprNode_Cast& node) {
        visit(node.m_value);
    }
    virtual void visit(ExprNode_BinOp& node) {
        visit(node.m_left);
        visit(node.m_right);
    }
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
    ::std::shared_ptr<ExprNode> take_node() { assert(m_node.get()); return ::std::move(m_node); }
    void visit_nodes(NodeVisitor& v);

    friend ::std::ostream& operator<<(::std::ostream& os, const Expr& pat);
    
    SERIALISABLE_PROTOTYPES();
};

}

#endif

