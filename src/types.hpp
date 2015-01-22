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

/// Representation of restrictions placed on a type before it is made concrete
// Possible bounds:
// - Known to be a tuple of various inner types
// - Unknown struct / enum
// - Impls a trait
// - Unknown
class TypeBounds
{
    
};

/// A type
class TypeRef:
    public Serialisable
{
    /// Class
    enum Class {
        ANY,    //< '_' - Wildcard
        UNIT,   //< '()' - Unit / void
        PRIMITIVE,  //< Any primitive (builtin type)
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
    struct TagBoundedAny {};
    TypeRef(TagBoundedAny, ::std::vector<TypeRef> traits):
        m_class(ANY),
        m_inner_types( ::std::move(traits) )
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
   
    /// Merge with another type (combines known aspects, conflitcs cause an exception)
    void merge_with(const TypeRef& other);
    /// Replace 'GENERIC' entries with the return value of the closure
    void resolve_args(::std::function<TypeRef(const char*)> fcn);
    /// Match 'GENERIC' entries with another type, passing matches to a closure
    void match_args(const TypeRef& other, ::std::function<void(const char*,const TypeRef&)> fcn) const;
    
    /// Returns true if the type is fully known (all sub-types are not wildcards)
    bool is_concrete() const;

    bool is_wildcard() const { return m_class == ANY; }
    bool is_unit() const { return m_class == UNIT; }
    bool is_path() const { return m_class == PATH; }
    bool is_type_param() const { return m_class == GENERIC; }
    bool is_reference() const { return m_class == REFERENCE; }
    const ::std::string& type_param() const { assert(is_type_param()); return m_path[0].name(); }
    AST::Path& path() { assert(is_path() || m_class == ASSOCIATED); return m_path; }
    const AST::Path& path() const { assert(is_path() || m_class == ASSOCIATED); return m_path; }
    ::std::vector<TypeRef>& sub_types() { return m_inner_types; }
    const ::std::vector<TypeRef>& sub_types() const { return m_inner_types; }
    
    void add_trait(TypeRef trait) { assert(is_wildcard()); m_inner_types.push_back( ::std::move(trait) ); }
    const ::std::vector<TypeRef>& traits() const { assert(is_wildcard()); return m_inner_types; }   

    bool operator==(const TypeRef& x) const;
    bool operator!=(const TypeRef& x) const { return !(*this == x); }
     
    friend ::std::ostream& operator<<(::std::ostream& os, const TypeRef& tr);
    
    static const char* class_name(TypeRef::Class c);
    friend void operator>>(::Deserialiser& d, TypeRef::Class& c);
   
    SERIALISABLE_PROTOTYPES(); 
};

#endif // TYPES_HPP_INCLUDED
