/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/expr.hpp
 * - AST Expression Nodes
 */
#ifndef AST_EXPR_INCLUDED
#define AST_EXPR_INCLUDED

#include <ostream>
#include <memory>   // unique_ptr
#include <vector>

#include "../parse/tokentree.hpp"
#include "types.hpp"
#include "pattern.hpp"
#include "attrs.hpp"
#include "expr_ptr.hpp"
#include "../hir/asm.hpp"

namespace AST {

class Pattern;
class NodeVisitor;

class ExprNode
{
    AttributeList   m_attrs;
    Span    m_span;
public:
    virtual ~ExprNode() = 0;

    virtual void visit(NodeVisitor& nv) = 0;
    virtual void print(::std::ostream& os) const = 0;
    virtual ExprNodeP clone() const = 0;

    void set_span(Span s) { m_span = ::std::move(s); }
    const Span& span() const { return m_span; }

    void set_attrs(AttributeList&& mi) {
        for(auto& i : mi.m_items)
            m_attrs.m_items.push_back(mv$(i));
        mi.m_items.clear();
    }
    AttributeList& attrs() { return m_attrs; }
};

#define NODE_METHODS()  \
    void visit(NodeVisitor& nv) override;\
    void print(::std::ostream& os) const override; \
    ExprNodeP clone() const override;

struct ExprNode_Block:
    public ExprNode
{
    enum class Type {
        Bare,
        Unsafe,
        Const,
    };
    Type m_block_type;
    bool m_yields_final_value;
    Ident   m_label;
    ::std::shared_ptr<AST::Module> m_local_mod;
    ::std::vector<ExprNodeP>    m_nodes;

    ExprNode_Block(::std::vector<ExprNodeP> nodes={}):
        m_block_type(Type::Bare),
        m_yields_final_value(true),
        m_label(""),
        m_local_mod(),
        m_nodes( ::std::move(nodes) )
    {}
    ExprNode_Block(Type type, bool yields_final_value, ::std::vector<ExprNodeP> nodes, ::std::shared_ptr<AST::Module> local_mod):
        m_block_type(type),
        m_yields_final_value(yields_final_value),
        m_label(""),
        m_local_mod( ::std::move(local_mod) ),
        m_nodes( ::std::move(nodes) )
    {
    }

    NODE_METHODS();
};

struct ExprNode_Try:
    public ExprNode
{
    ExprNodeP   m_inner;

    ExprNode_Try(ExprNodeP inner):
        m_inner( ::std::move(inner) )
    {
    }

    NODE_METHODS();
};

struct ExprNode_Macro:
    public ExprNode
{
    AST::Path   m_path;
    RcString   m_ident;
    ::TokenTree m_tokens;
    bool    m_is_braced;

    ExprNode_Macro(AST::Path name, RcString ident, ::TokenTree&& tokens, bool is_braced=false):
        m_path( ::std::move(name) ),
        m_ident(ident),
        m_tokens( ::std::move(tokens) )
        , m_is_braced(is_braced)
    {}

    NODE_METHODS();
};

// llvm_asm! macro
struct ExprNode_Asm:
    public ExprNode
{
    struct ValRef
    {
        ::std::string   name;
        ExprNodeP   value;
    };

    ::std::string   m_text;
    ::std::vector<ValRef>   m_output;
    ::std::vector<ValRef>   m_input;
    ::std::vector<::std::string>    m_clobbers;
    ::std::vector<::std::string>    m_flags;

    ExprNode_Asm(::std::string text, ::std::vector<ValRef> output, ::std::vector<ValRef> input, ::std::vector<::std::string> clobbers, ::std::vector<::std::string> flags):
        m_text( ::std::move(text) ),
        m_output( ::std::move(output) ),
        m_input( ::std::move(input) ),
        m_clobbers( ::std::move(clobbers) ),
        m_flags( ::std::move(flags) )
    {
    }

    NODE_METHODS();
};

// asm! macro
struct ExprNode_Asm2:
    public ExprNode
{
    TAGGED_UNION(Param, Const,
        (Const, AST::ExprNodeP),
        (Sym, AST::Path),
        (RegSingle, struct {
            AsmCommon::Direction    dir;
            AsmCommon::RegisterSpec spec;
            AST::ExprNodeP  val;
            }),
        (Reg, struct {
            AsmCommon::Direction    dir;
            AsmCommon::RegisterSpec spec;
            AST::ExprNodeP  val_in;
            AST::ExprNodeP  val_out;
            })
        );

    AsmCommon::Options  m_options;
    std::vector<AsmCommon::Line>   m_lines;
    std::vector<Param>  m_params;

    ExprNode_Asm2(AsmCommon::Options options, std::vector<AsmCommon::Line> lines, std::vector<Param> params)
        : m_options(options)
        , m_lines( ::std::move(lines) )
        , m_params( ::std::move(params) )
    {
    }

    NODE_METHODS();
};

// Break/Continue/Return
struct ExprNode_Flow:
    public ExprNode
{
    enum Type {
        RETURN,
        YIELD,
        CONTINUE,
        BREAK,
        // `do yeet value` - a failed `?`
        YEET,
    } m_type;
    Ident   m_target;
    ExprNodeP    m_value;

    ExprNode_Flow(Type type, Ident target, ExprNodeP value):
        m_type(type),
        m_target( ::std::move(target) ),
        m_value( ::std::move(value) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_LetBinding:
    public ExprNode
{
    Pattern m_pat;
    TypeRef m_type;
    ExprNodeP    m_value;
    ExprNodeP    m_else;
    /// Allocated binding slots/indexes for the pattern in `let-else`
    ::std::pair<unsigned,unsigned>  m_letelse_slots;

    ExprNode_LetBinding(Pattern pat, TypeRef type, ExprNodeP value, ExprNodeP else_arm={})
        : m_pat( ::std::move(pat) )
        , m_type( ::std::move(type) )
        , m_value( ::std::move(value) )
        , m_else( ::std::move(else_arm) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_Assign:
    public ExprNode
{
    enum Operation {
        NONE,
        ADD, SUB,
        MUL, DIV, MOD,
        AND, OR , XOR,
        SHR, SHL,
    } m_op;
    ExprNodeP    m_slot;
    ExprNodeP    m_value;

    ExprNode_Assign(): m_op(NONE) {}
    ExprNode_Assign(Operation op, ExprNodeP slot, ExprNodeP value):
        m_op(op),
        m_slot( ::std::move(slot) ),
        m_value( ::std::move(value) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_CallPath:
    public ExprNode
{
    Path    m_path;
    ::std::vector<ExprNodeP> m_args;

    ExprNode_CallPath(Path&& path, ::std::vector<ExprNodeP>&& args):
        m_path( ::std::move(path) ),
        m_args( ::std::move(args) )
    {
    }

    NODE_METHODS();
};
struct ExprNode_CallMethod:
    public ExprNode
{
    ExprNodeP    m_val;
    PathNode    m_method;
    ::std::vector<ExprNodeP> m_args;

    ExprNode_CallMethod(ExprNodeP obj, PathNode method, ::std::vector<ExprNodeP> args):
        m_val( ::std::move(obj) ),
        m_method( ::std::move(method) ),
        m_args( ::std::move(args) )
    {
    }

    NODE_METHODS();
};
// Call an object (Fn/FnMut/FnOnce)
struct ExprNode_CallObject:
    public ExprNode
{
    ExprNodeP    m_val;
    ::std::vector<ExprNodeP> m_args;

    ExprNode_CallObject(ExprNodeP val, ::std::vector< ExprNodeP >&& args):
        m_val( ::std::move(val) ),
        m_args( ::std::move(args) )
    {
    }
    NODE_METHODS();
};

struct ExprNode_Loop:
    public ExprNode
{
    enum Type {
        LOOP,
        WHILE,
        FOR,
    } m_type;
    Ident   m_label;
    AST::Pattern    m_pattern;
    ExprNodeP    m_cond; // if NULL, loop is a 'loop'
    ExprNodeP    m_code;

    ExprNode_Loop():
        m_type(LOOP),
        m_label("")
    {
    }
    ExprNode_Loop(Ident label, ExprNodeP code):
        m_type(LOOP),
        m_label( ::std::move(label) ),
        m_code( ::std::move(code) )
    {}
    ExprNode_Loop(Ident label, ExprNodeP cond, ExprNodeP code):
        m_type(WHILE),
        m_label( ::std::move(label) ),
        m_cond( ::std::move(cond) ),
        m_code( ::std::move(code) )
    {}
    ExprNode_Loop(Ident label, AST::Pattern pattern, ExprNodeP val, ExprNodeP code):
        m_type(FOR),
        m_label( ::std::move(label) ),
        m_pattern( ::std::move(pattern) ),
        m_cond( ::std::move(val) ),
        m_code( ::std::move(code) )
    {}
    NODE_METHODS();
private:
    ExprNode_Loop(Type type, Ident label, AST::Pattern pattern, ExprNodeP val, ExprNodeP code)
        : m_type( type )
        , m_label( ::std::move(label) )
        , m_pattern( ::std::move(pattern) )
        , m_cond( ::std::move(val) )
        , m_code( ::std::move(code) )
    {}
};
struct IfLet_Condition
{
    ::std::unique_ptr<AST::Pattern> opt_pat;
    ExprNodeP    value;
};
struct ExprNode_WhileLet:
    public ExprNode
{
    Ident   m_label;
    std::vector<IfLet_Condition> m_conditions;
    ExprNodeP    m_code;

    ExprNode_WhileLet(Ident label, std::vector<IfLet_Condition> conditions, ExprNodeP code)
        : m_label( ::std::move(label) )
        , m_conditions( ::std::move(conditions) )
        , m_code( ::std::move(code) )
    {}
    NODE_METHODS();
};

struct ExprNode_Match_Arm
{
    AttributeList   m_attrs;
    ::std::vector<Pattern>  m_patterns;
    std::vector<IfLet_Condition> m_guard;

    ExprNodeP    m_code;


    ExprNode_Match_Arm()
    {}
    ExprNode_Match_Arm(::std::vector<Pattern> patterns, std::vector<IfLet_Condition> guard, ExprNodeP code):
        m_patterns( mv$(patterns) ),
        m_guard( mv$(guard) ),
        m_code( mv$(code) )
    {}
};

struct ExprNode_Match:
    public ExprNode
{
    ExprNodeP    m_val;
    ::std::vector<ExprNode_Match_Arm>  m_arms;

    ExprNode_Match(ExprNodeP val, ::std::vector<ExprNode_Match_Arm> arms):
        m_val( ::std::move(val) ),
        m_arms( ::std::move(arms) )
    {
    }
    NODE_METHODS();
};

struct ExprNode_If:
    public ExprNode
{
    ExprNodeP    m_cond;
    ExprNodeP    m_true;
    ExprNodeP    m_false;

    ExprNode_If(ExprNodeP cond, ExprNodeP true_code, ExprNodeP false_code):
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
    std::vector<IfLet_Condition>   m_conditions;
    ExprNodeP    m_true;
    ExprNodeP    m_false;

    ExprNode_IfLet(std::vector<IfLet_Condition> conditions, ExprNodeP true_code, ExprNodeP false_code)
        : m_conditions( ::std::move(conditions) )
        , m_true( ::std::move(true_code) )
        , m_false( ::std::move(false_code) )
    {
    }
    NODE_METHODS();
};
/// Represents `_` in expression position
struct ExprNode_WildcardPattern:
    public ExprNode
{
    NODE_METHODS();
};
// Literal integer
struct ExprNode_Integer:
    public ExprNode
{
    enum eCoreType  m_datatype;
    U128    m_value;

    ExprNode_Integer(U128 value, enum eCoreType datatype):
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

    ExprNode_Bool(bool value):
        m_value(value)
    {
    }

    NODE_METHODS();
};
// Literal string
struct ExprNode_String:
    public ExprNode
{
    ::std::string   m_value;
    /// Hygiene for format strings
    Ident::Hygiene  m_hygiene;

    ExprNode_String(::std::string value, Ident::Hygiene h={})
        : m_value( ::std::move(value) )
        , m_hygiene( ::std::move(h) )
    {}

    NODE_METHODS();
};
// Literal byte string
struct ExprNode_ByteString:
    public ExprNode
{
    ::std::string   m_value;

    ExprNode_ByteString(::std::string value):
        m_value( ::std::move(value) )
    {}

    NODE_METHODS();
};

// Closure / Lambda
struct ExprNode_Closure:
    public ExprNode
{
    typedef ::std::vector< ::std::pair<AST::Pattern, TypeRef> > args_t;

    args_t  m_args;
    TypeRef m_return;
    ExprNodeP    m_code;
    bool m_is_move;     //< The closure takes ownership of all values
    bool m_is_pinned;   //< The closure cannot be moved (this is for generators)
    
    ExprNode_Closure(args_t args, TypeRef rv, ExprNodeP code, bool is_move, bool is_pinned):
        m_args( ::std::move(args) ),
        m_return( ::std::move(rv) ),
        m_code( ::std::move(code) ),
        m_is_move( is_move ),
        m_is_pinned( is_pinned )
    {}

    NODE_METHODS();
};
// Literal structure
struct ExprNode_StructLiteral:
    public ExprNode
{
    struct Ent {
        AttributeList   attrs;
        RcString   name;
        ExprNodeP    value;
    };
    typedef ::std::vector<Ent> t_values;
    Path    m_path;
    ExprNodeP    m_base_value;
    t_values    m_values;

    ExprNode_StructLiteral(Path path, ExprNodeP base_value, t_values&& values ):
        m_path( std::move(path) ),
        m_base_value( std::move(base_value) ),
        m_values( std::move(values) )
    {}

    NODE_METHODS();
};
// Struct literal pattern only
// This implicitly has a `..` in it
struct ExprNode_StructLiteralPattern:
    public ExprNode
{
    typedef ::std::vector<ExprNode_StructLiteral::Ent> t_values;
    Path    m_path;
    t_values    m_values;

    ExprNode_StructLiteralPattern(Path path, t_values&& values)
        : m_path( std::move(path) )
        , m_values( std::move(values) )
    {}

    NODE_METHODS();
};
// Array
struct ExprNode_Array:
    public ExprNode
{
    ExprNodeP    m_size; // if non-NULL, it's a sized array
    ::std::vector< ExprNodeP >   m_values;

    ExprNode_Array(::std::vector< ExprNodeP > vals):
        m_values( ::std::move(vals) )
    {}
    ExprNode_Array(ExprNodeP val, ExprNodeP size):
        m_size( ::std::move(size) )
    {
        m_values.push_back( ::std::move(val) );
    }

    NODE_METHODS();
};
// Tuple
struct ExprNode_Tuple:
    public ExprNode
{
    ::std::vector< ExprNodeP >   m_values;

    ExprNode_Tuple(::std::vector< ExprNodeP > vals):
        m_values( ::std::move(vals) )
    {}

    NODE_METHODS();
};
// Variable / Constant
struct ExprNode_NamedValue:
    public ExprNode
{
    Path    m_path;

    ExprNode_NamedValue(Path path):
        m_path( ::std::move(path) )
    {
    }
    NODE_METHODS();
};
// Field dereference
struct ExprNode_Field:
    public ExprNode
{
    ExprNodeP   m_obj;
    RcString    m_name;

    ExprNode_Field(ExprNodeP obj, RcString name):
        m_obj( ::std::move(obj) ),
        m_name( ::std::move(name) )
    {
    }
    NODE_METHODS();
};
struct ExprNode_Index:
    public ExprNode
{
    ExprNodeP m_obj;
    ExprNodeP m_idx;

    ExprNode_Index(ExprNodeP obj, ExprNodeP idx):
        m_obj( ::std::move(obj) ),
        m_idx( ::std::move(idx) )
    {}

    NODE_METHODS();
};

// Pointer dereference
struct ExprNode_Deref:
    public ExprNode
{
    ExprNodeP    m_value;

    ExprNode_Deref(ExprNodeP value):
        m_value( ::std::move(value) )
    {
    }

    NODE_METHODS();
};

// Type cast ('as')
struct ExprNode_Cast:
    public ExprNode
{
    ExprNodeP    m_value;
    TypeRef m_type;

    ExprNode_Cast(ExprNodeP value, TypeRef&& dst_type):
        m_value( ::std::move(value) ),
        m_type( ::std::move(dst_type) )
    {
    }
    NODE_METHODS();
};

// Type annotation (': _')
struct ExprNode_TypeAnnotation:
    public ExprNode
{
    ExprNodeP    m_value;
    TypeRef m_type;

    ExprNode_TypeAnnotation(ExprNodeP value, TypeRef&& dst_type):
        m_value( ::std::move(value) ),
        m_type( ::std::move(dst_type) )
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

        RANGE,
        RANGE_INC,
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

        PLACE_IN,   // `in PLACE { expr }` or `PLACE <- expr`
    };

    Type    m_type;
    ExprNodeP m_left;
    ExprNodeP m_right;

    ExprNode_BinOp(Type type, ExprNodeP left, ExprNodeP right):
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
        REF,    // '& <expr>'
        REFMUT, // '&mut <expr>'
        RawBorrow,
        RawBorrowMut,
        BOX,    // 'box <expr>'
        INVERT, // '!<expr>'
        NEGATE, // '-<expr>'
        QMARK, // '<expr>?'
        AWait,  // `.await`
    };

    enum Type   m_type;
    ExprNodeP m_value;

    ExprNode_UniOp(Type type, ExprNodeP value):
        m_type(type),
        m_value( ::std::move(value) )
    {
    }

    NODE_METHODS();
};

#undef NODE_METHODS

class NodeVisitor
{
public:
    virtual ~NodeVisitor() = default;
    inline void visit(ExprNodeP& cnode) {
        if(cnode.get())
            cnode->visit(*this);
    }
    virtual bool is_const() const { return false; }

    #define NT(nt) \
        virtual void visit(nt& node) = 0/*; \
        virtual void visit(const nt& node) = 0*/
    NT(ExprNode_Block);
    NT(ExprNode_Try);
    NT(ExprNode_Macro);
    NT(ExprNode_Asm);
    NT(ExprNode_Asm2);
    NT(ExprNode_Flow);
    NT(ExprNode_LetBinding);
    NT(ExprNode_Assign);
    NT(ExprNode_CallPath);
    NT(ExprNode_CallMethod);
    NT(ExprNode_CallObject);
    NT(ExprNode_Loop);
    NT(ExprNode_WhileLet);
    NT(ExprNode_Match);
    NT(ExprNode_If);
    NT(ExprNode_IfLet);

    NT(ExprNode_WildcardPattern);
    NT(ExprNode_Integer);
    NT(ExprNode_Float);
    NT(ExprNode_Bool);
    NT(ExprNode_String);
    NT(ExprNode_ByteString);
    NT(ExprNode_Closure);
    NT(ExprNode_StructLiteral);
    NT(ExprNode_StructLiteralPattern);
    NT(ExprNode_Array);
    NT(ExprNode_Tuple);
    NT(ExprNode_NamedValue);

    NT(ExprNode_Field);
    NT(ExprNode_Index);
    NT(ExprNode_Deref);
    NT(ExprNode_Cast);
    NT(ExprNode_TypeAnnotation);
    NT(ExprNode_BinOp);
    NT(ExprNode_UniOp);
    #undef NT
};
class NodeVisitorDef:
    public NodeVisitor
{
public:
    inline void visit(ExprNodeP& cnode) {
        if(cnode.is_valid()) {
            TRACE_FUNCTION_F(cnode.type_name());
            cnode->visit(*this);
        }
    }
    #define NT(nt) \
        virtual void visit(nt& node) override;/* \
        virtual void visit(const nt& node) override*/
    NT(ExprNode_Block);
    NT(ExprNode_Try);
    NT(ExprNode_Macro);
    NT(ExprNode_Asm);
    NT(ExprNode_Asm2);
    NT(ExprNode_Flow);
    NT(ExprNode_LetBinding);
    NT(ExprNode_Assign);
    NT(ExprNode_CallPath);
    NT(ExprNode_CallMethod);
    NT(ExprNode_CallObject);
    NT(ExprNode_Loop);
    NT(ExprNode_WhileLet);
    NT(ExprNode_Match);
    NT(ExprNode_If);
    NT(ExprNode_IfLet);

    NT(ExprNode_WildcardPattern);
    NT(ExprNode_Integer);
    NT(ExprNode_Float);
    NT(ExprNode_Bool);
    NT(ExprNode_String);
    NT(ExprNode_ByteString);
    NT(ExprNode_Closure);
    NT(ExprNode_StructLiteral);
    NT(ExprNode_StructLiteralPattern);
    NT(ExprNode_Array);
    NT(ExprNode_Tuple);
    NT(ExprNode_NamedValue);

    NT(ExprNode_Field);
    NT(ExprNode_Index);
    NT(ExprNode_Deref);
    NT(ExprNode_Cast);
    NT(ExprNode_TypeAnnotation);
    NT(ExprNode_BinOp);
    NT(ExprNode_UniOp);
    #undef NT
};

}

#endif

