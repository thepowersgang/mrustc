//
//
//
#pragma once
#include <string>
#include <vector>
#include <memory>

const size_t POINTER_SIZE = 8;

#define __NE(fld)  if(this->fld != x.fld) return true
#define __LT(fld)  if(this->fld != x.fld) return this->fld < x.fld

enum Ordering
{
    OrdLess,
    OrdEqual,
    OrdGreater,
};

struct DataType;

enum class RawType
{
    Unreachable,
    Function,
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
    TraitObject,    // Data pointer is `*const ()`, metadata type stored in `composite_type`
};
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

    bool operator==(const TypeWrapper& x) const {
        return !(*this != x);
    }
    bool operator!=(const TypeWrapper& x) const {
        __NE(type);
        __NE(size);
        return false;
    }
    bool operator<(const TypeWrapper& x) const {
        __LT(type);
        __LT(size);
        return false;
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
    /// Definition of a type
    struct TypeRef
    {
        // Top to bottom list of wrappers (first entry is the outermost wrapper)
        ::std::vector<TypeWrapper>  wrappers;
        RawType inner_type = RawType::Unit;
        const DataType* composite_type = nullptr;

        TypeRef()
        {
        }

        explicit TypeRef(const DataType* dt):
            inner_type(RawType::Composite),
            composite_type(dt)
        {
        }
        explicit TypeRef(RawType rt):
            inner_type(rt)
        {
        }
        explicit TypeRef(CoreType ct):
            inner_type(ct.raw_type)
        {
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
        bool has_slice_meta() const;    // The attached metadata is a count
        TypeRef get_inner() const;
        TypeRef get_field(size_t idx, size_t& ofs) const;

        bool operator==(const RawType& x) const {
            if( this->wrappers.size() != 0 )
                return false;
            return this->inner_type == x;
        }
        bool operator!=(const RawType& x) const {
            return !(*this == x);
        }
        bool operator==(const TypeRef& x) const {
            return !(*this != x);
        }
        bool operator!=(const TypeRef& x) const {
            __NE(wrappers);
            __NE(inner_type);
            __NE(composite_type);
            return false;
        }
        bool operator<(const TypeRef& x) const {
            __LT(wrappers);
            __LT(inner_type);
            __LT(composite_type);
            return false;
        }

        size_t get_field_ofs(size_t idx, const ::std::vector<size_t>& other_idx,  TypeRef& ty) const;

        friend ::std::ostream& operator<<(::std::ostream& os, const TypeRef& x);
    };

    struct SimplePath
    {
        ::std::string   crate_name;
        ::std::vector<::std::string>    ents;
        bool operator==(const SimplePath& x) const {
            return !(*this != x);
        }
        bool operator!=(const SimplePath& x) const {
            __NE(crate_name);
            __NE(ents);
            return false;
        }
        bool operator<(const SimplePath& x) const {
            __LT(crate_name);
            __LT(ents);
            return false;
        }
        friend ::std::ostream& operator<<(::std::ostream& os, const SimplePath& x);
    };

    struct PathParams
    {
        ::std::vector<TypeRef>  tys;

        friend ::std::ostream& operator<<(::std::ostream& os, const PathParams& x);
    };
    struct GenericPath
    {
        SimplePath  m_simplepath;
        PathParams  m_params;

        GenericPath() {}
        GenericPath(SimplePath sp):
            m_simplepath(sp)
        {
        }
        bool operator==(const GenericPath& x) const {
            return !(*this != x);
        }
        bool operator!=(const GenericPath& x) const {
            __NE(m_simplepath);
            __NE(m_params.tys);
            return false;
        }
        bool operator<(const GenericPath& x) const {
            __LT(m_simplepath);
            __LT(m_params.tys);
            return false;
        }

        friend ::std::ostream& operator<<(::std::ostream& os, const GenericPath& x);
    };
    struct Path
    {
        TypeRef m_type;
        GenericPath m_trait;
        ::std::string   m_name; // if empty, the path is Generic in m_trait
        PathParams  m_params;

        Path(SimplePath sp):
            Path(GenericPath(sp))
        {
        }
        Path(GenericPath gp):
            m_trait(gp)
        {
        }
        Path(TypeRef ty, GenericPath trait, ::std::string name, PathParams params):
            m_type(ty),
            m_trait(::std::move(trait)),
            m_name(name),
            m_params(params)
        {
        }

        bool operator==(const Path& x) const {
            return !(*this != x);
        }
        bool operator!=(const Path& x) const {
            __NE(m_type);
            __NE(m_trait);
            __NE(m_name);
            __NE(m_params.tys);
            return false;
        }
        bool operator<(const Path& x) const {
            __LT(m_type);
            __LT(m_trait);
            __LT(m_name);
            __LT(m_params.tys);
            return false;
        }

        friend ::std::ostream& operator<<(::std::ostream& os, const Path& x);
    };

} // nameapce HIR


#undef __NE
#undef __LT
