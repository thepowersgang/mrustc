
#ifndef _HIR_TYPE_HPP_
#define _HIR_TYPE_HPP_
#pragma once

#include <tagged_union.hpp>
#include <hir/path.hpp>
#include <hir/expr_ptr.hpp>

namespace HIR {

struct TypeRef;

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
enum class BorrowType
{
    Shared,
    Unique,
    Owned,
};

struct LifetimeRef
{
    ::std::string   name;
};

struct FunctionType
{
    bool    is_unsafe;
    ::std::string   m_abi;
    ::std::unique_ptr<TypeRef>  m_rettype;
    ::std::vector<TypeRef>  m_arg_types;
};

struct TypeRef
{
    // Options:
    // - Primitive
    // - Parameter
    // - Path
    
    // - Array
    // - Tuple
    // - Borrow
    // - Pointer

    TAGGED_UNION(Data, Infer,
    (Infer, struct {}),
    (Primitive, ::HIR::CoreType),
    (Path,  ::HIR::Path),
    (Generic, struct {
        ::std::string   name;
        unsigned int    binding;
        }),
    (TraitObject, struct {
        ::std::vector< ::HIR::GenericPath > m_traits;
        ::HIR::LifetimeRef  m_lifetime;
        }),
    (Array, struct {
        ::std::unique_ptr<TypeRef>  inner;
        ::HIR::ExprPtr size;
        }),
    (Tuple, ::std::vector<TypeRef>),
    (Borrow, struct {
        ::HIR::BorrowType   type;
        ::std::unique_ptr<TypeRef>  inner;
        }),
    (Pointer, struct {
        bool    is_mut;
        ::std::unique_ptr<TypeRef>  inner;
        }),
    (Function, FunctionType)
    );
    
    Data   m_data;
    
    TypeRef() {}
    TypeRef(TypeRef&& ) = default;
    TypeRef(const TypeRef& ) = delete;
    
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
        m_data( Data::make_Path(mv$(p)) )
    {}
};

}   // namespace HIR

#endif

