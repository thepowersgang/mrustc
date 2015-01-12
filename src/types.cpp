/*
 */
#include "types.hpp"
#include "ast/ast.hpp"

::std::ostream& operator<<(::std::ostream& os, const TypeRef& tr) {
    os << "TypeRef(TODO)";
    return os;
}

SERIALISE_TYPE(TypeRef::, "TypeRef", {
    // TODO: TypeRef serialise
})
