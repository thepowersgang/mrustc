/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * hir_sim.hpp
 * - Clones of the mrustc HIR types (HEADER)
 */
#pragma once
#include <string>
#include <vector>
#include <memory>
#include "../../src/include/rc_string.hpp"

const size_t POINTER_SIZE = 8;

#define __ORD(fld)  do { auto o = ::ord(this->fld, x.fld); if( o != OrdEqual )  return o; } while(0)
#define __ORD_C(ty, fld)  do { auto o = ::ord((ty)this->fld, (ty)x.fld); if( o != OrdEqual )  return o; } while(0)
#define __NE(fld)  if(this->fld != x.fld) return true
#define __LT(fld)  if(this->fld != x.fld) return this->fld < x.fld

#if 0
enum Ordering
{
    OrdLess,
    OrdEqual,
    OrdGreater,
};
#endif

struct DataType;
struct FunctionType;

enum class RawType
{
    Unreachable,
    Function,   // TODO: Needs a way of indicating the signature?
    Unit,

    Bool,
    U8, I8,
    U16, I16,
    U32, I32,
    U64, I64,
    U128, I128,
    USize, ISize,

    F32, F64,

    Char, Str,

    Composite,  // Struct, Enum, Union, tuple, ...
    TraitObject,    // Data pointer is `*const ()`, vtable type stored in `composite_type`
};
static inline Ordering ord(RawType a, RawType b) {
    return ord( int(a), int(b) );
}
struct TypeWrapper
{
    enum class Ty
    {
        Array,  // With size
        Borrow, // With BorrowType
        Pointer,    // With BorrowType
        Slice,  // Must be bottom
    } type;
    size_t  size;

    Ordering ord(const TypeWrapper& x) const {
        __ORD_C(int, type);
        __ORD(size);
        return OrdEqual;
    }
    bool operator==(const TypeWrapper& x) const {
        return this->ord(x) == OrdEqual;
    }
    bool operator!=(const TypeWrapper& x) const {
        return this->ord(x) != OrdEqual;
    }
    bool operator<(const TypeWrapper& x) const {
        return this->ord(x) == OrdLess;
    }
};

namespace HIR {

    enum class BorrowType
    {
        Shared,
        Unique,
        Move,
    };
    ::std::ostream& operator<<(::std::ostream& os, const BorrowType& x);
    struct CoreType
    {
        RawType raw_type;
    };

    struct GenericRef
    {
        // Should never be needed!
        friend ::std::ostream& operator<<(::std::ostream& os, const GenericRef& x) {
            return os << "GenericRef";
        }
    };
    struct ArraySize {
        unsigned    count;
        friend ::std::ostream& operator<<(::std::ostream& os, const ArraySize& x) {
            return os << x.count;
        }
        bool operator<(const ArraySize& o) const { return count < o.count; }
    };

    /// Definition of a type
    struct TypeRef
    {
        // Top to bottom list of wrappers (first entry is the outermost wrapper)
        ::std::vector<TypeWrapper>  wrappers;
        RawType inner_type = RawType::Unit;
        union {
            const DataType* composite_type;
            const FunctionType* function_type;
        } ptr;

        TypeRef()
        {
            ptr.composite_type = nullptr;
        }

        explicit TypeRef(const DataType* dt):
            inner_type(RawType::Composite)
        {
            ptr.composite_type = dt;
        }
        explicit TypeRef(const FunctionType* fp):
            inner_type(RawType::Function)
        {
            ptr.function_type = fp;
        }
        explicit TypeRef(RawType rt):
            inner_type(rt)
        {
            ptr.composite_type = nullptr;
        }
        explicit TypeRef(CoreType ct):
            inner_type(ct.raw_type)
        {
            ptr.composite_type = nullptr;
        }
        static TypeRef diverge() {
            TypeRef rv;
            rv.inner_type = RawType::Unreachable;
            return rv;
        }
        static TypeRef unit() {
            TypeRef rv;
            rv.inner_type = RawType::Unit;
            return rv;
        }

        size_t get_size(size_t ofs=0) const;
        size_t get_align(size_t ofs=0) const;

        // Returns true if this (unsized) type is a wrapper around a slice
        // - Fills `out_inner_size` with the size of the slice element
        bool has_slice_meta(size_t& out_inner_size) const;    // The attached metadata is a count of elements
        // Returns the base unsized type for this type (returning nullptr if there's no unsized field)
        // - Fills `running_inner_size` with the offset to the unsized field
        const TypeRef* get_unsized_type(size_t& running_inner_size) const;
        // Returns the type of associated metadata for this (unsized) type (or `!` if not unsized)
        TypeRef get_meta_type() const;
        // Get the inner type (one level of wrapping removed)
        TypeRef get_inner() const;

        // Add a wrapper over this type (moving)
        TypeRef wrap(TypeWrapper::Ty ty, size_t size)&&;
        // Add a wrapper over this type (copying)
        TypeRef wrapped(TypeWrapper::Ty ty, size_t size) const {
            return TypeRef(*this).wrap(ty, size);
        }
        // Get the wrapper at the provided offset (0 = outermost)
        const TypeWrapper* get_wrapper(size_t ofs=0) const {
            //assert(ofs <= this->wrappers.size());
            if( ofs < this->wrappers.size() ) {
                return &this->wrappers[ofs];
            }
            else {
                return nullptr;
            }
        }

        // Returns true if the type contains any pointers
        bool has_pointer() const;
        // Get the type and offset of the specified field index
        TypeRef get_field(size_t idx, size_t& ofs) const;
        // Get the offset and type of a field (recursing using `other_idx`)
        size_t get_field_ofs(size_t idx, const ::std::vector<size_t>& other_idx,  TypeRef& ty) const;

        const DataType& composite_type() const {
            assert(inner_type == RawType::Composite || inner_type == RawType::TraitObject);
            assert(ptr.composite_type);
            return *ptr.composite_type;
        }
        const FunctionType& function_type() const {
            assert(inner_type == RawType::Function);
            assert(ptr.function_type);
            return *ptr.function_type;
        }

        bool operator==(const RawType& x) const {
            if( this->wrappers.size() != 0 )
                return false;
            return this->inner_type == x;
        }
        bool operator!=(const RawType& x) const {
            return !(*this == x);
        }
        Ordering ord(const TypeRef& x) const {
            __ORD(wrappers);
            __ORD_C(int, inner_type);
            __ORD_C(uintptr_t, ptr.composite_type); // pointer comparison only
            return OrdEqual;
        }
        bool operator==(const TypeRef& x) const {
            return this->ord(x) == OrdEqual;
        }
        bool operator!=(const TypeRef& x) const {
            return this->ord(x) != OrdEqual;
        }
        bool operator<(const TypeRef& x) const {
            return this->ord(x) == OrdLess;
        }

        friend ::std::ostream& operator<<(::std::ostream& os, const TypeRef& x);
    };


    struct PathParams
    {
        ::std::vector<TypeRef>  tys;

        friend ::std::ostream& operator<<(::std::ostream& os, const PathParams& x);
    };

    struct Path {
        RcString    n;

        Ordering ord(const Path& x) const {
            return ::ord(n, x.n);
        }

        bool operator==(const Path& p) const { return ord(p) == OrdEqual; }
        bool operator!=(const Path& p) const { return ord(p) != OrdEqual; }
        friend ::std::ostream& operator<<(::std::ostream& os, const Path& x){
            return os << x.n;
        }
    };
    struct GenericPath {
        RcString    n;

        friend ::std::ostream& operator<<(::std::ostream& os, const GenericPath& x){
            return os << x.n;
        }
    };

} // nameapce HIR


#undef __NE
#undef __LT
