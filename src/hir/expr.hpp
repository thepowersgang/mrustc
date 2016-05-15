/*
 */
#pragma once

#include <memory>
#include <hir/pattern.hpp>
#include <hir/type.hpp>

namespace HIR {

class ExprVisitor;

class ExprNode
{
public:
    virtual void visit(ExprVisitor& v) = 0;
    virtual ~ExprNode();
};

typedef ::std::unique_ptr<ExprNode> ExprNodeP;

#define NODE_METHODS()  \
    virtual void visit(ExprVisitor& nv) override;

struct ExprNode_Block:
    public ExprNode
{
    bool    m_is_unsafe;
    ::std::vector< ExprNodeP >  m_nodes;
    
    NODE_METHODS();
};
struct ExprNode_Return:
    public ExprNode
{
    ::HIR::ExprNodeP    m_value;
    
    ExprNode_Return(::HIR::ExprNodeP v):
        m_value( mv$(m_value) )
    {}
    
    NODE_METHODS();
};
struct ExprNode_Loop:
    public ExprNode
{
    ::std::string   m_label;
    ::HIR::ExprNodeP    m_code;
    
    NODE_METHODS();
};
struct ExprNode_LoopControl:
    public ExprNode
{
    ::std::string   m_label;
    bool    m_continue;
    //::HIR::ExprNodeP    m_value;

    ExprNode_LoopControl(::std::string label, bool cont):
        m_label( mv$(label) ),
        m_continue( cont )
    {}
    
    NODE_METHODS();
};
struct ExprNode_Let:
    public ExprNode
{
    ::HIR::Pattern  m_pattern;
    ::HIR::TypeRef  m_type;
    ::HIR::ExprNodeP    m_value;
    
    ExprNode_Let(::HIR::Pattern pat, ::HIR::TypeRef ty, ::HIR::ExprNodeP val):
        m_pattern( mv$(pat) ),
        m_type( mv$(ty) ),
        m_value( mv$(m_value) )
    {}
    
    NODE_METHODS();
};

struct ExprNode_Assign:
    public ExprNode
{
    enum class Op {
        None,
        Add, Sub,
        Mul, Div, Mod,
        And, Or , Xor,
        Shr, Shl,
    };
    
    Op  m_op;
    ExprNodeP   m_slot;
    ExprNodeP   m_value;

    ExprNode_Assign(Op op, ::HIR::ExprNodeP slot, ::HIR::ExprNodeP value):
        m_op(op),
        m_slot( mv$(slot) ),
        m_value( mv$(value) )
    {}
    
    NODE_METHODS();
};
struct ExprNode_BinOp:
    public ExprNode
{
    enum class Op {
        CmpEqu,
        CmpNEqu,
        CmpLt,
        CmpLtE,
        CmpGt,
        CmpGtE,
        
        BoolAnd,
        BoolOr,

        Add, Sub,
        Mul, Div, Mod,
        And, Or , Xor,
        Shr, Shl,
    };

    Op    m_op;
    ::HIR::ExprNodeP m_left;
    ::HIR::ExprNodeP m_right;

    ExprNode_BinOp() {}
    ExprNode_BinOp(Op op, ::HIR::ExprNodeP left, ::HIR::ExprNodeP right):
        m_op(op),
        m_left( mv$(left) ),
        m_right( mv$(right) )
    {}
    
    NODE_METHODS();
};
struct ExprNode_UniOp:
    public ExprNode
{
    enum class Op {
        Ref,    // '& <expr>'
        RefMut, // '&mut <expr>'
        Invert, // '!<expr>'
        Negate, // '-<expr>'
    };
    
    Op  m_op;
    ::HIR::ExprNodeP    m_value;

    ExprNode_UniOp(Op op, ::HIR::ExprNodeP value):
        m_op(op),
        m_value( mv$(value) )
    {}
    
    NODE_METHODS();
};
struct ExprNode_Cast:
    public ExprNode
{
    ::HIR::ExprNodeP    m_value;
    ::HIR::TypeRef  m_type;

    ExprNode_Cast(::HIR::ExprNodeP value, ::HIR::TypeRef dst_type):
        m_value( mv$(value) ),
        m_type( mv$(dst_type) )
    {}
    NODE_METHODS();
};

struct ExprNode_CallPath:
    public ExprNode
{
    ::HIR::Path m_path;
    ::std::vector<ExprNodeP> m_args;
    
    ExprNode_CallPath(::HIR::Path path, ::std::vector< ::HIR::ExprNodeP> args):
        m_path( mv$(path) ),
        m_args( mv$(args) )
    {}
    
    NODE_METHODS();
};

struct ExprNode_Literal:
    public ExprNode
{
    TAGGED_UNION(Data, Integer,
    (Integer, struct {
        ::HIR::CoreType m_type; // if not an integer type, it's unknown
        uint64_t m_value;
        }),
    (Float, struct {
        ::HIR::CoreType m_type; // If not a float type, it's unknown
        double  m_value;
        }),
    (Boolean, bool),
    (String, ::std::string),
    (ByteString, ::std::vector<char>)
    );

    Data m_data;

    ExprNode_Literal(Data data):
        m_data( mv$(data) )
    {}
    
    NODE_METHODS();
};

#undef NODE_METHODS

class ExprVisitor
{
public:
    #define NV(nt)  virtual void visit(nt& n) = 0;
    
    NV(ExprNode_Block)
    NV(ExprNode_Return)
    NV(ExprNode_Let)
    NV(ExprNode_Loop)
    NV(ExprNode_LoopControl)
    
    NV(ExprNode_Assign)
    NV(ExprNode_BinOp)
    NV(ExprNode_UniOp)
    NV(ExprNode_Cast)
    
    NV(ExprNode_CallPath);

    NV(ExprNode_Literal);
};

}

