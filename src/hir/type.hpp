
#ifndef _HIR_TYPE_HPP_
#define _HIR_TYPE_HPP_
#pragma once

#include <hir/path.hpp>

namespace HIR {

class TypeRef
{
    // Options:
    // - Primitive
    // - Parameter
    // - Path
    
    // - Array
    // - Tuple
    // - Borrow
    // - Pointer
public:
    TypeRef(::HIR::Path _);
};

}   // namespace HIR

#endif

