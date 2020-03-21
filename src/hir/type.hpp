/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/type.hpp
 * - HIR Type representation
 */
#ifndef _HIR_TYPE_HPP_
#define _HIR_TYPE_HPP_
#pragma once

#include <tagged_union.hpp>
#include <hir/path.hpp>
#include <hir/expr_ptr.hpp>
#include <span.hpp>

/// Binding index for a Generic that indicates "Self"
#define GENERIC_Self    0xFFFF

constexpr const char* CLOSURE_PATH_PREFIX = "closure#";

namespace HIR {

struct TraitMarkings;
class ExternType;
class Struct;
class Union;
class Enum;
struct ExprNode_Closure;

class TypeRef;

enum class InferClass
{
    None,
    Diverge,
    Integer,
    Float,
};

enum class CoreType
{
    Usize, Isize,
    U8, I8,
    U16, I16,
    U32, I32,
    U64, I64,
    U128, I128,

    F32, F64,

    Bool,
    Char, Str,
};
extern ::std::ostream& operator<<(::std::ostream& os, const CoreType& ct);
static inline bool is_integer(const CoreType& v) {
    switch(v)
    {
    case CoreType::Usize: case CoreType::Isize:
    case CoreType::U8 : case CoreType::I8:
    case CoreType::U16: case CoreType::I16:
    case CoreType::U32: case CoreType::I32:
    case CoreType::U64: case CoreType::I64:
    case CoreType::U128: case CoreType::I128:
        return true;
    default:
        return false;
    }
}
static inline bool is_float(const CoreType& v) {
    switch(v)
    {
    case CoreType::F32:
    case CoreType::F64:
        return true;
    default:
        return false;
    }
}

enum class BorrowType
{
    Shared,
    Unique,
    Owned,
};
extern ::std::ostream& operator<<(::std::ostream& os, const BorrowType& bt);

struct LifetimeRef
{
    static const uint32_t UNKNOWN = 0;
    static const uint32_t STATIC = 0xFFFF;

    //RcString  name;
    // Values below 2^16 are parameters/static, values above are per-function region IDs allocated during region inferrence.
    uint32_t  binding = UNKNOWN;

    LifetimeRef()
        :binding(UNKNOWN)
    {
    }
    LifetimeRef(uint32_t binding)
        :binding(binding)
    {
    }

    static LifetimeRef new_static() {
        LifetimeRef rv;
        rv.binding = STATIC;
        return rv;
    }

    Ordering ord(const LifetimeRef& x) const {
        return ::ord(binding, x.binding);
    }
    bool operator==(const LifetimeRef& x) const {
        return binding == x.binding;
    }
    bool operator!=(const LifetimeRef& x) const {
        return !(*this == x);
    }
    friend ::std::ostream& operator<<(::std::ostream& os, const LifetimeRef& x) {
        if( x.binding == UNKNOWN )
        {
            os << "'_";
        }
        else if( x.binding == STATIC )
        {
            os << "'static";
        }
        else if( x.binding < 0xFFFF )
        {
            switch( x.binding & 0xFF00 )
            {
            case 0: os << "'I" << (x.binding & 0xFF);   break;
            case 1: os << "'M" << (x.binding & 0xFF);   break;
            default: os << "'unk" << x.binding;   break;
            }
        }
        else
        {
            os << "'_" << (x.binding - 0x1000);
        }
        return os;
    }
};

struct FunctionType
{
    bool    is_unsafe;
    ::std::string   m_abi;
    ::std::unique_ptr<TypeRef>  m_rettype;
    ::std::vector<TypeRef>  m_arg_types;
};

/// Array size used for types AND array literals
TAGGED_UNION_EX(ArraySize, (), Unevaluated, (
    /// Un-evaluated size (still an expression)
    (Unevaluated, ::std::shared_ptr<::HIR::ExprPtr>),
    /// Compiled and partially evaluated
    //(LiteralExpr, ::HIR::LiteralExpr),
    /// Fully known
    (Known, uint64_t)
    ),
    /*extra_move=*/(),
    /*extra_assign=*/(),
    /*extra=*/(
        ArraySize clone() const;
        Ordering ord(const ArraySize& x) const;
        bool operator==(const ArraySize& x) const { return ord(x) == OrdEqual; }
        bool operator!=(const ArraySize& x) const { return !operator==(x); }
    )
    );
extern ::std::ostream& operator<<(::std::ostream& os, const ArraySize& x);


TAGGED_UNION_EX(TypePathBinding, (), Unbound, (
    (Unbound, struct {}),   // Not yet bound, either during lowering OR during resolution (when associated and still being resolved)
    (Opaque, struct {}),    // Opaque, i.e. An associated type of a generic (or Self in a trait)
    (ExternType, const ::HIR::ExternType*),
    (Struct, const ::HIR::Struct*),
    (Union, const ::HIR::Union*),
    (Enum, const ::HIR::Enum*)
    ), (), (), (
        TypePathBinding clone() const;

        const TraitMarkings* get_trait_markings() const;

        bool operator==(const TypePathBinding& x) const;
        bool operator!=(const TypePathBinding& x) const { return !(*this == x); }
    )
    );

TAGGED_UNION(TypeData, Diverge,
    (Infer, struct {
        unsigned int index;
        InferClass  ty_class;

        /// Returns true if the ivar is a literal
        bool is_lit() const {
            switch(this->ty_class)
            {
            case InferClass::None:
            case InferClass::Diverge:
                return false;
            case InferClass::Integer:
            case InferClass::Float:
                return true;
            }
            throw "";
        }
        }),
    (Diverge, struct {}),
    (Primitive, ::HIR::CoreType),
    (Path, struct {
        ::HIR::Path path;
        TypePathBinding binding;

        bool is_closure() const {
            return path.m_data.is_Generic()
                && path.m_data.as_Generic().m_path.m_components.back().size() > 8
                && path.m_data.as_Generic().m_path.m_components.back().compare(0,strlen(CLOSURE_PATH_PREFIX), CLOSURE_PATH_PREFIX) == 0
                ;
        }
        }),
    (Generic, struct {
        RcString    name;
        // 0xFFFF = Self, 0-255 = Type/Trait, 256-511 = Method, 512-767 = Placeholder
        unsigned int    binding;

        bool is_placeholder() const {
            return (binding >> 8) == 2;
        }
        }),
    (TraitObject, struct {
        ::HIR::TraitPath    m_trait;
        ::std::vector< ::HIR::GenericPath > m_markers;
        ::HIR::LifetimeRef  m_lifetime;
        }),
    (ErasedType, struct {
        ::HIR::Path m_origin;
        unsigned int m_index;
        ::std::vector< ::HIR::TraitPath>    m_traits;
        ::HIR::LifetimeRef  m_lifetime;
        }),
    (Array, struct {
        ::std::unique_ptr<TypeRef>  inner;
        ArraySize  size;
        }),
    (Slice, struct {
        ::std::unique_ptr<TypeRef>  inner;
        }),
    (Tuple, ::std::vector<TypeRef>),
    (Borrow, struct {
        ::HIR::LifetimeRef  lifetime;
        ::HIR::BorrowType   type;
        ::std::unique_ptr<TypeRef>  inner;
        }),
    (Pointer, struct {
        ::HIR::BorrowType   type;
        ::std::unique_ptr<TypeRef>  inner;
        }),
    (Function, FunctionType),
    (Closure, struct {
        const ::HIR::ExprNode_Closure*  node;
        ::std::unique_ptr<TypeRef>  m_rettype;
        ::std::vector<TypeRef>  m_arg_types;
        })
    );

#if 0
class Type;
class TypeRef
{
    const Type* m_ptr;
    TypeRef(Type::Data d):
        m_ptr(new Type(d))
    {
    }
public:
    TypeRef():
        TypeRef(Type::Data::make_Infer({ ~0u, InferClass::None }))
    {
    }
    explicit TypeRef(const TypeRef& x):
        m_ptr(x.m_ptr)
    {
        x.m_ptr.m_refcount += 1;
    }
    TypeRef(TypeRef&& x):
        m_ptr(x.m_ptr)
    {
        x.m_ptr = nullptr;
    }
    ~TypeRef()
    {
        if(x.m_ptr)
        {
            x.m_ptr.m_refcount -= 1;
            if(x.m_ptr.m_refcount == 0)
            {
                delete x.m_ptr;
                x.m_ptr = nullptr;
            }
        }
    }

    const Type::Data& operator*() const { return m_ptr->m_data; }
    const Type::Data* operator->() const { return &m_ptr->m_data; }

    static TypeRef new_unit() {
        return TypeRef(Type::Data::make_Tuple({}));
    }
    static TypeRef new_diverge() {
        return TypeRef(Type::Data::make_Diverge({}));
    }
    static TypeRef new_infer(unsigned int idx = ~0u, InferClass ty_class = InferClass::None) {
        return TypeRef(Data::make_Infer({idx, ty_class}));
    }
    static TypeRef new_borrow(BorrowType bt, TypeRef inner) {
        return TypeRef(Data::make_Borrow({ ::HIR::LifetimeRef(), bt, mv$(inner) }));
    }
    static TypeRef new_pointer(BorrowType bt, TypeRef inner) {
        return TypeRef(Data::make_Pointer({bt, mv$(inner)}));
    }
    static TypeRef new_slice(TypeRef inner) {
        return TypeRef(Data::make_Slice({mv$(inner)}));
    }
    static TypeRef new_array(TypeRef inner, uint64_t size) {
        assert(size != ~0u);
        return TypeRef(Data::make_Array({mv$(inner), size}));
    }
    static TypeRef new_array(TypeRef inner, ::HIR::ExprPtr size_expr) {
        return TypeRef(Data::make_Array({mv$(inner), std::make_shared<HIR::ExprPtr>(mv$(size_expr)) }));
    }
    static TypeRef new_path(::HIR::Path path, TypePathBinding binding) {
        return TypeRef(Data::make_Path({ mv$(path), mv$(binding) }));
    }
    static TypeRef new_closure(::HIR::ExprNode_Closure* node_ptr, ::std::vector< ::HIR::TypeRef> args, ::HIR::TypeRef rv) {
        return TypeRef(Data::make_Closure({ node_ptr, mv$(rv), mv$(args) }));
    }

    TypeRef clone() const;
    void fmt(::std::ostream& os) const;
};
class Type
{
    friend class TypeRef;
public:
    // Existing TypeRef

private:
    unsigned    m_refcount;
public:
    Data   m_data;
private:
    Type(Data d):
        m_refcount(1),
        m_data(d)
    {
    }
};
#endif

// TODO: Convert to a shared_ptr (or interior RC)
// - How to handle resolution actions? (Replacing generics)
class TypeRef
{
public:
    TypeData   m_data;

    TypeRef():
        m_data(TypeData::make_Infer({ ~0u, InferClass::None }))
    {}
    TypeRef(TypeRef&& ) = default;
    TypeRef(const TypeRef& ) = delete;
    TypeRef& operator=(TypeRef&& ) = default;
    TypeRef& operator=(const TypeRef&) = delete;

    TypeRef(::HIR::TypeData x):
        m_data( mv$(x) )
    {}

    TypeRef(::std::vector< ::HIR::TypeRef> sts):
        m_data( TypeData::make_Tuple(mv$(sts)) )
    {}
    TypeRef(RcString name, unsigned int slot):
        m_data( TypeData::make_Generic({ mv$(name), slot }) )
    {}
    TypeRef(::HIR::CoreType ct):
        m_data( TypeData::make_Primitive(mv$(ct)) )
    {}

    static TypeRef new_unit() {
        return TypeRef(TypeData::make_Tuple({}));
    }
    static TypeRef new_diverge() {
        return TypeRef(TypeData::make_Diverge({}));
    }
    static TypeRef new_infer(unsigned int idx = ~0u, InferClass ty_class = InferClass::None) {
        return TypeRef(TypeData::make_Infer({idx, ty_class}));
    }
    static TypeRef new_borrow(BorrowType bt, TypeRef inner) {
        return TypeRef(TypeData::make_Borrow({ ::HIR::LifetimeRef(), bt, box$(mv$(inner)) }));
    }
    static TypeRef new_pointer(BorrowType bt, TypeRef inner) {
        return TypeRef(TypeData::make_Pointer({bt, box$(mv$(inner))}));
    }
    static TypeRef new_slice(TypeRef inner) {
        return TypeRef(TypeData::make_Slice({box$(mv$(inner))}));
    }
    static TypeRef new_array(TypeRef inner, uint64_t size) {
        assert(size != ~0u);
        return TypeRef(TypeData::make_Array({box$(mv$(inner)), size}));
    }
    static TypeRef new_array(TypeRef inner, ::HIR::ExprPtr size_expr) {
        return TypeRef(TypeData::make_Array({box$(mv$(inner)), std::make_shared<HIR::ExprPtr>(mv$(size_expr)) }));
    }
    static TypeRef new_path(::HIR::Path path, TypePathBinding binding) {
        return TypeRef(TypeData::make_Path({ mv$(path), mv$(binding) }));
    }
    static TypeRef new_closure(::HIR::ExprNode_Closure* node_ptr, ::std::vector< ::HIR::TypeRef> args, ::HIR::TypeRef rv) {
        return TypeRef(TypeData::make_Closure({ node_ptr, box$(mv$(rv)), mv$(args) }));
    }

    TypeRef clone() const;

    void fmt(::std::ostream& os) const;

    bool operator==(const ::HIR::TypeRef& x) const;
    bool operator!=(const ::HIR::TypeRef& x) const { return !(*this == x); }
    bool operator<(const ::HIR::TypeRef& x) const { return ord(x) == OrdLess; }
    Ordering ord(const ::HIR::TypeRef& x) const;

    bool contains_generics() const;

    // Match generics in `this` with types from `x`
    // Raises a bug against `sp` if there is a form mismatch or `this` has an infer
    void match_generics(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder, t_cb_match_generics) const;

    bool match_test_generics(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder, t_cb_match_generics) const;

    // Compares this type with another, calling the first callback to resolve placeholders in the other type, and the second callback for generics in this type
    ::HIR::Compare match_test_generics_fuzz(const Span& sp, const ::HIR::TypeRef& x_in, t_cb_resolve_type resolve_placeholder, t_cb_match_generics callback) const;

    // Compares this type with another, using `resolve_placeholder` to get replacements for generics/infers in `x`
    Compare compare_with_placeholders(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder) const;

    const ::HIR::SimplePath* get_sort_path() const {
        // - Generic paths get sorted
        if( TU_TEST1(this->m_data, Path, .path.m_data.is_Generic()) )
        {
            return &this->m_data.as_Path().path.m_data.as_Generic().m_path;
        }
        // - So do trait objects
        else if( this->m_data.is_TraitObject() )
        {
            return &this->m_data.as_TraitObject().m_trait.m_path.m_path;
        }
        else
        {
            // Keep as nullptr, will search primitive list
            return nullptr;
        }
    }
};

extern ::std::ostream& operator<<(::std::ostream& os, const ::HIR::TypeRef& ty);

}   // namespace HIR

#endif

