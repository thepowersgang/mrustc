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

class PrettyPrintType
{
    const TypeRef&  m_type;
public:
    PrettyPrintType(const TypeRef& ty):
        m_type(ty)
    {}
    
    void print(::std::ostream& os) const;
    
    friend ::std::ostream& operator<<(::std::ostream& os, const PrettyPrintType& v);
};

/// A type
class TypeRef:
    public Serialisable
{
    /// Class
    enum Class {
        NONE,
        ANY,    //< '_' - Wildcard
        //BOUNDED,    //< '_: Traits' - Bounded type (a resolved type parameter usually)
        UNIT,   //< '()' - Unit / void
        PRIMITIVE,  //< Any primitive (builtin type)
        FUNCTION,
        TUPLE,
        REFERENCE,
        POINTER,
        ARRAY,
        GENERIC,
        PATH,
        MULTIDST,   // Multi-trait DST (e.g. Trait + Send + Sync)
    };
    
    Class   m_class;
    enum eCoreType  m_core_type;
    bool    m_is_inner_mutable;
    
    AST::Path   m_path; // local = argument
    ::std::vector<TypeRef>  m_inner_types;
    ::std::shared_ptr<AST::ExprNode>    m_size_expr; //< Can be null (unsized array)
    
    /// A generic pointer, used for tagging with extra information
    /// e.g. The source TypeParams for GENERIC
    const void* m_tagged_ptr;
public:
    TypeRef():
        m_class(ANY)
    {}
    
    struct TagInvalid {};
    TypeRef(TagInvalid):
        m_class(NONE)
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
    TypeRef(enum eCoreType type):
        m_class(PRIMITIVE),
        m_core_type(type)
    {}

    struct TagTuple {};
    TypeRef(TagTuple _, ::std::vector<TypeRef> inner_types):
        m_class(TUPLE),
        m_inner_types( ::std::move(inner_types) )
    {}
    struct TagFunction {};
    TypeRef(TagFunction, ::std::string abi, ::std::vector<TypeRef> args, TypeRef ret):
        m_class(FUNCTION),
        m_path( {AST::PathNode( ::std::move(abi), {})} ), // abuse path for string
        m_inner_types( ::std::move(args) )
    {
        m_inner_types.push_back( ::std::move(ret) );
    }
    
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
        m_path( ::std::move(name) ),
        m_tagged_ptr(nullptr)
    {}
    TypeRef(TagArg, ::std::string name, const AST::TypeParams& params):
        m_class(GENERIC),
        m_path( ::std::move(name) ),
        m_tagged_ptr(&params)
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
   
    TypeRef( ::std::vector<AST::Path> traits ):
        m_class(MULTIDST)
    {
        for( auto& t : traits )
            m_inner_types.push_back( TypeRef(::std::move(t)) );
    }
    
    /// Dereference the type (return the result of *type_instance)
    bool deref(bool is_implicit);
    /// Merge with another type (combines known aspects, conflitcs cause an exception)
    void merge_with(const TypeRef& other);
    /// Replace 'GENERIC' entries with the return value of the closure
    void resolve_args(::std::function<TypeRef(const char*)> fcn);
    /// Match 'GENERIC' entries with another type, passing matches to a closure
    void match_args(const TypeRef& other, ::std::function<void(const char*,const TypeRef&)> fcn) const;
    
    bool impls_wildcard(const AST::Crate& crate, const AST::Path& trait) const;
    
    /// Returns true if the type is fully known (all sub-types are not wildcards)
    bool is_concrete() const;

    bool is_unbounded() const { return m_class == ANY && m_inner_types.size() == 0; }
    bool is_wildcard() const { return m_class == ANY; }
    
    bool is_unit() const { return m_class == UNIT; }
    bool is_primitive() const { return m_class == PRIMITIVE; }
    
    bool is_path() const { return m_class == PATH; }
    const AST::Path& path() const { assert(is_path()); return m_path; }
    AST::Path& path() { assert(is_path()); return m_path; }
    
    bool is_type_param() const { return m_class == GENERIC; }
    const ::std::string& type_param() const { assert(is_type_param()); return m_path[0].name(); }
    void set_type_params_ptr(const AST::TypeParams& p) { assert(is_type_param()); m_tagged_ptr = &p; };
    const AST::TypeParams* type_params_ptr() const {
        assert(is_type_param());
        return reinterpret_cast<const AST::TypeParams*>(m_tagged_ptr);
    }
    
    bool is_reference() const { return m_class == REFERENCE; }
    bool is_pointer() const { return m_class == POINTER; }
    bool is_tuple() const { return m_class == TUPLE; }

    ::std::vector<TypeRef>& sub_types() { return m_inner_types; }
    const ::std::vector<TypeRef>& sub_types() const { return m_inner_types; }
    
    void add_trait(TypeRef trait) { assert(is_wildcard()); m_inner_types.push_back( ::std::move(trait) ); }
    const ::std::vector<TypeRef>& traits() const { assert(is_wildcard()); return m_inner_types; }   

    /// Returns 0 if types are identical, 1 if TypeRef::TagArg is present in one, and -1 if form differs
    int equal_no_generic(const TypeRef& x) const;
    
    Ordering ord(const TypeRef& x) const;
    bool operator==(const TypeRef& x) const { return ord(x) == OrdEqual; }
    bool operator!=(const TypeRef& x) const { return ord(x) != OrdEqual; }
    bool operator<(const TypeRef& x) const { return ord(x) == OrdLess; };
    
    PrettyPrintType print_pretty() const { return PrettyPrintType(*this); }
    
    friend class PrettyPrintType;
    
    friend ::std::ostream& operator<<(::std::ostream& os, const TypeRef& tr);
    
    static const char* class_name(TypeRef::Class c);
    friend void operator>>(::Deserialiser& d, TypeRef::Class& c);
   
    SERIALISABLE_PROTOTYPES(); 
};

class Type_Function:
    public Serialisable
{
    bool    is_unsafe;
    ::std::string   m_abi;
    TypeRef m_rettype;
    ::std::vector<TypeRef>  m_arg_types;
};

#endif // TYPES_HPP_INCLUDED
