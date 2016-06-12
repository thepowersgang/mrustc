
#ifndef _HIR_TYPE_HPP_
#define _HIR_TYPE_HPP_
#pragma once

#include <tagged_union.hpp>
#include <hir/path.hpp>
#include <hir/expr_ptr.hpp>
#include <span.hpp>

namespace HIR {

class Struct;
class Enum;
struct ExprNode_Closure;

class TypeRef;

typedef ::std::function<void(unsigned int, const ::HIR::TypeRef&)> t_cb_match_generics;

enum class InferClass
{
    None,
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
    
    F32, F64,
    
    Bool,
    Char, Str,
};
extern ::std::ostream& operator<<(::std::ostream& os, const CoreType& ct);

enum class BorrowType
{
    Shared,
    Unique,
    Owned,
};

struct LifetimeRef
{
    ::std::string   name;
    
    bool operator==(const LifetimeRef& x) const {
        return name == x.name;
    }
};

struct FunctionType
{
    bool    is_unsafe;
    ::std::string   m_abi;
    ::std::unique_ptr<TypeRef>  m_rettype;
    ::std::vector<TypeRef>  m_arg_types;
};

class TypeRef
{
public:
    // Options:
    // - Primitive
    // - Parameter
    // - Path
    
    // - Array
    // - Tuple
    // - Borrow
    // - Pointer

    TAGGED_UNION_EX(TypePathBinding, (), Unbound, (
    (Unbound, struct {}),   // Not yet bound, either during lowering OR during resolution (when associated and still being resolved)
    (Opaque, struct {}),    // Opaque, i.e. An associated type of a generic (or Self in a trait)
    (Struct, const ::HIR::Struct*),
    (Enum, const ::HIR::Enum*)
    ), (), (), (
        TypePathBinding clone() const;
    )
    );

    TAGGED_UNION(Data, Infer,
    (Infer, struct {
        unsigned int index = ~0u;
        InferClass  ty_class = InferClass::None;
        }),
    (Diverge, struct {}),
    (Primitive, ::HIR::CoreType),
    (Path, struct {
        ::HIR::Path path;
        TypePathBinding binding;
        }),
    (Generic, struct {
        ::std::string   name;
        unsigned int    binding;
        }),
    (TraitObject, struct {
        ::HIR::TraitPath    m_trait;
        ::std::vector< ::HIR::GenericPath > m_markers;
        ::HIR::LifetimeRef  m_lifetime;
        }),
    (Array, struct {
        ::std::unique_ptr<TypeRef>  inner;
        ::HIR::ExprPtr size;
        size_t  size_val;
        }),
    (Slice, struct {
        ::std::unique_ptr<TypeRef>  inner;
        }),
    (Tuple, ::std::vector<TypeRef>),
    (Borrow, struct {
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
    
    Data   m_data;
    
    TypeRef() {}
    TypeRef(TypeRef&& ) = default;
    TypeRef(const TypeRef& ) = delete;
    TypeRef& operator=(TypeRef&& ) = default;
    TypeRef& operator=(const TypeRef&) = delete;
    
    struct TagUnit {};
    TypeRef(TagUnit ):
        m_data( Data::make_Tuple({}) )
    {}

    TypeRef(::std::vector< ::HIR::TypeRef> sts):
        m_data( Data::make_Tuple(mv$(sts)) )
    {}
    TypeRef(::std::string name, unsigned int slot):
        m_data( Data::make_Generic({ mv$(name), slot }) )
    {}
    TypeRef(::HIR::TypeRef::Data x):
        m_data( mv$(x) )
    {}
    TypeRef(::HIR::CoreType ct):
        m_data( Data::make_Primitive(mv$(ct)) )
    {}
    TypeRef(::HIR::Path p):
        m_data( Data::make_Path( {mv$(p), TypePathBinding()} ) )
    {}

    static TypeRef new_unit() {
        return TypeRef(Data::make_Tuple({}));
    }
    static TypeRef new_diverge() {
        return TypeRef(Data::make_Diverge({}));
    }
    static TypeRef new_borrow(BorrowType bt, TypeRef inner) {
        return TypeRef(Data::make_Borrow({bt, box$(mv$(inner))}));
    }
    static TypeRef new_pointer(BorrowType bt, TypeRef inner) {
        return TypeRef(Data::make_Pointer({bt, box$(mv$(inner))}));
    }
    static TypeRef new_slice(TypeRef inner) {
        return TypeRef(Data::make_Slice({box$(mv$(inner))}));
    }
    static TypeRef new_array(TypeRef inner, unsigned int size) {
        assert(size != ~0u);
        return TypeRef(Data::make_Array({box$(mv$(inner)), ::HIR::ExprPtr(), size}));
    }
    static TypeRef new_array(TypeRef inner, ::HIR::ExprPtr size_expr) {
        return TypeRef(Data::make_Array({box$(mv$(inner)), mv$(size_expr), ~0u}));
    }
    static TypeRef new_path(::HIR::Path path, TypePathBinding binding) {
        return TypeRef(Data::make_Path({ mv$(path), mv$(binding) }));
    }
    
    TypeRef clone() const;
    
    void fmt(::std::ostream& os) const;
    
    bool operator==(const ::HIR::TypeRef& x) const;
    bool operator!=(const ::HIR::TypeRef& x) const { return !(*this == x); }

    
    // Match generics in `this` with types from `x`
    // Raises a bug against `sp` if there is a form mismatch or `this` has an infer
    void match_generics(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder, ::std::function<void(unsigned int, const ::HIR::TypeRef&)> callback) const;
    
    bool match_test_generics(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder, ::std::function<void(unsigned int, const ::HIR::TypeRef&)> callback) const;
    
    // Compares this type with another, using `resolve_placeholder` to get replacements for generics/infers in `x`
    Compare compare_with_placeholders(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder) const;
};

extern ::std::ostream& operator<<(::std::ostream& os, const ::HIR::TypeRef& ty);

}   // namespace HIR

#endif

