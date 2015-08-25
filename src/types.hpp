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

struct TypeArgRef
{
    ::std::string   name;
    unsigned int    level;
    const AST::TypeParams*  params;
};

struct Type_Function:
    public Serialisable
{
    bool    is_unsafe;
    ::std::string   m_abi;
    ::std::unique_ptr<TypeRef>  m_rettype;
    ::std::vector<TypeRef>  m_arg_types;

    Type_Function() {}
    Type_Function(bool is_unsafe, ::std::string abi, ::std::unique_ptr<TypeRef> ret, ::std::vector<TypeRef> args):
        is_unsafe(is_unsafe),
        m_abi(abi),
        m_rettype(mv$(ret)),
        m_arg_types(mv$(args))
    {}
    Type_Function(const Type_Function& other);

    Ordering ord(const Type_Function& x) const;
    
    SERIALISABLE_PROTOTYPES();
};

TAGGED_ENUM(TypeData, None,
    (None, ()),
    (Any,  ()),
    (Unit, ()),
    (Primitive, (
        enum eCoreType core_type;
        )),
    (Function, (
        Type_Function   info;
        )),
    (Tuple, (
        ::std::vector<TypeRef> inner_types;
        )),
    (Borrow, (
        bool is_mut;
        ::std::unique_ptr<TypeRef> inner;
        )),
    (Pointer, (
        bool is_mut;
        ::std::unique_ptr<TypeRef> inner;
        )),
    (Array, (
        ::std::unique_ptr<TypeRef> inner;
        ::std::shared_ptr<AST::ExprNode> size;
        )),
    (Generic, (
        ::std::string name;
        unsigned int level;
        const AST::TypeParams* params;
        )),
    (Path, (
        AST::Path path;
        )),
    (TraitObject, (
        ::std::vector<AST::Path> traits;
        ))
    );

/// A type
class TypeRef:
    public Serialisable
{
    /// A generic pointer, used for tagging with extra information
    /// e.g. The source TypeParams for GENERIC
    const void* m_tagged_ptr;
public:
    TypeData    m_data;
    
    TypeRef(TypeRef&& other) noexcept:
        m_data( mv$(other.m_data) )
    {}
    
    TypeRef(const TypeRef& other)
    {
        switch( other.m_data.tag() )
        {
        #define _COPY(VAR)  case TypeData::VAR: m_data = TypeData::make_##VAR(other.m_data.as_##VAR()); break;
        #define _CLONE(VAR, code...)    case TypeData::VAR: { auto& old = other.m_data.as_##VAR(); m_data = TypeData::make_##VAR(code); } break;
        _COPY(None)
        _COPY(Any)
        _COPY(Unit)
        _COPY(Primitive)
        _COPY(Function)
        _COPY(Tuple)
        _CLONE(Borrow,  { old.is_mut, box$(TypeRef(*old.inner)) })
        _CLONE(Pointer, { old.is_mut, box$(TypeRef(*old.inner)) })
        _CLONE(Array, { box$(TypeRef(*old.inner)), old.size })
        _COPY(Generic)
        _COPY(Path)
        _COPY(TraitObject)
        #undef _COPY
        #undef _CLONE
        }
    }
    TypeRef& operator=(const TypeRef& other) {
        m_data = TypeRef(other).m_data;
        return *this;
    }
    
    TypeRef():
        m_data(TypeData::make_Any({}))
    {}
    
    struct TagInvalid {};
    TypeRef(TagInvalid):
        m_data(TypeData::make_None({}))
    {}

    struct TagUnit {};  // unit maps to a zero-length tuple, just easier to type
    TypeRef(TagUnit):
        m_data(TypeData::make_Unit({}))
    {}

    struct TagPrimitive {};
    TypeRef(TagPrimitive, enum eCoreType type):
        m_data(TypeData::make_Primitive({type}))
    {}
    TypeRef(enum eCoreType type):
        m_data(TypeData::make_Primitive({type}))
    {}

    struct TagTuple {};
    TypeRef(TagTuple _, ::std::vector<TypeRef> inner_types):
        m_data(TypeData::make_Tuple({::std::move(inner_types)}))
    {}
    struct TagFunction {};
    TypeRef(TagFunction, ::std::string abi, ::std::vector<TypeRef> args, TypeRef ret):
        m_data(TypeData::make_Function({ Type_Function( false, abi, box$(ret), mv$(args) ) }))
    {}
    
    struct TagReference {};
    TypeRef(TagReference _, bool is_mut, TypeRef inner_type):
        m_data(TypeData::make_Borrow({ is_mut, ::make_unique_ptr(mv$(inner_type)) }))
    {}
    struct TagPointer {};
    TypeRef(TagPointer _, bool is_mut, TypeRef inner_type):
        m_data(TypeData::make_Pointer({ is_mut, ::make_unique_ptr(mv$(inner_type)) }))
    {}
    struct TagSizedArray {};
    TypeRef(TagSizedArray _, TypeRef inner_type, ::std::shared_ptr<AST::ExprNode> size):
        m_data(TypeData::make_Array({ ::make_unique_ptr(mv$(inner_type)), mv$(size) }))
    {}
    struct TagUnsizedArray {};
    TypeRef(TagUnsizedArray _, TypeRef inner_type):
        m_data(TypeData::make_Array({ ::make_unique_ptr(mv$(inner_type)), ::std::shared_ptr<AST::ExprNode>() }))
    {}

    struct TagArg {};
    TypeRef(TagArg, ::std::string name):
        m_data(TypeData::make_Generic({ name, 0, nullptr }))
    {}
    TypeRef(TagArg, ::std::string name, const AST::TypeParams& params):
        m_data(TypeData::make_Generic({ name, 0, &params }))
    {}
    TypeRef(::std::string name):
        TypeRef(TagArg(), ::std::move(name))
    {}

    struct TagPath {};
    TypeRef(TagPath, AST::Path path):
        m_data(TypeData::make_Path({ ::std::move(path) }))
    {}
    TypeRef(AST::Path path):
        TypeRef(TagPath(), ::std::move(path))
    {}
   
    TypeRef( ::std::vector<AST::Path> traits ):
        m_data(TypeData::make_TraitObject({ ::std::move(traits) }))
    {}
    
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

    bool is_unbounded() const { return m_data.is_Any(); }
    bool is_wildcard() const { return m_data.is_Any(); }
    
    bool is_unit() const { return m_data.is_Unit(); }
    bool is_primitive() const { return m_data.is_Primitive(); }
    
    bool is_path() const { return m_data.is_Path(); }
    const AST::Path& path() const { return m_data.as_Path().path; }
    AST::Path& path() { return m_data.as_Path().path; }
    
    bool is_type_param() const { return m_data.is_Generic(); }
    const ::std::string& type_param() const { return m_data.as_Generic().name; }
    void set_type_params_ptr(const AST::TypeParams& p) { assert(is_type_param()); m_tagged_ptr = &p; };
    const AST::TypeParams* type_params_ptr() const {
        assert(is_type_param());
        return reinterpret_cast<const AST::TypeParams*>(m_tagged_ptr);
    }
    
    bool is_reference() const { return m_data.is_Borrow(); }
    bool is_pointer() const { return m_data.is_Pointer(); }
    bool is_tuple() const { return m_data.is_Tuple(); }
    
    //::option<const TypeData::Tuple&> as_tuple() const {
    //    switch(m_data.tag())
    //    {
    //    }
    //}

    const TypeRef& inner_type() const {
        switch(m_data.tag())
        {
        case TypeData::Borrow:  return *m_data.as_Borrow().inner;
        case TypeData::Pointer: return *m_data.as_Pointer().inner;
        case TypeData::Array:   return *m_data.as_Array().inner;
        default:    throw ::std::runtime_error("Called inner_type on non-wrapper");
        }
    }
    TypeRef& inner_type() {
        switch(m_data.tag())
        {
        case TypeData::Borrow:  return *m_data.as_Borrow().inner;
        case TypeData::Pointer: return *m_data.as_Pointer().inner;
        case TypeData::Array:   return *m_data.as_Array().inner;
        default:    throw ::std::runtime_error("Called inner_type on non-wrapper");
        }
    }
    //::std::vector<TypeRef>& sub_types() { return m_inner_types; }
    //const ::std::vector<TypeRef>& sub_types() const { return m_inner_types; }
    
    //void add_trait(TypeRef trait) { assert(is_wildcard()); m_inner_types.push_back( ::std::move(trait) ); }
    //const ::std::vector<TypeRef>& traits() const { assert(is_wildcard()); return m_inner_types; }   

    /// Returns 0 if types are identical, 1 if TypeRef::TagArg is present in one, and -1 if form differs
    int equal_no_generic(const TypeRef& x) const;
    
    Ordering ord(const TypeRef& x) const;
    bool operator==(const TypeRef& x) const { return ord(x) == OrdEqual; }
    bool operator!=(const TypeRef& x) const { return ord(x) != OrdEqual; }
    bool operator<(const TypeRef& x) const { return ord(x) == OrdLess; };
    
    PrettyPrintType print_pretty() const { return PrettyPrintType(*this); }
    
    friend class PrettyPrintType;
    
    friend ::std::ostream& operator<<(::std::ostream& os, const TypeRef& tr);
   
    static ::std::unique_ptr<TypeRef> from_deserialiser(Deserialiser& s);
    SERIALISABLE_PROTOTYPES(); 
};

#endif // TYPES_HPP_INCLUDED
