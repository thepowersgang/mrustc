/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/mir.hpp
 * - MIR (Middle Intermediate Representation) definitions
 */
#pragma once
#include <tagged_union.hpp>
#include <vector>
#include <string>
#include <hir/type.hpp>

namespace MIR {

typedef unsigned int    RegionId;
typedef unsigned int    BasicBlockId;

// "LVALUE" - Assignable values
TAGGED_UNION_EX(LValue, (), Variable, (
    // User-named variable
    (Variable, unsigned int),
    // Temporary with no user-defined name
    (Temporary, struct {
        unsigned int idx;
        }),
    // Function argument (matters for destructuring)
    (Argument, struct {
        unsigned int idx;
        }),
    // `static` or `static mut`
    (Static, ::HIR::Path),
    // Function return
    (Return, struct{}),
    (Field, struct {
        ::std::unique_ptr<LValue>   val;
        unsigned int    field_index;
        }),
    // Dereference a value
    (Deref, struct {
        ::std::unique_ptr<LValue>   val;
        }),
    // Index an array or slice (typeof(val) == [T; n] or [T])
    (Index, struct {
        ::std::unique_ptr<LValue>   val;
        ::std::unique_ptr<LValue>   idx;
        }),
    (Downcast, struct {
        ::std::unique_ptr<LValue>   val;
        unsigned int    variant_index;
        })
    ), (),(), (
        LValue clone() const;
    )
    );
extern ::std::ostream& operator<<(::std::ostream& os, const LValue& x);

enum class eBinOp
{
    ADD, ADD_OV,
    SUB, SUB_OV,
    MUL, MUL_OV,
    DIV, DIV_OV,
    MOD,// MOD_OV,
    
    BIT_OR,
    BIT_AND,
    BIT_XOR,
    
    BIT_SHR,
    BIT_SHL,
    
    EQ, NE,
    GT, GE,
    LT, LE,
};
enum class eUniOp
{
    INV,
    NEG
};

// Compile-time known values
TAGGED_UNION(Constant, Int,
    (Int, ::std::int64_t),
    (Uint, ::std::uint64_t),
    (Float, double),
    (Bool, bool),
    (Bytes, ::std::vector< ::std::uint8_t>),
    (StaticString, ::std::string),
    (ItemAddr, ::HIR::Path)
    );
extern ::std::ostream& operator<<(::std::ostream& os, const Constant& v);

TAGGED_UNION(RValue, Use,
    (Use, LValue),
    (Constant, Constant),
    (SizedArray, struct {
        LValue  val;
        unsigned int    count;
        }),
    (Borrow, struct {
        RegionId    region;
        ::HIR::BorrowType   type;
        LValue  val;
        }),
    (Cast, struct {
        LValue  val;
        ::HIR::TypeRef  type;
        }),
    (BinOp, struct {
        LValue  val_l;
        eBinOp  op;
        LValue  val_r;
        }),
    (UniOp, struct {
        LValue  val;
        eUniOp  op;
        }),
    (DstMeta, struct {
        LValue  val;
        }),
    (MakeDst, struct {
        LValue  ptr_val;
        LValue  meta_val;
        }),
    (Tuple, struct {
        ::std::vector<LValue>   vals;
        }),
    (Array, struct {
        ::std::vector<LValue>   vals;
        }),
    (Struct, struct {
        ::HIR::GenericPath  path;
        ::std::vector<LValue>   vals;
        })
);

TAGGED_UNION(Terminator, Incomplete,
    (Incomplete, struct {}),
    (Return, struct {}),
    (Diverge, struct {}),
    (Goto, BasicBlockId),
    (Panic, struct { BasicBlockId dst; }),
    (If, struct {
        LValue cond;
        BasicBlockId    bb0;
        BasicBlockId    bb1;
        }),
    (Switch, struct {
        LValue val;
        ::std::vector<BasicBlockId>  targets;
        }),
    (Call, struct {
        BasicBlockId    ret_block;
        BasicBlockId    panic_block;
        LValue  ret_val;
        LValue  fcn_val;
        ::std::vector<LValue>   args;
        })
    );
extern ::std::ostream& operator<<(::std::ostream& os, const Terminator& x);

enum class eDropKind {
    SHALLOW,
    DEEP,
};
TAGGED_UNION(Statement, Assign,
    (Assign, struct {
        LValue  dst;
        RValue  src;
        }),
    (Drop, struct {
        eDropKind   kind;   // NOTE: For the `box` primitive
        LValue  slot;
        })
    );

struct BasicBlock
{
    ::std::vector<Statement>    statements;
    Terminator  terminator;
};


class Function
{
public:
    ::std::vector< ::HIR::TypeRef>  named_variables;
    ::std::vector< ::HIR::TypeRef>  temporaries;
    
    ::std::vector<BasicBlock>   blocks;
};

};

