#ifndef TYPES_HPP_INCLUDED
#define TYPES_HPP_INCLUDED

#include <vector>
#include "coretypes.hpp"
#include "ast/ast.hpp"

class TypeRef
{
public:
    TypeRef() {}
    struct TagPrimitive {};
    TypeRef(TagPrimitive, enum eCoreType type) {}
    struct TagTuple {};
    TypeRef(TagTuple _, ::std::vector<TypeRef> inner_types) {}
    struct TagReference {};
    TypeRef(TagReference _, bool is_mut, TypeRef inner_type) {}
    struct TagPointer {};
    TypeRef(TagPointer _, bool is_mut, TypeRef inner_type) {}
    struct TagSizedArray {};
    TypeRef(TagSizedArray _, TypeRef inner_type, AST::Expr size) {}
    struct TagUnsizedArray {};
    TypeRef(TagUnsizedArray _, TypeRef inner_type) {}

    struct TagPath {};
    TypeRef(TagPath, AST::Path path) {}
};

#endif // TYPES_HPP_INCLUDED
