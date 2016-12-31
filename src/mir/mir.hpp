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
    // Field access (tuple, struct, tuple struct, enum field, ...)
    // NOTE: Also used to index an array/slice by a compile-time known index (e.g. in destructuring)
    (Field, struct {
        ::std::unique_ptr<LValue>   val;
        unsigned int    field_index;
        }),
    // Dereference a value
    (Deref, struct {
        ::std::unique_ptr<LValue>   val;
        }),
    // Index an array or slice (typeof(val) == [T; n] or [T])
    // NOTE: This is not bounds checked!
    (Index, struct {
        ::std::unique_ptr<LValue>   val;
        ::std::unique_ptr<LValue>   idx;
        }),
    // Interpret an enum as a particular variant
    (Downcast, struct {
        ::std::unique_ptr<LValue>   val;
        unsigned int    variant_index;
        })
    ), (),(), (
        LValue clone() const;
    )
    );
extern ::std::ostream& operator<<(::std::ostream& os, const LValue& x);
extern bool operator<(const LValue& a, const LValue& b);
extern bool operator==(const LValue& a, const LValue& b);
static inline bool operator!=(const LValue& a, const LValue& b) {
    return !(a == b);
}

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
    (Bytes, ::std::vector< ::std::uint8_t>),    // Byte string
    (StaticString, ::std::string),  // String
    (Const, struct { ::HIR::Path p; }),   // `const`
    (ItemAddr, ::HIR::Path) // address of a value
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
    // Cast on primitives
    (Cast, struct {
        LValue  val;
        ::HIR::TypeRef  type;
        }),
    // Binary operation on primitives
    (BinOp, struct {
        LValue  val_l;
        eBinOp  op;
        LValue  val_r;
        }),
    // Unary operation on primitives
    (UniOp, struct {
        LValue  val;
        eUniOp  op;
        }),
    // Extract the metadata from a DST pointer
    // NOTE: If used on an array, this yields the array size (for generics)
    (DstMeta, struct {
        LValue  val;
        }),
    // Extract the pointer from a DST pointer (as *const ())
    (DstPtr, struct {
        LValue  val;
        }),
    // Construct a DST pointer from a thin pointer and metadata
    (MakeDst, struct {
        LValue  ptr_val;
        LValue  meta_val;
        }),
    (Tuple, struct {
        ::std::vector<LValue>   vals;
        }),
    // Array literal
    (Array, struct {
        ::std::vector<LValue>   vals;
        }),
    // Create a new instance of a union (and eventually enum)
    (Variant, struct {
        ::HIR::GenericPath  path;
        unsigned int index;
        LValue    val;
        }),
    // Create a new instance of a struct (or enum)
    (Struct, struct {
        ::HIR::GenericPath  path;
        unsigned int variant_idx;   // if ~0, it's a struct
        ::std::vector<LValue>   vals;
        })
);
extern ::std::ostream& operator<<(::std::ostream& os, const RValue& x);

TAGGED_UNION(CallTarget, Intrinsic,
    (Value, LValue),
    (Path,  ::HIR::Path),
    (Intrinsic, struct {
        ::std::string   name;
        ::HIR::PathParams   params;
        })
    );

TAGGED_UNION(Terminator, Incomplete,
    (Incomplete, struct {}),    // Block isn't complete (ERROR in output)
    (Return, struct {}),    // Return clealy to caller
    (Diverge, struct {}),   // Continue unwinding up the stack
    (Goto, BasicBlockId),   // Jump to another block
    (Panic, struct { BasicBlockId dst; }),  // ?
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
        CallTarget  fcn;
        ::std::vector<LValue>   args;
        })
    );
extern ::std::ostream& operator<<(::std::ostream& os, const Terminator& x);

enum class eDropKind {
    SHALLOW,
    DEEP,
};
TAGGED_UNION(Statement, Assign,
    // Value assigment
    (Assign, struct {
        LValue  dst;
        RValue  src;
        }),
    // Inline assembly
    (Asm, struct {
        ::std::string   tpl;
        ::std::vector< ::std::pair<::std::string,LValue> >  outputs;
        ::std::vector< ::std::pair<::std::string,LValue> >  inputs;
        ::std::vector< ::std::string>   clobbers;
        ::std::vector< ::std::string>   flags;
        }),
    // Update the state of a drop flag
    //(SetDropFlag, struct {
    //    unsigned int idx;
    //    bool new_val;
    //    }),
    // Drop a value
    (Drop, struct {
        //unsigned int flag_idx;  // Valid if != ~0u
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

