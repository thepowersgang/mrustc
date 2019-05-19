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

#if 0
// TODO: Store LValues as:
// - A packed root value (one word, using the low bits as an enum descriminator)
// - A list of (inner to outer) wrappers
struct LValue
{
    class Storage
    {
        uintptr_t   val;
        static uintptr_t MAX_ARG = (1 << 30) - 1; // max value of 30 bits
    public:
        Storage(const Storage&) = delete;
        Storage& operator=(const Storage&) = delete;
        Storage(Storage&& x)
            :val(x.val)
        {
            x.val = 0;
        }
        Storage& operator=(Storage&& x)
        {
            this->~Storage();
            this->val = x.val;
            x.val = 0;
        }
        ~Storage()
        {
            if( is_Static() ) {
                delete reinterpret_cast<::HIR::Path*>(val & ~3u);
                val = 0;
            }
        }

        static Storage new_Return() { return Storage { MAX_ARG << 2 }; }
        static Storage new_Argument(unsigned idx) { assert(idx < MAX_ARG); return Storage { idx << 2 }; )
        static Storage new_Local(unsigned idx) { assert(idx <= MAX_ARG); return Storage { (idx << 2) | 1 } };
        static Storage new_Static(::HIR::Path p) {
            ::HIR::Path* ptr = new ::HIR::Path(::std::move(p));
            return Storage { static_cast<uintptr_t>(ptr) | 2; }
        }

        bool is_Return() const   { return val == (MAX_ARG << 2) /*&& (val & 3) == 0*/; }
        bool is_Argument() const { return val != (MAX_ARG << 2) && (val & 3) == 0; }
        bool is_Local() const    { return (val & 3) == 1; }
        bool is_Static() const   { return (val & 3) == 2; }

        // No as_Return
        unsigned as_Argument() const { assert(is_Argument()); return val >> 2; }
        unsigned as_Local() const { assert(is_Local()); return val >> 2; }
        const ::HIR::Path& as_Static() const { assert(is_Static()); return *reinterpret_cast<const ::HIR::Path*>(val & ~3u); }
    };
    class Wrapper
    {
        uintptr_t   val;
    public:
        static Wrapper new_Deref() { return Wrapper { 0 }; }
        static Wrapepr new_Field   (unsigned idx) { return Wrapper { (idx << 2) | 1 }; }
        static Wrapepr new_Downcast(unsigned idx) { return Wrapper { (idx << 2) | 2 }; }
        static Wrapepr new_Index   (unsigned idx) { return Wrapper { (idx << 2) | 3 }; }

        bool is_Deref   () const { return (val & 3) == 0; }
        // Stores the field index
        bool is_Field   () const { return (val & 3) == 1; }
        // Stores the variant index
        bool is_Downcast() const { return (val & 3) == 2; }
        // Stores a Local index
        bool is_Index   () const { return (val & 3) == 3; }

        // no as_Deref()
        const unsigned as_Field() const { assert(is_Field()); return (val >> 2); }
        const unsigned as_Downcast() const { assert(is_Downcast()); return (val >> 2); }
        // TODO: Should this return a LValue?
        const unsigned as_Index() const { assert(is_Index()); return (val >> 2); }
    };

    Storage m_root;
    ::std::vector<Wrapper>  m_wrappers;

    static LValue new_Return() { return LValue { Storage::new_Return(), {} }; }
    static LValue new_Argument(unsigned idx) { return LValue { Storage::new_Argument(idx), {} }; }
    static LValue new_Local(unsigned idx) { return LValue { Storage::new_Local(idx), {} }; }
    static LValue new_Static(::HIR::Path p) { return LValue { Storage::new_Static(::std::move(p)), {} }; }

    static LValue new_Deref(LValue lv) { lv.m_wrappers.push_back(Wrapper::new_Deref()); }
    static LValue new_Field(LValue lv, unsigned idx)    { lv.m_wrappers.push_back(Wrapper::new_Field(idx)); }
    static LValue new_Downcast(LValue lv, unsigned idx) { lv.m_wrappers.push_back(Wrapper::new_Downcast(idx)); }
    static LValue new_Index(LValue lv, unsigned local_idx) { lv.m_wrappers.push_back(Wrapper::new_Index(local_idx)); }

    LValue monomorphise(const MonomorphState& ms, unsigned local_offset=0);
    //LValue monomorphise(const TransParams& ms, unsigned local_offset=0);
    LValue clone() const;

    class Ref
    {
        LValue& r;
        size_t wrapper_idx;
    public::
        LValue clone() const;
        void replace(LValue x) const;
    };
};
#endif

// "LVALUE" - Assignable values
TAGGED_UNION_EX(LValue, (), Return, (
    // Function return
    (Return, struct{}),
    // Function argument (input)
    (Argument, struct { unsigned int idx; }),
    // Variable/Temporary
    (Local, unsigned int),
    // `static` or `static mut`
    (Static, ::std::unique_ptr<::HIR::Path>),
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
    // NOTE: These are behind pointers to save inline space (HIR::Path is ~11
    // words, compared to 4 for MIR::Constant without it)
    (Const, struct { ::std::unique_ptr<::HIR::Path> p; }),   // `const`
    (ItemAddr, ::std::unique_ptr<::HIR::Path>) // address of a value
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
        RcString   name;
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


struct EnumCache;   // Defined in trans/enumerate.cpp
class EnumCachePtr
{
    const EnumCache*  p;
public:
    EnumCachePtr(const EnumCache* p=nullptr): p(p) {}
    ~EnumCachePtr();
    EnumCachePtr(EnumCachePtr&& x): p(x.p) { x.p = nullptr; }
    EnumCachePtr& operator=(EnumCachePtr&& x) { this->~EnumCachePtr(); p = x.p; x.p = nullptr; return *this; }
    operator bool() { return p; }
    const EnumCache& operator*() const { return *p; }
    const EnumCache* operator->() const { return p; }
};
class Function
{
public:
    ::std::vector< ::HIR::TypeRef>  locals;
    //::std::vector< RcString>   local_names;
    ::std::vector<bool> drop_flags;

    ::std::vector<BasicBlock>   blocks;

    // Cache filled/used by enumerate
    mutable EnumCachePtr trans_enum_state;
};

};

