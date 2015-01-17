#ifndef TYPES_HPP_INCLUDED
#define TYPES_HPP_INCLUDED

#include <memory>

#include "common.hpp"
#include "coretypes.hpp"
#include "ast/path.hpp"
#include <serialise.hpp>

namespace AST {
class ExprNode;
class Expr;
}

class TypeRef:
    public Serialisable
{
    enum Class {
        ANY,
        UNIT,
        PRIMITIVE,
        TUPLE,
        REFERENCE,
        POINTER,
        ARRAY,
        GENERIC,
        PATH,
        ASSOCIATED,
    };
    
    Class   m_class;
    enum eCoreType  m_core_type;
    bool    m_is_inner_mutable;
    
    AST::Path   m_path; // local = argument
    ::std::vector<TypeRef>  m_inner_types;
    ::std::shared_ptr<AST::ExprNode>    m_size_expr; //< Can be null (unsized array)
public:
    TypeRef():
        m_class(ANY)
    {}

    struct TagUnit {};  // unit maps to a zero-length tuple, just easier to type
    TypeRef(TagUnit):
        m_class(UNIT)
    {}

    struct TagPrimitive {};
    TypeRef(TagPrimitive, enum eCoreType type):
        m_class(PRIMITIVE),
        m_core_type(type)
    {}
    struct TagTuple {};
    TypeRef(TagTuple _, ::std::vector<TypeRef> inner_types):
        m_class(TUPLE),
        m_inner_types( ::std::move(inner_types) )
    {}
    struct TagReference {};
    TypeRef(TagReference _, bool is_mut, TypeRef inner_type):
        m_class(REFERENCE),
        m_is_inner_mutable(is_mut),
        m_inner_types({::std::move(inner_type)})
    {}
    struct TagPointer {};
    TypeRef(TagPointer _, bool is_mut, TypeRef inner_type):
        m_class(POINTER),
        m_is_inner_mutable(is_mut),
        m_inner_types({::std::move(inner_type)})
    {}
    struct TagSizedArray {};
    TypeRef(TagSizedArray _, TypeRef inner_type, ::std::shared_ptr<AST::ExprNode> size):
        m_class(ARRAY),
        m_inner_types({::std::move(inner_type)}),
        m_size_expr( ::std::move(size) )
    {}
    struct TagUnsizedArray {};
    TypeRef(TagUnsizedArray _, TypeRef inner_type):
        m_class(ARRAY),
        m_inner_types({::std::move(inner_type)})
    {}

    struct TagArg {};
    TypeRef(TagArg, ::std::string name):
        m_class(GENERIC),
        m_path({AST::PathNode(name, {})})
    {}
    TypeRef(::std::string name):
        TypeRef(TagArg(), ::std::move(name))
    {}

    struct TagPath {};
    TypeRef(TagPath, AST::Path path):
        m_class(PATH),
        m_path( ::std::move(path) )
    {}
    TypeRef(AST::Path path):
        TypeRef(TagPath(), ::std::move(path))
    {}
   
    struct TagAssoc {};
    TypeRef(TagAssoc, TypeRef base, TypeRef trait, ::std::string assoc_name):
        TypeRef(::std::move(base), ::std::move(trait), ::std::move(assoc_name))
    {}
    TypeRef(TypeRef base, TypeRef trait, ::std::string assoc_name):
        m_class(ASSOCIATED),
        m_path( {AST::PathNode(assoc_name, {})} ),
        m_inner_types( {::std::move(base), ::std::move(trait)} )
    {}
   
    bool is_path() const { return m_class == PATH; }
    AST::Path& path() { assert(is_path()); return m_path; }
    ::std::vector<TypeRef>& sub_types() { return m_inner_types; }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const TypeRef& tr);
    
    static const char* class_name(TypeRef::Class c);
    friend void operator>>(::Deserialiser& d, TypeRef::Class& c);
   
    SERIALISABLE_PROTOTYPES(); 
};

#endif // TYPES_HPP_INCLUDED
