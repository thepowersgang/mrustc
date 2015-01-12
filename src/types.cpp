/*
 */
#include "types.hpp"
#include "ast/ast.hpp"

template <typename T>
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::vector<T>& v) {
    if( v.size() > 0 )
    {
        bool is_first = true;
        for( const auto& i : v )
        {
            if(!is_first)
                os << ", ";
            is_first = false;
            os << i;
        }
    }
    return os;
}


::std::ostream& operator<<(::std::ostream& os, const TypeRef& tr) {
    os << "TypeRef(";
    switch(tr.m_class)
    {
    case TypeRef::ANY:
        os << "TagAny";
        break;
    case TypeRef::UNIT:
        os << "TagUnit";
        break;
    case TypeRef::PRIMITIVE:
        os << "TagPrimitive, " << tr.m_core_type;
        break;
    case TypeRef::TUPLE:
        os << "TagTuple, {" << tr.m_inner_types << "}";
        break;
    case TypeRef::REFERENCE:
        os << "TagReference, " << (tr.m_is_inner_mutable ? "mut" : "const") << ", " << tr.m_inner_types[0];
        break;
    case TypeRef::POINTER:
        os << "TagPointer, " << (tr.m_is_inner_mutable ? "mut" : "const") << ", " << tr.m_inner_types[0];
        break;
    case TypeRef::ARRAY:
        os << "TagSizedArray, " << tr.m_inner_types[0] << ", " << tr.m_size_expr;
        break;
    case TypeRef::GENERIC:
        os << "TagArg, " << tr.m_path[0].name();
        break;
    case TypeRef::PATH:
        os << "TagPath, " << tr.m_path;
        break;
    }
    os << ")";
    return os;
}

SERIALISE_TYPE(TypeRef::, "TypeRef", {
    // TODO: TypeRef serialise
})
