
#ifndef _HIR_TYPE_HPP_
#define _HIR_TYPE_HPP_
#pragma once

#include <tagged_union.hpp>
#include <hir/path.hpp>
#include <hir/expr_ptr.hpp>

namespace HIR {

enum class CoreType
{
    Usize, Isize,
    U8, I8,
    U16, I16,
    U32, I32,
    U64, I64,
    
    F32, F64,
    
    Char, Str,
};
enum class BorrowType
{
    Shared,
    Unique,
    Owned,
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
        })
    );
    
    Data   type;
    
    
    TypeRef(::HIR::Path _);
};

}   // namespace HIR

#endif

