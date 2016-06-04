/*
 */
#pragma once

#include <memory>
#include <hir/pattern.hpp>
#include <hir/type.hpp>
#include <span.hpp>

namespace HIR {

class ExprVisitor;

class ExprNode
{
public:
    ::HIR::TypeRef    m_res_type;

    const Span& span() const;
    
    virtual void visit(ExprVisitor& v) = 0;
    ExprNode()
    {}
    ExprNode(::HIR::TypeRef ty):
        m_res_type( mv$(ty) )
    {}
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
    ::std::vector< ::HIR::SimplePath>   m_traits;
    
    ExprNode_Block():
        m_is_unsafe(false)
    {}
    ExprNode_Block(bool is_unsafe, ::std::vector<ExprNodeP> nodes):
        m_is_unsafe(is_unsafe),
        m_nodes( mv$(nodes) )
    {}
    
    NODE_METHODS();
};
struct ExprNode_Return:
    public ExprNode
{
    ::HIR::ExprNodeP    m_value;
    
    ExprNode_Return(::HIR::ExprNodeP value):
        ExprNode(::HIR::TypeRef::Data::make_Diverge({})),
        m_value( mv$(value) )
    {
    }
    
    NODE_METHODS();
};
struct ExprNode_Loop:
    public ExprNode
{
    ::std::string   m_label;
    ::HIR::ExprNodeP    m_code;
    
    ExprNode_Loop(::std::string label, ::HIR::ExprNodeP code):
        ExprNode(::HIR::TypeRef(TypeRef::TagUnit{})),
        m_label( mv$(label) ),
        m_code( mv$(code) )
    {}
    
    NODE_METHODS();
};
struct ExprNode_LoopControl:
    public ExprNode
{
    ::std::string   m_label;
    bool    m_continue;
    //::HIR::ExprNodeP    m_value;

    ExprNode_LoopControl(::std::string label, bool cont):
        ExprNode(::HIR::TypeRef::Data::make_Diverge({})),
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
        ExprNode(::HIR::TypeRef(TypeRef::TagUnit{})),
        m_pattern( mv$(pat) ),
        m_type( mv$(ty) ),
        m_value( mv$(val) )
    {}
    
    NODE_METHODS();
};

struct ExprNode_Match:
    public ExprNode
{
    struct Arm
    {
        ::std::vector< ::HIR::Pattern>  m_patterns;
        ::HIR::ExprNodeP    m_cond;
        ::HIR::ExprNodeP    m_code;
    };
    
    ::HIR::ExprNodeP    m_value;
    ::std::vector<Arm> m_arms;

    ExprNode_Match(::HIR::ExprNodeP val, ::std::vector<Arm> arms):
        m_value( mv$(val) ),
        m_arms( mv$(arms) )
    {}

    NODE_METHODS();
};

struct ExprNode_If:
    public ExprNode
{
    ::HIR::ExprNodeP    m_cond;
    ::HIR::ExprNodeP    m_true;
    ::HIR::ExprNodeP    m_false;
    
    ExprNode_If(::HIR::ExprNodeP cond, ::HIR::ExprNodeP true_code, ::HIR::ExprNodeP false_code):
        m_cond( mv$(cond) ),
        m_true( mv$(true_code) ),
        m_false( mv$(false_code) )
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
        ExprNode(::HIR::TypeRef(TypeRef::TagUnit{})),
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
    {
        switch(m_op)
        {
        case Op::BoolAnd:   case Op::BoolOr:
        case Op::CmpEqu:    case Op::CmpNEqu:
        case Op::CmpLt: case Op::CmpLtE:
        case Op::CmpGt: case Op::CmpGtE:
            m_res_type = ::HIR::TypeRef( ::HIR::CoreType::Bool );
            break;
        default:
            break;
        }
    }
    
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
struct ExprNode_Unsize:
    public ExprNode
{
    ::HIR::ExprNodeP    m_value;
    ::HIR::TypeRef  m_type;

    ExprNode_Unsize(::HIR::ExprNodeP value, ::HIR::TypeRef dst_type):
        m_value( mv$(value) ),
        m_type( mv$(dst_type) )
    {}
    
    NODE_METHODS();
};
struct ExprNode_Index:
    public ExprNode
{
    ::HIR::ExprNodeP    m_val;
    ::HIR::ExprNodeP    m_index;
    
    ExprNode_Index(::HIR::ExprNodeP val, ::HIR::ExprNodeP index):
        m_val( mv$(val) ),
        m_index( mv$(index) )
    {}
    
    NODE_METHODS();
};
struct ExprNode_Deref:
    public ExprNode
{
    ::HIR::ExprNodeP    m_val;
    
    ExprNode_Deref(::HIR::ExprNodeP val):
        m_val( mv$(val) )
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
struct ExprNode_CallValue:
    public ExprNode
{
    ::HIR::ExprNodeP m_val;
    ::std::vector<ExprNodeP> m_args;
    
    ExprNode_CallValue(::HIR::ExprNodeP val, ::std::vector< ::HIR::ExprNodeP> args):
        m_val( mv$(val) ),
        m_args( mv$(args) )
    {}
    
    NODE_METHODS();
};
struct ExprNode_CallMethod:
    public ExprNode
{
    ::HIR::ExprNodeP    m_val;
    ::std::string   m_method;
    ::HIR::PathParams  m_params;
    ::std::vector< ::HIR::ExprNodeP>    m_args;

    ExprNode_CallMethod() {}
    ExprNode_CallMethod(::HIR::ExprNodeP val, ::std::string method_name, ::HIR::PathParams params, ::std::vector< ::HIR::ExprNodeP> args):
        m_val( mv$(val) ),
        m_method( mv$(method_name) ),
        m_params( mv$(params) ),
        m_args( mv$(args) )
    {
    }
    
    NODE_METHODS();
};
struct ExprNode_Field:
    public ExprNode
{
    ::HIR::ExprNodeP    m_val;
    ::std::string   m_field;
    
    ExprNode_Field(::HIR::ExprNodeP val, ::std::string field):
        m_val( mv$(val) ),
        m_field( mv$(field) )
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
    {
        TU_MATCH(Data, (m_data), (e),
        (Integer,
            if( e.m_type != ::HIR::CoreType::Str ) {
                m_res_type = ::HIR::TypeRef::Data::make_Primitive(e.m_type);
            }
            ),
        (Float,
            if( e.m_type != ::HIR::CoreType::Str ) {
                m_res_type = ::HIR::TypeRef::Data::make_Primitive(e.m_type);
            }
            ),
        (Boolean,
            m_res_type = ::HIR::TypeRef::Data::make_Primitive( ::HIR::CoreType::Bool );
            ),
        (String,
            m_res_type = ::HIR::TypeRef::Data::make_Borrow({
                ::HIR::BorrowType::Shared,
                box$( ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Primitive(::HIR::CoreType::Str) ) )
                });
            ),
        (ByteString,
            m_res_type = ::HIR::TypeRef::Data::make_Borrow({
                ::HIR::BorrowType::Shared,
                box$( ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Array({
                    box$( ::HIR::TypeRef(::HIR::TypeRef::Data::make_Primitive(::HIR::CoreType::U8)) ),
                    ::HIR::ExprPtr(),
                    e.size()
                    }) ) )
                });
            )
        )
    }
    
    NODE_METHODS();
};
struct ExprNode_PathValue:
    public ExprNode
{
    ::HIR::Path m_path;
    
    ExprNode_PathValue(::HIR::Path path):
        m_path( mv$(path) )
    {}
    
    NODE_METHODS();
};
struct ExprNode_Variable:
    public ExprNode
{
    ::std::string   m_name;
    unsigned int    m_slot;
    
    ExprNode_Variable(::std::string name, unsigned int slot):
        m_name( mv$(name) ),
        m_slot( slot )
    {}
    
    NODE_METHODS();
};

struct ExprNode_StructLiteral:
    public ExprNode
{
    typedef ::std::vector< ::std::pair< ::std::string, ExprNodeP > > t_values;
    
    ::HIR::GenericPath  m_path;
    ::HIR::ExprNodeP    m_base_value;
    t_values    m_values;
    
    ExprNode_StructLiteral(::HIR::GenericPath path, ::HIR::ExprNodeP base_value, t_values values):
        m_path( mv$(path) ),
        m_base_value( mv$(base_value) ),
        m_values( mv$(values) )
    {
        // TODO: set m_res_type based on path
    }
    
    NODE_METHODS();
};
struct ExprNode_Tuple:
    public ExprNode
{
    ::std::vector< ::HIR::ExprNodeP>    m_vals;
    
    ExprNode_Tuple(::std::vector< ::HIR::ExprNodeP> vals):
        m_vals( mv$(vals) )
    {}
    
    NODE_METHODS();
};
struct ExprNode_ArrayList:
    public ExprNode
{
    ::std::vector< ::HIR::ExprNodeP>    m_vals;
    
    ExprNode_ArrayList(::std::vector< ::HIR::ExprNodeP> vals):
        ExprNode( ::HIR::TypeRef::Data::make_Array({
            box$( ::HIR::TypeRef() ),
            ::HIR::ExprPtr(),
            vals.size()
            }) ),
        m_vals( mv$(vals) )
    {}
    
    NODE_METHODS();
};
struct ExprNode_ArraySized:
    public ExprNode
{
    ::HIR::ExprNodeP    m_val;
    ::HIR::ExprNodeP    m_size; // TODO: Has to be constant
    size_t  m_size_val;
    
    ExprNode_ArraySized(::HIR::ExprNodeP val, ::HIR::ExprNodeP size):
        m_val( mv$(val) ),
        m_size( mv$(size) ),
        m_size_val( ~0u )
    {}
    
    NODE_METHODS();
};

struct ExprNode_Closure:
    public ExprNode
{
    typedef ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >   args_t;
    
    args_t  m_args;
    ::HIR::TypeRef  m_return;
    ::HIR::ExprNodeP    m_code;
    
    ExprNode_Closure(args_t args, ::HIR::TypeRef rv, ::HIR::ExprNodeP code):
        m_args( ::std::move(args) ),
        m_return( ::std::move(rv) ),
        m_code( ::std::move(code) )
    {}
    
    NODE_METHODS();
};

#undef NODE_METHODS

class ExprVisitor
{
public:
    virtual void visit_node(ExprNode& node);
    #define NV(nt)  virtual void visit(nt& n) = 0;
    
    NV(ExprNode_Block)
    NV(ExprNode_Return)
    NV(ExprNode_Let)
    NV(ExprNode_Loop)
    NV(ExprNode_LoopControl)
    NV(ExprNode_Match)
    NV(ExprNode_If)
    
    NV(ExprNode_Assign)
    NV(ExprNode_BinOp)
    NV(ExprNode_UniOp)
    NV(ExprNode_Cast)   // Conversion
    NV(ExprNode_Unsize) // Coercion
    NV(ExprNode_Index)
    NV(ExprNode_Deref)
    
    NV(ExprNode_CallPath);
    NV(ExprNode_CallValue);
    NV(ExprNode_CallMethod);
    NV(ExprNode_Field);

    NV(ExprNode_Literal);
    NV(ExprNode_PathValue);
    NV(ExprNode_Variable);
    
    NV(ExprNode_StructLiteral);
    NV(ExprNode_Tuple);
    NV(ExprNode_ArrayList);
    NV(ExprNode_ArraySized);
    
    NV(ExprNode_Closure);
    #undef NV
};

class ExprVisitorDef:
    public ExprVisitor
{
public:
    #define NV(nt)  virtual void visit(nt& n);
    
    NV(ExprNode_Block)
    NV(ExprNode_Return)
    NV(ExprNode_Let)
    NV(ExprNode_Loop)
    NV(ExprNode_LoopControl)
    NV(ExprNode_Match)
    NV(ExprNode_If)
    
    NV(ExprNode_Assign)
    NV(ExprNode_BinOp)
    NV(ExprNode_UniOp)
    NV(ExprNode_Cast)
    NV(ExprNode_Unsize)
    NV(ExprNode_Index)
    NV(ExprNode_Deref)
    
    NV(ExprNode_CallPath);
    NV(ExprNode_CallValue);
    NV(ExprNode_CallMethod);
    NV(ExprNode_Field);

    NV(ExprNode_Literal);
    NV(ExprNode_PathValue);
    NV(ExprNode_Variable);
    
    NV(ExprNode_StructLiteral);
    NV(ExprNode_Tuple);
    NV(ExprNode_ArrayList);
    NV(ExprNode_ArraySized);
    
    NV(ExprNode_Closure);
    #undef NV
};

}

