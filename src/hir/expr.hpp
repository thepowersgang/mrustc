/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/expr.hpp
 * - HIR Expression nodes
 */
#pragma once

#include <memory>
#include <hir/pattern.hpp>
#include <hir/type.hpp>
#include <span.hpp>
#include <hir/visitor.hpp>
#include <hir_typeck/common.hpp>
#include "asm.hpp"

namespace HIR {

typedef ::std::vector< ::std::pair<const ::HIR::SimplePath*,const ::HIR::Trait*> >  t_trait_list;

// Indicates how a result is used
enum class ValueUsage {
    // Not yet known (defalt state)
    Unknown,
    // Value is borrowed (shared)
    Borrow,
    // Value is mutated or uniquely borrowed
    Mutate,
    // Value is moved
    Move,
};
static inline ::std::ostream& operator<<(::std::ostream& os, const ValueUsage& x) {
    switch(x)
    {
    case ValueUsage::Unknown: os << "Unknown"; break;
    case ValueUsage::Borrow:  os << "Borrow";  break;
    case ValueUsage::Mutate:  os << "Mutate";  break;
    case ValueUsage::Move:    os << "Move";    break;
    }
    return os;
}

class GenericParams;

class ExprVisitor;

class ExprNode
{
public:
    Span    m_span;
    ::HIR::TypeRef    m_res_type;   // TODO: Replace this with an index into an ivar table
    //unsigned m_res_type_idx;
    ValueUsage  m_usage = ValueUsage::Unknown;

    const Span& span() const { return m_span; }

    virtual void visit(ExprVisitor& v) = 0;
    ExprNode(Span sp):
        m_span( mv$(sp) )
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
    ExprNodeP   m_value_node;   // can be null

    ::HIR::SimplePath   m_local_mod;
    t_trait_list    m_traits;

    ExprNode_Block(Span sp):
        ExprNode(mv$(sp)),
        m_is_unsafe(false)
    {}
    ExprNode_Block(Span sp, bool is_unsafe, ::std::vector<ExprNodeP> nodes, ExprNodeP value_node):
        ExprNode( mv$(sp) ),
        m_is_unsafe(is_unsafe),
        m_nodes( mv$(nodes) ),
        m_value_node( mv$(value_node) )
    {}

    NODE_METHODS();
};
struct ExprNode_Asm:
    public ExprNode
{
    struct ValRef
    {
        ::std::string   spec;
        ::HIR::ExprNodeP    value;
    };
    ::std::string   m_template;
    ::std::vector<ValRef>   m_outputs;
    ::std::vector<ValRef>   m_inputs;
    ::std::vector< ::std::string>   m_clobbers;
    ::std::vector< ::std::string>   m_flags;

    ExprNode_Asm(Span sp, ::std::string tpl_str, ::std::vector<ValRef> outputs, ::std::vector<ValRef> inputs, ::std::vector< ::std::string> clobbers, ::std::vector< ::std::string> flags):
        ExprNode(mv$(sp)),
        m_template( mv$(tpl_str) ),
        m_outputs( mv$(outputs) ),
        m_inputs( mv$(inputs) ),
        m_clobbers( mv$(clobbers) ),
        m_flags( mv$(flags) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_Asm2:
    public ExprNode
{
    TAGGED_UNION(Param, Const,
        (Const, HIR::ExprNodeP),
        (Sym, HIR::Path),
        (RegSingle, struct {
            AsmCommon::Direction    dir;
            AsmCommon::RegisterSpec spec;
            HIR::ExprNodeP  val;
            }),
        (Reg, struct {
            AsmCommon::Direction    dir;
            AsmCommon::RegisterSpec spec;
            HIR::ExprNodeP  val_in;
            HIR::ExprNodeP  val_out;
            })
        );

    AsmCommon::Options  m_options;
    std::vector<AsmCommon::Line>   m_lines;
    std::vector<Param>  m_params;

    ExprNode_Asm2(Span sp, AsmCommon::Options options, std::vector<AsmCommon::Line> lines, std::vector<Param> params)
        : ExprNode(mv$(sp))
        , m_options(options)
        , m_lines( move(lines) )
        , m_params( move(params) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_Return:
    public ExprNode
{
    ::HIR::ExprNodeP    m_value;

    ExprNode_Return(Span sp, ::HIR::ExprNodeP value):
        ExprNode(mv$(sp)),
        m_value( mv$(value) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_Yield:
    public ExprNode
{
    ::HIR::ExprNodeP    m_value;

    ExprNode_Yield(Span sp, ::HIR::ExprNodeP value):
        ExprNode(mv$(sp)),
        m_value( mv$(value) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_Loop:
    public ExprNode
{
    RcString    m_label;
    ::HIR::ExprNodeP    m_code;
    bool    m_diverges = false;
    bool    m_require_label = false;

    ExprNode_Loop(Span sp, RcString label, ::HIR::ExprNodeP code):
        ExprNode(mv$(sp)),
        m_label( mv$(label) ),
        m_code( mv$(code) )
    {}

    NODE_METHODS();
};
struct ExprNode_LoopControl:
    public ExprNode
{
    RcString   m_label;
    bool    m_continue;
    ::HIR::ExprNodeP    m_value;

    ExprNode_LoopControl(Span sp, RcString label, bool cont, ::HIR::ExprNodeP value={}):
        ExprNode(mv$(sp)),
        m_label( mv$(label) ),
        m_continue( cont ),
        m_value( mv$(value) )
    {}

    NODE_METHODS();
};
struct ExprNode_Let:
    public ExprNode
{
    ::HIR::Pattern  m_pattern;
    ::HIR::TypeRef  m_type;
    ::HIR::ExprNodeP    m_value;

    ExprNode_Let(Span sp, ::HIR::Pattern pat, ::HIR::TypeRef ty, ::HIR::ExprNodeP val):
        ExprNode(mv$(sp)),
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

    ExprNode_Match(Span sp, ::HIR::ExprNodeP val, ::std::vector<Arm> arms):
        ExprNode( mv$(sp) ),
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

    ExprNode_If(Span sp, ::HIR::ExprNodeP cond, ::HIR::ExprNodeP true_code, ::HIR::ExprNodeP false_code):
        ExprNode( mv$(sp) ),
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
    static const char* opname(Op v) {
        switch(v)
        {
        case Op::None:  return "";
        case Op::Add: return "+";
        case Op::Sub: return "-";
        case Op::Mul: return "*";
        case Op::Div: return "/";
        case Op::Mod: return "%";

        case Op::And: return "&";
        case Op::Or:  return "|";
        case Op::Xor: return "^";

        case Op::Shr: return ">>";
        case Op::Shl: return "<<";
        }
        throw "";
    }

    Op  m_op;
    ExprNodeP   m_slot;
    ExprNodeP   m_value;

    ExprNode_Assign(Span sp, Op op, ::HIR::ExprNodeP slot, ::HIR::ExprNodeP value):
        ExprNode(mv$(sp)),
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
    static const char* opname(Op v) {
        switch(v)
        {
        case Op::CmpEqu:    return "==";
        case Op::CmpNEqu:   return "!=";
        case Op::CmpLt:     return "<";
        case Op::CmpLtE:    return "<=";
        case Op::CmpGt:     return ">";
        case Op::CmpGtE:    return ">=";

        case Op::BoolAnd:   return "&&";
        case Op::BoolOr:    return "||";

        case Op::Add: return "+";
        case Op::Sub: return "-";
        case Op::Mul: return "*";
        case Op::Div: return "/";
        case Op::Mod: return "%";

        case Op::And: return "&";
        case Op::Or:  return "|";
        case Op::Xor: return "^";

        case Op::Shr: return ">>";
        case Op::Shl: return "<<";
        }
        return "??";
    }

    Op    m_op;
    ::HIR::ExprNodeP m_left;
    ::HIR::ExprNodeP m_right;

    ExprNode_BinOp(Span sp, Op op, ::HIR::ExprNodeP left, ::HIR::ExprNodeP right):
        ExprNode( mv$(sp) ),
        m_op(op),
        m_left( mv$(left) ),
        m_right( mv$(right) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_UniOp:
    public ExprNode
{
    enum class Op {
        Invert, // '!<expr>'
        Negate, // '-<expr>'
    };
    static const char* opname(Op v) {
        switch(v) {
        case Op::Invert:return "!";
        case Op::Negate:return "-";
        }
        throw "";
    }

    Op  m_op;
    ::HIR::ExprNodeP    m_value;

    ExprNode_UniOp(Span sp, Op op, ::HIR::ExprNodeP value):
        ExprNode( mv$(sp) ),
        m_op(op),
        m_value( mv$(value) )
    {}

    NODE_METHODS();
};
struct ExprNode_Borrow:
    public ExprNode
{
    ::HIR::BorrowType   m_type;
    ::HIR::ExprNodeP    m_value;

    ExprNode_Borrow(Span sp, ::HIR::BorrowType bt, ::HIR::ExprNodeP value):
        ExprNode( mv$(sp) ),
        m_type(bt),
        m_value( mv$(value) )
    {}

    NODE_METHODS();
};
struct ExprNode_RawBorrow:
    public ExprNode
{
    ::HIR::BorrowType   m_type;
    ::HIR::ExprNodeP    m_value;

    ExprNode_RawBorrow(Span sp, ::HIR::BorrowType bt, ::HIR::ExprNodeP value):
        ExprNode( mv$(sp) ),
        m_type(bt),
        m_value( mv$(value) )
    {}

    NODE_METHODS();
};
struct ExprNode_Cast:
    public ExprNode
{
    ::HIR::ExprNodeP    m_value;
    ::HIR::TypeRef  m_dst_type;

    ExprNode_Cast(Span sp, ::HIR::ExprNodeP value, ::HIR::TypeRef dst_type):
        ExprNode( mv$(sp) )
        ,m_value( mv$(value) )
        ,m_dst_type( mv$(dst_type) )
    {
    }

    NODE_METHODS();
};
// Magical pointer unsizing operation:
// - `&[T; n] -> &[T]`
// - `&T -> &Trait`
// - `Box<T> -> Box<Trait>`
// NOTE: Also used for type ascription
struct ExprNode_Unsize:
    public ExprNode
{
    ::HIR::ExprNodeP    m_value;
    ::HIR::TypeRef  m_dst_type;

    ExprNode_Unsize(Span sp, ::HIR::ExprNodeP value, ::HIR::TypeRef dst_type):
        ExprNode( mv$(sp) )
        ,m_value( mv$(value) )
        ,m_dst_type( mv$(dst_type) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_Index:
    public ExprNode
{
    ::HIR::ExprNodeP    m_value;
    ::HIR::ExprNodeP    m_index;

    struct {
        ::HIR::TypeRef  index_ty;
    } m_cache;

    ExprNode_Index(Span sp, ::HIR::ExprNodeP val, ::HIR::ExprNodeP index):
        ExprNode(mv$(sp)),
        m_value( mv$(val) ),
        m_index( mv$(index) )
    {}

    NODE_METHODS();
};
// unary `*`
struct ExprNode_Deref:
    public ExprNode
{
    ::HIR::ExprNodeP    m_value;

    ExprNode_Deref(Span sp, ::HIR::ExprNodeP val):
        ExprNode(mv$(sp)),
        m_value( mv$(val) )
    {}

    NODE_METHODS();
};
/// `box` and `in`/`<-`
struct ExprNode_Emplace:
    public ExprNode
{
    /// This influences the ops trait used
    enum class Type {
        Noop,   // Hack to allow coercion - acts as a no-op node
        Placer,
        Boxer,
    };

    Type    m_type;
    ExprNodeP   m_place;
    ExprNodeP   m_value;

    ExprNode_Emplace(Span sp, Type ty, ::HIR::ExprNodeP place, ::HIR::ExprNodeP val):
        ExprNode( mv$(sp) ),
        m_type(ty),
        m_place( mv$(place) ),
        m_value( mv$(val) )
    {
    }

    NODE_METHODS();
};

struct ExprNode_TupleVariant:
    public ExprNode
{
    // Path to variant/struct
    ::HIR::GenericPath m_path;
    bool    m_is_struct;
    ::std::vector<ExprNodeP> m_args;

    // - Cache for typeck
    ::std::vector< ::HIR::TypeRef>  m_arg_types;

    ExprNode_TupleVariant(Span sp, ::HIR::GenericPath path, bool is_struct, ::std::vector< ::HIR::ExprNodeP> args):
        ExprNode(mv$(sp)),
        m_path( mv$(path) ),
        m_is_struct( is_struct ),
        m_args( mv$(args) )
    {}

    NODE_METHODS();
};

struct ExprCallCache
{
    ::std::vector< ::HIR::TypeRef>  m_arg_types;
    const ::HIR::GenericParams* m_fcn_params;
    const ::HIR::GenericParams* m_top_params;
    const ::HIR::Function*  m_fcn;

    ::std::unique_ptr<Monomorphiser>    m_monomorph;
};

struct ExprNode_CallPath:
    public ExprNode
{
    ::HIR::Path m_path;
    ::std::vector<ExprNodeP> m_args;

    // - Cache for typeck
    ExprCallCache   m_cache;

    ExprNode_CallPath(Span sp, ::HIR::Path path, ::std::vector< ::HIR::ExprNodeP> args):
        ExprNode(mv$(sp)),
        m_path( mv$(path) ),
        m_args( mv$(args) )
    {}

    NODE_METHODS();
};
struct ExprNode_CallValue:
    public ExprNode
{
    ::HIR::ExprNodeP m_value;
    ::std::vector<ExprNodeP> m_args;

    // - Argument types used as coercion targets
    ::std::vector< ::HIR::TypeRef>  m_arg_ivars;

    // - Cache for typeck
    ::std::vector< ::HIR::TypeRef>  m_arg_types;

    // Indicates what trait should/is being used for this call
    // - Determined by typeck using the present trait bound (also adds borrows etc)
    // - If the called value is a closure, this stays a Unknown until closure expansion
    enum class TraitUsed {
        Unknown,
        Fn,
        FnMut,
        FnOnce,
    };
    TraitUsed   m_trait_used = TraitUsed::Unknown;

    ExprNode_CallValue(Span sp, ::HIR::ExprNodeP val, ::std::vector< ::HIR::ExprNodeP> args):
        ExprNode(mv$(sp)),
        m_value( mv$(val) ),
        m_args( mv$(args) )
    {}

    NODE_METHODS();
};
struct ExprNode_CallMethod:
    public ExprNode
{
    ::HIR::ExprNodeP    m_value;
    RcString   m_method;
    ::HIR::PathParams  m_params;
    ::std::vector< ::HIR::ExprNodeP>    m_args;

    // - Set during typeck to the real path to the method
    ::HIR::Path m_method_path;
    // - Cache of argument/return types
    ExprCallCache   m_cache;

    // - List of possible traits (in-scope traits that contain this method)
    t_trait_list    m_traits;
    // - A pool of ivars to use for searching for trait impls
    ::std::vector<unsigned int> m_trait_param_ivars;

    ExprNode_CallMethod(Span sp, ::HIR::ExprNodeP val, RcString method_name, ::HIR::PathParams params, ::std::vector< ::HIR::ExprNodeP> args):
        ExprNode( mv$(sp) ),
        m_value( mv$(val) ),
        m_method( mv$(method_name) ),
        m_params( mv$(params) ),
        m_args( mv$(args) ),

        m_method_path( ::HIR::SimplePath("",{}) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_Field:
    public ExprNode
{
    ::HIR::ExprNodeP    m_value;
    RcString   m_field;

    ExprNode_Field(Span sp, ::HIR::ExprNodeP val, RcString field):
        ExprNode(mv$(sp)),
        m_value( mv$(val) ),
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

    ExprNode_Literal(Span sp, Data data):
        ExprNode( mv$(sp) ),
        m_data( mv$(data) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_UnitVariant:
    public ExprNode
{
    // Path to variant/struct
    ::HIR::GenericPath m_path;
    bool    m_is_struct;

    ExprNode_UnitVariant(Span sp, ::HIR::GenericPath path, bool is_struct):
        ExprNode(mv$(sp)),
        m_path( mv$(path) ),
        m_is_struct( is_struct )
    {}

    NODE_METHODS();
};
struct ExprNode_PathValue:
    public ExprNode
{
    enum Target {
        UNKNOWN,
        FUNCTION,
        STRUCT_CONSTR,
        ENUM_VAR_CONSTR,
        STATIC,
        CONSTANT,
    };
    ::HIR::Path m_path;
    Target  m_target;

    ExprNode_PathValue(Span sp, ::HIR::Path path, Target target):
        ExprNode(mv$(sp)),
        m_path( mv$(path) ),
        m_target( target )
    {}

    NODE_METHODS();
};
struct ExprNode_Variable:
    public ExprNode
{
    RcString   m_name;
    unsigned int    m_slot;

    ExprNode_Variable(Span sp, RcString name, unsigned int slot):
        ExprNode(mv$(sp)),
        m_name( mv$(name) ),
        m_slot( slot )
    {}

    NODE_METHODS();
};
struct ExprNode_ConstParam:
    public ExprNode
{
    RcString   m_name;
    unsigned int    m_binding;

    ExprNode_ConstParam(Span sp, RcString name, unsigned int binding):
        ExprNode(mv$(sp)),
        m_name( mv$(name) ),
        m_binding( binding )
    {}

    NODE_METHODS();
};

struct ExprNode_StructLiteral:
    public ExprNode
{
    typedef ::std::vector< ::std::pair< RcString, ExprNodeP > > t_values;

    ::HIR::TypeRef  m_type;
    bool    m_is_struct;
    ::HIR::ExprNodeP    m_base_value;
    t_values    m_values;

    /// Actual path extracted from the TypeRef (populated after inner UFCS expansion)
    ::HIR::GenericPath   m_real_path;
    /// Monomorphised types of each field.
    ::std::vector< ::HIR::TypeRef>  m_value_types;

    ExprNode_StructLiteral(Span sp, ::HIR::TypeRef ty, bool is_struct, ::HIR::ExprNodeP base_value, t_values values):
        ExprNode( mv$(sp) ),
        m_type( mv$(ty) ),
        m_is_struct( is_struct ),
        m_base_value( mv$(base_value) ),
        m_values( mv$(values) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_Tuple:
    public ExprNode
{
    ::std::vector< ::HIR::ExprNodeP>    m_vals;

    ExprNode_Tuple(Span sp, ::std::vector< ::HIR::ExprNodeP> vals):
        ExprNode(mv$(sp)),
        m_vals( mv$(vals) )
    {}

    NODE_METHODS();
};
struct ExprNode_ArrayList:
    public ExprNode
{
    ::std::vector< ::HIR::ExprNodeP>    m_vals;

    ExprNode_ArrayList(Span sp, ::std::vector< ::HIR::ExprNodeP> vals):
        ExprNode( mv$(sp) ),
        m_vals( mv$(vals) )
    {}

    NODE_METHODS();
};
// TODO: Might want a second variant for dynamically-sized arrays
struct ExprNode_ArraySized:
    public ExprNode
{
    ::HIR::ExprNodeP    m_val;
    ::HIR::ArraySize    m_size;

    ExprNode_ArraySized(Span sp, ::HIR::ExprNodeP val, ::HIR::ExprPtr size):
        ExprNode(mv$(sp)),
        m_val( mv$(val) ),
        m_size( HIR::ConstGeneric(std::make_shared<HIR::ExprPtr>(mv$(size))) )
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
    bool    m_is_move = false;

    enum class Class {
        Unknown,
        NoCapture,
        Shared,
        Mut,
        Once,
    } m_class = Class::Unknown;
    bool m_is_copy = true;  // Assume that closures are Copy/Clone (for the purposes of typecheck) until AVU is run

    // - Cache between the AVU and ExpandClosures passes
    struct {
        ::std::vector<unsigned int> local_vars;
        ::std::vector< ::std::pair<unsigned int, ::HIR::ValueUsage> > captured_vars;
    } m_avu_cache;

    // - Path to the generated closure type
    const ::HIR::Struct*    m_obj_ptr = nullptr;
    ::HIR::GenericPath  m_obj_path_base;
    ::HIR::GenericPath  m_obj_path;
    ::std::vector< ::HIR::ExprNodeP>    m_captures;

    ExprNode_Closure(Span sp, args_t args, ::HIR::TypeRef rv, ::HIR::ExprNodeP code, bool is_move):
        ExprNode(mv$(sp)),
        m_args( ::std::move(args) ),
        m_return( ::std::move(rv) ),
        m_code( ::std::move(code) ),
        m_is_move(is_move)
    {}

    NODE_METHODS();
};

struct ExprNode_Generator:
    public ExprNode
{
    //ExprNode_Closure::args_t    m_args;
    ::HIR::TypeRef  m_return;
    ::HIR::TypeRef  m_yield_ty;
    ::HIR::ExprNodeP    m_code;
    bool    m_is_move;
    bool    m_is_pinned;

    struct {
        ::std::vector<unsigned int> local_vars;
        ::std::vector< ::std::pair<unsigned int, ::HIR::ValueUsage> > captured_vars;
    } m_avu_cache;

    // Generated type information
    const ::HIR::Struct*    m_obj_ptr = nullptr;
    ::HIR::GenericPath  m_obj_path;
    // Captured variables (used for emitting the constructor)
    ::std::vector< ::HIR::ExprNodeP>    m_captures;
    // State data type (needed for initialising)
    ::HIR::TypeRef  m_state_data_type;

    ExprNode_Generator(Span sp, /*ExprNode_Closure::args_t args,*/ ::HIR::TypeRef rv, ::HIR::ExprNodeP code, bool is_move, bool is_pinned):
        ExprNode(mv$(sp)),
        //m_args( ::std::move(args) ),
        m_return( ::std::move(rv) ),
        m_code( ::std::move(code) ),
        m_is_move(is_move),
        m_is_pinned(is_pinned)
    {}

    NODE_METHODS();
};

/// <summary>
/// Top-level wrapper for the generator method
/// </summary>
struct ExprNode_GeneratorWrapper:
    public ExprNode
{
    //ExprNode_Closure::args_t    m_args;
    ::HIR::TypeRef  m_return;
    ::HIR::TypeRef  m_yield_ty;
    ::HIR::ExprNodeP    m_code;

    // Generated type information
    const ::HIR::Struct*    m_obj_ptr = nullptr;
    ::HIR::GenericPath  m_obj_path;

    ::HIR::TypeRef  m_state_data_type;
    ::HIR::SimplePath   m_state_idx_enum;
    
    ::HIR::Function*    m_drop_fcn_ptr = nullptr;

    ::std::vector<HIR::ValueUsage> m_capture_usages;

    ExprNode_GeneratorWrapper(Span sp, /*ExprNode_Closure::args_t args,*/ ::HIR::TypeRef rv, ::HIR::ExprNodeP code, bool is_move, bool is_pinned):
        ExprNode(mv$(sp)),
        //m_args( ::std::move(args) ),
        m_return( ::std::move(rv) ),
        m_code( ::std::move(code) )
    {}

    NODE_METHODS();
};

#undef NODE_METHODS

class ExprVisitor
{
public:
    virtual ~ExprVisitor() = default;
    virtual void visit_node_ptr(::std::unique_ptr<ExprNode>& node_ptr);
    virtual void visit_node(ExprNode& node);
    #define NV(nt)  virtual void visit(nt& n) = 0;

    NV(ExprNode_Block)
    NV(ExprNode_Asm)
    NV(ExprNode_Asm2)
    NV(ExprNode_Return)
    NV(ExprNode_Yield)
    NV(ExprNode_Let)
    NV(ExprNode_Loop)
    NV(ExprNode_LoopControl)
    NV(ExprNode_Match)
    NV(ExprNode_If)

    NV(ExprNode_Assign)
    NV(ExprNode_BinOp)
    NV(ExprNode_UniOp)
    NV(ExprNode_Borrow)
    NV(ExprNode_RawBorrow)
    NV(ExprNode_Cast)   // Conversion
    NV(ExprNode_Unsize) // Coercion
    NV(ExprNode_Index)
    NV(ExprNode_Deref)
    NV(ExprNode_Emplace)

    NV(ExprNode_TupleVariant);
    NV(ExprNode_CallPath);
    NV(ExprNode_CallValue);
    NV(ExprNode_CallMethod);
    NV(ExprNode_Field);

    NV(ExprNode_Literal);
    NV(ExprNode_UnitVariant);
    NV(ExprNode_PathValue);
    NV(ExprNode_Variable);
    NV(ExprNode_ConstParam);

    NV(ExprNode_StructLiteral);
    NV(ExprNode_Tuple);
    NV(ExprNode_ArrayList);
    NV(ExprNode_ArraySized);

    NV(ExprNode_Closure);
    NV(ExprNode_Generator);
    NV(ExprNode_GeneratorWrapper);
    #undef NV
};

class ExprVisitorDef:
    public ExprVisitor
{
public:
    #define NV(nt)  virtual void visit(nt& n) override;

    virtual void visit_node_ptr(::std::unique_ptr<ExprNode>& node_ptr) override;

    NV(ExprNode_Block)
    NV(ExprNode_Asm)
    NV(ExprNode_Asm2)
    NV(ExprNode_Return)
    NV(ExprNode_Yield)
    NV(ExprNode_Let)
    NV(ExprNode_Loop)
    NV(ExprNode_LoopControl)
    NV(ExprNode_Match)
    NV(ExprNode_If)

    NV(ExprNode_Assign)
    NV(ExprNode_BinOp)
    NV(ExprNode_UniOp)
    NV(ExprNode_Borrow)
    NV(ExprNode_RawBorrow)
    NV(ExprNode_Cast)
    NV(ExprNode_Unsize)
    NV(ExprNode_Index)
    NV(ExprNode_Deref)
    NV(ExprNode_Emplace)

    NV(ExprNode_TupleVariant);
    NV(ExprNode_CallPath);
    NV(ExprNode_CallValue);
    NV(ExprNode_CallMethod);
    NV(ExprNode_Field);

    NV(ExprNode_Literal);
    NV(ExprNode_UnitVariant);
    NV(ExprNode_PathValue);
    NV(ExprNode_Variable);
    NV(ExprNode_ConstParam);

    NV(ExprNode_StructLiteral);
    NV(ExprNode_Tuple);
    NV(ExprNode_ArrayList);
    NV(ExprNode_ArraySized);

    NV(ExprNode_Closure);
    NV(ExprNode_Generator);
    NV(ExprNode_GeneratorWrapper);
    #undef NV

    virtual void visit_pattern(const Span& sp, ::HIR::Pattern& pat);
    virtual void visit_type(::HIR::TypeRef& ty);
    virtual void visit_trait_path(::HIR::TraitPath& p);
    virtual void visit_path_params(::HIR::PathParams& ty);
    virtual void visit_path(::HIR::Visitor::PathContext pc, ::HIR::Path& ty);
    virtual void visit_generic_path(::HIR::Visitor::PathContext pc, ::HIR::GenericPath& ty);
};

}

void HIR_DumpExpr(::std::ostream& sink, const ::HIR::ExprPtr& expr);
