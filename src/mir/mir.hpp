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
#include <memory>   // std::unique_ptr
#include <hir/type.hpp>

namespace MIR {

typedef unsigned int    RegionId;
typedef unsigned int    BasicBlockId;

// "LVALUE" - Assignable values
TAGGED_UNION_EX(LValue, (), Return, (
    // Function return
    (Return, struct{}),
    // Function argument (input)
    (Argument, struct { unsigned int idx; }),
    // Variable/Temporary
    (Local, unsigned int),
    // `static` or `static mut`
    (Static, ::HIR::Path),
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
TAGGED_UNION_EX(Constant, (), Int, (
    (Int, struct {
        ::std::int64_t  v;
        ::HIR::CoreType t;
        }),
    (Uint, struct {
        ::std::uint64_t v;
        ::HIR::CoreType t;
        }),
    (Float, struct {
        double  v;
        ::HIR::CoreType t;
        }),
    (Bool, struct {
        bool    v;  // NOTE: Defensive to prevent implicit casts
        }),
    (Bytes, ::std::vector< ::std::uint8_t>),    // Byte string
    (StaticString, ::std::string),  // String
    (Const, struct { ::HIR::Path p; }),   // `const`
    (ItemAddr, ::HIR::Path) // address of a value
    ), (), (), (
        friend ::std::ostream& operator<<(::std::ostream& os, const Constant& v);
        ::Ordering ord(const Constant& b) const;
        inline bool operator==(const Constant& b) const { return ord(b) == ::OrdEqual; }
        inline bool operator!=(const Constant& b) const { return ord(b) != ::OrdEqual; }
        inline bool operator<(const Constant& b) const { return ord(b) == ::OrdLess; }
        inline bool operator<=(const Constant& b) const { return ord(b) != ::OrdGreater; }
        inline bool operator>(const Constant& b) const { return ord(b) == ::OrdGreater; }
        inline bool operator>=(const Constant& b) const { return ord(b) != ::OrdLess; }
        Constant clone() const;
    )
);

/// Parameter - A value used when a rvalue just reads (doesn't require a lvalue)
/// Can be either a lvalue (memory address), or a constant
TAGGED_UNION_EX(Param, (), LValue, (
    (LValue, LValue),
    (Constant, Constant)
    ), (), (), (
        Param clone() const;
        friend ::std::ostream& operator<<(::std::ostream& os, const Param& v);
        bool operator==(const Param& b) const;
        inline bool operator!=(const Param& b) const {
            return !(*this == b);
        }
    )
);

TAGGED_UNION_EX(RValue, (), Use, (
    (Use, LValue),
    (Constant, Constant),
    (SizedArray, struct {
        Param   val;
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
        Param   val_l;
        eBinOp  op;
        Param   val_r;
        }),
    // Unary operation on primitives
    (UniOp, struct {
        LValue  val;    // NOTE: Not a param, because UniOps can be const propagated
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
        Param   ptr_val;
        Param   meta_val;
        }),
    (Tuple, struct {
        ::std::vector<Param>   vals;
        }),
    // Array literal
    (Array, struct {
        ::std::vector<Param>   vals;
        }),
    // Create a new instance of a union (and eventually enum)
    (Variant, struct {
        ::HIR::GenericPath  path;
        unsigned int index;
        Param   val;
        }),
    // Create a new instance of a struct
    (Struct, struct {
        ::HIR::GenericPath  path;
        ::std::vector<Param>   vals;
        })
    ), (),(), (
        RValue clone() const;
    )
);
extern ::std::ostream& operator<<(::std::ostream& os, const RValue& x);
extern bool operator==(const RValue& a, const RValue& b);
static inline bool operator!=(const RValue& a, const RValue& b) {
    return !(a == b);
}

TAGGED_UNION(CallTarget, Intrinsic,
    (Value, LValue),
    (Path,  ::HIR::Path),
    (Intrinsic, struct {
        ::std::string   name;
        ::HIR::PathParams   params;
        })
    );
TAGGED_UNION_EX(SwitchValues, (), Unsigned, (
    (Unsigned, ::std::vector<uint64_t>),
    (Signed, ::std::vector<int64_t>),
    (String, ::std::vector<::std::string>)
    ), (),(), (
        SwitchValues clone() const;
    )
    );

TAGGED_UNION(Terminator, Incomplete,
    (Incomplete, struct {}),    // Block isn't complete (ERROR in output)
    (Return, struct {}),    // Return clealy to caller
    (Diverge, struct {}),   // Continue unwinding up the stack
    (Goto, BasicBlockId),   // Jump to another block
    (Panic, struct { BasicBlockId dst; }),  // ?
    (If, struct {
        LValue cond;
        BasicBlockId    bb0;    // true
        BasicBlockId    bb1;    // false
        }),
    (Switch, struct {
        LValue val;
        ::std::vector<BasicBlockId>  targets;
        }),
    (SwitchValue, struct {
        LValue  val;
        BasicBlockId    def_target;
        ::std::vector<BasicBlockId> targets;
        SwitchValues    values;
        }),
    (Call, struct {
        BasicBlockId    ret_block;
        BasicBlockId    panic_block;
        LValue  ret_val;
        CallTarget  fcn;
        ::std::vector<Param>   args;
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
    (SetDropFlag, struct {
        unsigned int idx;
        bool new_val;   // If `other` is populated, this indicates that the other value should be negated
        unsigned int other;
        }),
    // Drop a value
    (Drop, struct {
        eDropKind   kind;   // NOTE: For the `box` primitive
        LValue  slot;
        unsigned int flag_idx;  // Valid if != ~0u
        }),
    (ScopeEnd, struct {
        ::std::vector<unsigned> slots;
        })
    );
extern ::std::ostream& operator<<(::std::ostream& os, const Statement& x);
extern bool operator==(const Statement& a, const Statement& b);
static inline bool operator!=(const Statement& a, const Statement& b) {
    return !(a == b);
}

struct BasicBlock
{
    ::std::vector<Statement>    statements;
    Terminator  terminator;
};


class Function
{
public:
    ::std::vector< ::HIR::TypeRef>  locals;
    //::std::vector< ::std::string>   local_names;
    ::std::vector<bool> drop_flags;

    ::std::vector<BasicBlock>   blocks;
};

};

