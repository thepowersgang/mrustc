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
#include "../hir/asm.hpp"

struct MonomorphState;

namespace MIR {

typedef unsigned int    RegionId;
typedef unsigned int    BasicBlockId;

// Store LValues as:
// - A packed root value (one word, using the low bits as an enum descriminator)
// - A list of (inner to outer) wrappers
struct LValue
{
    class Storage
    {
    public:
        const static uintptr_t MAX_ARG = (1 << 30) - 1; // max value of 30 bits
    private:

        uintptr_t   val;

        Storage(uintptr_t v): val(v) {}
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
            return *this;
        }
        ~Storage()
        {
            if( is_Static() ) {
                delete reinterpret_cast<::HIR::Path*>(val & ~3ull);
                val = 0;
            }
        }

        static Storage new_Return() { return Storage(0 << 2); }
        static Storage new_Argument(unsigned idx) { assert(idx < MAX_ARG); return Storage((idx+1) << 2); }
        static Storage new_Local(unsigned idx) { assert(idx <= MAX_ARG); return Storage((idx << 2) | 1); }
        static Storage new_Static(::HIR::Path p) {
            ::HIR::Path* ptr = new ::HIR::Path(::std::move(p));
            return Storage(reinterpret_cast<uintptr_t>(ptr) | 2);
        }

        Storage clone() const;

        uintptr_t get_inner() const {
            assert(!is_Static());
            return val;
        }
        static Storage from_inner(uintptr_t v) {
            assert( (v & 3) < 2 );
            return Storage(v);
        }

        enum Tag {
            TAG_Argument,
            TAG_Local,
            TAG_Static,
            TAG_Return,
            TAGDEAD,
        };
        Tag tag() const {
            if(val == 0)
                return TAG_Return;
            return static_cast<Tag>(val & 3);
        }

        bool is_Return() const   { return val == 0; }
        bool is_Argument() const { return val != 0 && (val & 3) == 0; }
        bool is_Local() const    { return (val & 3) == 1; }
        bool is_Static() const   { return (val & 3) == 2; }

        const char     as_Return  () const { assert(is_Return()); return 0; }
        const unsigned as_Argument() const { assert(is_Argument()); return static_cast<unsigned>( (val >> 2) - 1 ); }
        const unsigned as_Local   () const { assert(is_Local()); return static_cast<unsigned>(val >> 2); }

        const ::HIR::Path& as_Static() const { assert(is_Static()); return *reinterpret_cast<const ::HIR::Path*>(val & ~3llu); }
              ::HIR::Path& as_Static()       { assert(is_Static()); return *reinterpret_cast<      ::HIR::Path*>(val & ~3llu); }

        Ordering ord(const Storage& x) const;
        bool operator==(const Storage& x) const { return this->ord(x) == OrdEqual; }
        bool operator!=(const Storage& x) const { return this->ord(x) != OrdEqual; }
    };
    class Wrapper
    {
        uint32_t    val;
        Wrapper(uint32_t v): val(v) {}
    public:
        static Wrapper new_Deref() { return Wrapper( 0 ); }
        static Wrapper new_Field   (unsigned idx) { return Wrapper( (idx << 2) | 1 ); }
        static Wrapper new_Downcast(unsigned idx) { return Wrapper( (idx << 2) | 2 ); }
        static Wrapper new_Index   (unsigned idx) { if(idx == ~0u) idx = Storage::MAX_ARG; return Wrapper( (idx << 2) | 3 ); }

        uint32_t get_inner() const { return val; }
        static Wrapper from_inner(uint32_t v) {
            return Wrapper(v);
        }

        enum Tag {
            TAG_Deref,
            TAG_Field,
            TAG_Downcast,
            TAG_Index,
            TAGDEAD,
        };
        Tag tag() const {
            return static_cast<Tag>(val & 3);
        }

        bool is_Deref   () const { return (val & 3) == 0; }
        // Stores the field index
        bool is_Field   () const { return (val & 3) == 1; }
        // Stores the variant index
        bool is_Downcast() const { return (val & 3) == 2; }
        // Stores a Local index
        bool is_Index   () const { return (val & 3) == 3; }

        const char     as_Deref   () const { assert(is_Deref()); return 0; }
        const unsigned as_Field   () const { assert(is_Field()); return (val >> 2); }
        const unsigned as_Downcast() const { assert(is_Downcast()); return (val >> 2); }
        // TODO: Should this return a LValue?
        const unsigned as_Index   () const { assert(is_Index()); unsigned rv = (val >> 2); return rv; }

        void inc_Field   () { assert(is_Field   ()); *this = Wrapper::new_Field   (as_Field   () + 1); }
        void inc_Downcast() { assert(is_Downcast()); *this = Wrapper::new_Downcast(as_Downcast() + 1); }

        Ordering ord(const Wrapper& x) const { return ::ord(val, x.val); }
        bool operator==(const Wrapper& x) const { return val == x.val; }
        bool operator!=(const Wrapper& x) const { return val != x.val; }
    };

    Storage m_root;
    ::std::vector<Wrapper>  m_wrappers;

    LValue()
        :m_root( Storage::new_Return() )
    {
    }
    LValue(Storage root, ::std::vector<Wrapper> wrappers)
        :m_root( ::std::move(root) )
        ,m_wrappers( ::std::move(wrappers) )
    {
    }

    static LValue new_Return  () { return LValue(Storage::new_Return(), {}); }
    static LValue new_Argument(unsigned idx ) { return LValue(Storage::new_Argument(idx), {}); }
    static LValue new_Local   (unsigned idx ) { return LValue(Storage::new_Local(idx), {}); }
    static LValue new_Static  (::HIR::Path p) { return LValue(Storage::new_Static(::std::move(p)), {}); }

    static LValue new_Deref(LValue lv) { lv.m_wrappers.push_back(Wrapper::new_Deref()); return lv; }
    static LValue new_Field(LValue lv, unsigned idx)    { lv.m_wrappers.push_back(Wrapper::new_Field(idx)); return lv; }
    static LValue new_Downcast(LValue lv, unsigned idx) { lv.m_wrappers.push_back(Wrapper::new_Downcast(idx)); return lv; }
    static LValue new_Index(LValue lv, unsigned local_idx) { lv.m_wrappers.push_back(Wrapper::new_Index(local_idx)); return lv; }

    bool is_Return() const { return m_wrappers.empty() && m_root.is_Return(); }
    bool is_Local () const { return m_wrappers.empty() && m_root.is_Local(); }
    const unsigned as_Local() const { assert(m_wrappers.empty()); return m_root.as_Local(); }

    bool is_Deref   () const { return m_wrappers.size() > 0 && m_wrappers.back().is_Deref(); }
    bool is_Field   () const { return m_wrappers.size() > 0 && m_wrappers.back().is_Field(); }
    bool is_Downcast() const { return m_wrappers.size() > 0 && m_wrappers.back().is_Downcast(); }
    const unsigned as_Field() const { assert(!m_wrappers.empty()); return m_wrappers.back().as_Field(); }

    void inc_Field   () { assert(m_wrappers.size() > 0); m_wrappers.back().inc_Field   (); }
    void inc_Downcast() { assert(m_wrappers.size() > 0); m_wrappers.back().inc_Downcast(); }

    Ordering ord(const LValue& x) const;

    LValue monomorphise(const MonomorphState& ms, unsigned local_offset=0);
    //LValue monomorphise(const TransParams& ms, unsigned local_offset=0);
    LValue clone() const {
        return LValue(m_root.clone(), m_wrappers);
    }
    LValue clone_wrapped(::std::vector<Wrapper> wrappers) const {
        if( this->m_wrappers.empty() ) {
            return LValue(m_root.clone(), ::std::move(wrappers));
        }
        else {
            return clone_wrapped(wrappers.begin(), wrappers.end());
        }
    }
    template<typename It>
    LValue clone_wrapped(It begin_it, It end_it) const {
        ::std::vector<Wrapper>  wrappers;
        wrappers.reserve(m_wrappers.size() + ::std::distance(begin_it, end_it));
        wrappers.insert(wrappers.end(), m_wrappers.begin(), m_wrappers.end());
        wrappers.insert(wrappers.end(), begin_it, end_it);
        return LValue(m_root.clone(), ::std::move(wrappers));
    }

    LValue clone_unwrapped(unsigned count=1) const {
        assert(count > 0);
        assert(count <= m_wrappers.size());
        return LValue(m_root.clone(), ::std::vector<Wrapper>(m_wrappers.begin(), m_wrappers.end() - count));
    }

    // Returns true if this LValue is a subset of the other (e.g. `_1.0` is a subset of `_1.0*`)
    bool is_subset_of(const LValue& other) const {
        return m_root == other.m_root && other.m_wrappers.size() >= m_wrappers.size() && std::equal(m_wrappers.begin(), m_wrappers.end(), other.m_wrappers.begin());
    }
    // Returns true if one lvalue is a subset of the other
    // - Equivalent to `a.is_subset_of(b) || b.is_subset_of(a)` (but more efficient)
    bool is_either_subset(const LValue& other) const {
        if( !(m_root == other.m_root) )
            return false;
        if( other.m_wrappers.size() > m_wrappers.size() )
            return ::std::equal(m_wrappers.begin(), m_wrappers.end(), other.m_wrappers.begin());
        else
            return ::std::equal(other.m_wrappers.begin(), other.m_wrappers.end(), m_wrappers.begin());
    }

    /// Helper class that represents a LValue unwrapped to a certain degree
    class RefCommon
    {
    protected:
        const LValue* m_lv;
        size_t m_wrapper_count;

        RefCommon(const LValue& lv, size_t wrapper_count)
            :m_lv(&lv)
            ,m_wrapper_count(wrapper_count)
        {
            assert(wrapper_count <= lv.m_wrappers.size());
        }

    public:
        LValue clone() const {
            return ::MIR::LValue( m_lv->m_root.clone(), ::std::vector<Wrapper>(m_lv->m_wrappers.begin(), m_lv->m_wrappers.begin() + m_wrapper_count) );
        }

        const LValue& lv() const { return *m_lv; }
        size_t wrapper_count() const { return m_wrapper_count; }

        /// Unwrap one level, returning false if already at the root
        bool try_unwrap() {
            if( m_wrapper_count == 0 ) {
                return false;
            }
            else {
                m_wrapper_count --;
                return true;
            }
        }

        enum Tag {
            TAGDEAD,
            TAG_Return,
            TAG_Argument,
            TAG_Local,
            TAG_Static,
            TAG_Deref,
            TAG_Field,
            TAG_Downcast,
            TAG_Index,
        };
        Tag tag() const {
            if( m_wrapper_count == 0 )
            {
                switch(m_lv->m_root.tag())
                {
                case Storage::TAGDEAD:  return TAGDEAD;
                case Storage::TAG_Return:   return TAG_Return;
                case Storage::TAG_Argument: return TAG_Argument;
                case Storage::TAG_Local:    return TAG_Local;
                case Storage::TAG_Static:   return TAG_Static;
                }
            }
            else
            {
                switch(m_lv->m_wrappers[m_wrapper_count-1].tag())
                {
                case Wrapper::TAGDEAD:  return TAGDEAD;
                case Wrapper::TAG_Deref:    return TAG_Deref;
                case Wrapper::TAG_Field:    return TAG_Field;
                case Wrapper::TAG_Downcast: return TAG_Downcast;
                case Wrapper::TAG_Index:    return TAG_Index;
                }
            }
            return TAGDEAD;
        }

        bool is_Local   () const { return m_wrapper_count == 0 && m_lv->m_root.is_Local   (); }
        bool is_Return  () const { return m_wrapper_count == 0 && m_lv->m_root.is_Return  (); }
        bool is_Argument() const { return m_wrapper_count == 0 && m_lv->m_root.is_Argument(); }
        bool is_Static  () const { return m_wrapper_count == 0 && m_lv->m_root.is_Static  (); }
        bool is_Deref   () const { return m_wrapper_count >= 1 && m_lv->m_wrappers[m_wrapper_count-1].is_Deref   (); }
        bool is_Field   () const { return m_wrapper_count >= 1 && m_lv->m_wrappers[m_wrapper_count-1].is_Field   (); }
        bool is_Downcast() const { return m_wrapper_count >= 1 && m_lv->m_wrappers[m_wrapper_count-1].is_Downcast(); }
        bool is_Index   () const { return m_wrapper_count >= 1 && m_lv->m_wrappers[m_wrapper_count-1].is_Index   (); }

        const unsigned   as_Local   () const { assert(is_Local   ()); return m_lv->m_root.as_Local   (); }
        const char       as_Return  () const { assert(is_Return  ()); return m_lv->m_root.as_Return  (); }
        const unsigned   as_Argument() const { assert(is_Argument()); return m_lv->m_root.as_Argument(); }
        const HIR::Path& as_Static  () const { assert(is_Static  ()); return m_lv->m_root.as_Static  (); }
        const char     as_Deref   () const { assert(is_Deref   ()); return m_lv->m_wrappers[m_wrapper_count-1].as_Deref   (); }
        const unsigned as_Field   () const { assert(is_Field   ()); return m_lv->m_wrappers[m_wrapper_count-1].as_Field   (); }
        const unsigned as_Downcast() const { assert(is_Downcast()); return m_lv->m_wrappers[m_wrapper_count-1].as_Downcast(); }
        const unsigned as_Index   () const { assert(is_Index   ()); return m_lv->m_wrappers[m_wrapper_count-1].as_Index   (); }

        void fmt(::std::ostream& os) const;
        Ordering ord(const RefCommon& b) const;
    };

    class CRef: public RefCommon
    {
    public:
        CRef(const LValue& lv)
            :RefCommon(lv, lv.m_wrappers.size())
        {
        }
        CRef(const LValue& lv, size_t wc)
            :RefCommon(lv, wc)
        {
        }

        /// Unwrap one level
        const CRef inner_ref() const {
            assert(m_wrapper_count > 0);
            auto rv = *this;
            rv.m_wrapper_count--;
            return rv;
        }

        friend ::std::ostream& operator<<(::std::ostream& os, const CRef& x) {
            x.fmt(os);
            return os;
        }

        bool operator<(const CRef& b) const {
            return this->ord(b) == OrdLess;
        }
        bool operator==(const CRef& b) const {
            return this->ord(b) == OrdEqual;
        }
    };
    class MRef: public RefCommon
    {
    public:
        MRef(LValue& lv)
            :RefCommon(lv, lv.m_wrappers.size())
        {
        }

        operator CRef() const {
            return CRef(*m_lv, m_wrapper_count);
        }

        MRef inner_ref() {
            assert(m_wrapper_count > 0);
            auto rv = *this;
            rv.m_wrapper_count--;
            return rv;
        }
        void replace(LValue x) {
            auto& mut_lv = const_cast<LValue&>(*m_lv);
            // Shortcut: No wrappers on source/destination (just assign the slot/root)
            if( m_wrapper_count == 0 && x.m_wrappers.empty() ) {
                mut_lv.m_root = ::std::move(x.m_root);
                return ;
            }
            // If there's wrappers on this value (assigning over inner portion)
            if( m_wrapper_count < m_lv->m_wrappers.size() ) {
                // Add those wrappers to the end of the new value
                x.m_wrappers.insert(x.m_wrappers.end(), m_lv->m_wrappers.begin() + m_wrapper_count, m_lv->m_wrappers.end());
            }
            // Overwrite
            mut_lv = ::std::move(x);
        }

        friend ::std::ostream& operator<<(::std::ostream& os, const MRef& x) {
            x.fmt(os);
            return os;
        }
    };

    Ordering ord(const LValue::CRef& x) const;
    Ordering ord(const LValue::MRef& x) const;
};
extern ::std::ostream& operator<<(::std::ostream& os, const LValue& x);
extern ::std::ostream& operator<<(::std::ostream& os, const LValue::Wrapper& x);
static inline bool operator<(const LValue& a, const LValue::CRef& b) {
    return a.ord(b) == OrdLess;
}
static inline bool operator<(const LValue& a, const LValue::MRef& b) {
    return a.ord(b) == OrdLess;
}
static inline bool operator<(const LValue::CRef& a, const LValue& b) {
    return b.ord(a) == OrdGreater;
}
static inline bool operator<(const LValue::MRef& a, const LValue& b) {
    return b.ord(a) == OrdGreater;
}
static inline bool operator<(const LValue& a, const LValue& b) {
    return a.ord(b) == OrdLess;
}
static inline bool operator==(const LValue& a, const LValue& b) {
    return a.ord(b) == OrdEqual;
}
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
    (Generic, ::HIR::GenericRef),
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
TAGGED_UNION_EX(Param, (), Constant, (
    (LValue, LValue),
    // TODO: Add `Borrow` here (makes some MIR manipulation more complex, but simplifies emitted code)
    (Borrow, struct {
        ::HIR::BorrowType   type;
        LValue  val;
        }),
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

TAGGED_UNION_EX(RValue, (), Tuple, (
    // TODO: Split "Use" into "Copy" and "Move" (Where 'move' indicates that the source is unused)
    (Use, LValue),
    (Borrow, struct {
        ::HIR::BorrowType   type;
        LValue  val;
        }),
    (Constant, Constant),
    (SizedArray, struct {
        Param   val;
        ::HIR::ArraySize    count;
        }),
    // Cast on primitives (thin pointers, integers, floats)
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
    // OR: (if `meta_val` is `Constant::ItemAddr(nullptr)`) A still-to-be-resolved unsizing coercion
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
    // Create a new instance of a union
    (UnionVariant, struct {
        ::HIR::GenericPath  path;
        unsigned int index;
        Param   val;
        }),
    // Create a new instance of an enum
    // - Separate from UnionVariant, as the contents is needed when creating the body
    (EnumVariant, struct {
        ::HIR::GenericPath  path;
        unsigned int index;
        ::std::vector<Param>   vals;
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
    (String, ::std::vector<::std::string>),
    (ByteString, ::std::vector<::std::vector<uint8_t>>)
    ), (),(), (
        SwitchValues clone() const;
        bool operator==(const SwitchValues& x) const;
        bool operator!=(const SwitchValues& x) const { return !(*this == x); }
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
extern bool operator==(const Terminator& a, const Terminator& b);
static inline bool operator!=(const Terminator& a, const Terminator& b) {
    return !(a == b);
}

TAGGED_UNION(AsmParam, Const,
    (Const, ::MIR::Constant),
    (Sym, ::HIR::Path),
    (Reg, struct {
        AsmCommon::Direction    dir;
        AsmCommon::RegisterSpec spec;
        std::unique_ptr<MIR::Param> input;
        std::unique_ptr<MIR::LValue> output;
        })
    );
extern bool operator==(const AsmParam& a, const AsmParam& b);

enum class eDropKind {
    SHALLOW,
    DEEP,
};
TAGGED_UNION(Statement, Asm,
    // Value assigment
    (Assign, struct {
        LValue  dst;
        RValue  src;
        }),
    // Inline assembly (`llvm_asm!`)
    (Asm, struct {
        ::std::string   tpl;
        ::std::vector< ::std::pair<::std::string,LValue> >  outputs;
        ::std::vector< ::std::pair<::std::string,LValue> >  inputs;
        ::std::vector< ::std::string>   clobbers;
        ::std::vector< ::std::string>   flags;
        }),
    // Inline assembly (stabilised)
    (Asm2, struct {
        AsmCommon::Options  options;
        std::vector<AsmCommon::Line>   lines;
        ::std::vector<AsmParam> params;
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
    operator bool() { return p != nullptr; }
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

