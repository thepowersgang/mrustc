#ifndef TYPES_HPP_INCLUDED
#define TYPES_HPP_INCLUDED

#include "common.hpp"
#include "coretypes.hpp"
#include "ast/path.hpp"

namespace AST {
class Expr;
}

class TypeRef
{
public:
    TypeRef() {}

    struct TagUnit {};  // unit maps to a zero-length tuple, just easier to type
    TypeRef(TagUnit) {}

    struct TagPrimitive {};
    TypeRef(TagPrimitive, enum eCoreType type) {}
    struct TagTuple {};
    TypeRef(TagTuple _, ::std::vector<TypeRef> inner_types) {}
    struct TagReference {};
    TypeRef(TagReference _, bool is_mut, TypeRef inner_type) {}
    struct TagPointer {};
    TypeRef(TagPointer _, bool is_mut, TypeRef inner_type) {}
    struct TagSizedArray {};
    TypeRef(TagSizedArray _, TypeRef inner_type, AST::Expr&& size);
    struct TagUnsizedArray {};
    TypeRef(TagUnsizedArray _, TypeRef inner_type) {}

    struct TagPath {};
    TypeRef(TagPath, AST::Path path) {}
    
    friend ::std::ostream& operator<<(::std::ostream& os, const TypeRef& tr) {
        os << "TypeRef(TODO)";
        return os;
    }
};

#endif // TYPES_HPP_INCLUDED
