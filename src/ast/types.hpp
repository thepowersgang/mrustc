#ifndef TYPES_HPP_INCLUDED
#define TYPES_HPP_INCLUDED

#include <memory>

#include "common.hpp"
#include "coretypes.hpp"
#include "ast/path.hpp"
#include "ast/macro.hpp"
#include <serialise.hpp>
#include <tagged_union.hpp>

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
    const AST::GenericParams*  params;
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
        m_abi(mv$(abi)),
        m_rettype(mv$(ret)),
        m_arg_types(mv$(args))
    {}
    Type_Function(Type_Function&& other) = default;
    Type_Function(const Type_Function& other);

    Ordering ord(const Type_Function& x) const;
    
    SERIALISABLE_PROTOTYPES();
};

TAGGED_UNION(TypeData, None,
    (None, struct { }),
    (Any,  struct { }),
    (Unit, struct { }),
    (Macro, struct {
        ::AST::MacroInvocation inv;
        }),
    (Primitive, struct {
        enum eCoreType core_type;
        }),
    (Function, struct {
        Type_Function   info;
        }),
    (Tuple, struct {
        ::std::vector<TypeRef> inner_types;
        }),
    (Borrow, struct {
        bool is_mut;
        ::std::unique_ptr<TypeRef> inner;
        }),
    (Pointer, struct {
        bool is_mut;
        ::std::unique_ptr<TypeRef> inner;
        }),
    (Array, struct {
        ::std::unique_ptr<TypeRef> inner;
        ::std::shared_ptr<AST::ExprNode> size;
        }),
    (Generic, struct {
        ::std::string name;
        unsigned int level;
        const ::AST::GenericParams* params;
        }),
    (Path, struct {
        AST::Path path;
        }),
    (TraitObject, struct {
        ::std::vector<::std::string>    hrls;
        ::std::vector<AST::Path> traits;
        })
    );

/// A type
class TypeRef:
    public Serialisable
{
    Span    m_span;
public:
    TypeData    m_data;
    
    virtual ~TypeRef();
    
    TypeRef(TypeRef&& other) noexcept:
        m_data( mv$(other.m_data) )
    {
        m_span = mv$(other.m_span);
    }
    
    TypeRef(const TypeRef& other);
    TypeRef& operator=(TypeRef&& other) {
        m_data = mv$( other.m_data );
        m_span = mv$( other.m_span );
        return *this;
    }
    TypeRef& operator=(const TypeRef& other) {
        m_data = TypeRef(other).m_data;
        return *this;
    }
    
    TypeRef(Span sp=Span()):
        m_span( mv$(sp) ),
        m_data(TypeData::make_Any({}))
    {}
    
    struct TagInvalid {};
    TypeRef(TagInvalid, Span sp):
        m_span(mv$(sp)),
        m_data(TypeData::make_None({}))
    {}
    
    struct TagMacro {};
    TypeRef(TagMacro, ::AST::MacroInvocation inv):
        m_span(inv.span()),
        m_data(TypeData::make_Macro({mv$(inv)}))
    {}

    struct TagUnit {};  // unit maps to a zero-length tuple, just easier to type
    TypeRef(TagUnit, Span sp):
        m_span(mv$(sp)),
        m_data(TypeData::make_Unit({}))
    {}

    struct TagPrimitive {};
    TypeRef(TagPrimitive, Span sp, enum eCoreType type):
        m_span(mv$(sp)),
        m_data(TypeData::make_Primitive({type}))
    {}
    TypeRef(Span sp, enum eCoreType type):
        m_span(mv$(sp)),
        m_data(TypeData::make_Primitive({type}))
    {}

    struct TagTuple {};
    TypeRef(TagTuple , Span sp, ::std::vector<TypeRef> inner_types):
        m_span(mv$(sp)),
        m_data(TypeData::make_Tuple({::std::move(inner_types)}))
    {}
    struct TagFunction {};
    TypeRef(TagFunction, Span sp, ::std::string abi, ::std::vector<TypeRef> args, TypeRef ret):
        m_span(mv$(sp)),
        m_data(TypeData::make_Function({ Type_Function( false, abi, box$(ret), mv$(args) ) }))
    {}
    
    struct TagReference {};
    TypeRef(TagReference , Span sp, bool is_mut, TypeRef inner_type):
        m_span(mv$(sp)),
        m_data(TypeData::make_Borrow({ is_mut, ::make_unique_ptr(mv$(inner_type)) }))
    {}
    struct TagPointer {};
    TypeRef(TagPointer , Span sp, bool is_mut, TypeRef inner_type):
        m_span(mv$(sp)),
        m_data(TypeData::make_Pointer({ is_mut, ::make_unique_ptr(mv$(inner_type)) }))
    {}
    struct TagSizedArray {};
    TypeRef(TagSizedArray , Span sp, TypeRef inner_type, ::std::shared_ptr<AST::ExprNode> size):
        m_span(mv$(sp)),
        m_data(TypeData::make_Array({ ::make_unique_ptr(mv$(inner_type)), mv$(size) }))
    {}
    struct TagUnsizedArray {};
    TypeRef(TagUnsizedArray , Span sp, TypeRef inner_type):
        m_span(mv$(sp)),
        m_data(TypeData::make_Array({ ::make_unique_ptr(mv$(inner_type)), ::std::shared_ptr<AST::ExprNode>() }))
    {}

    struct TagArg {};
    TypeRef(TagArg, ::std::string name):
        m_data(TypeData::make_Generic({ name, 0, nullptr }))
    {}
    TypeRef(TagArg, ::std::string name, const AST::GenericParams& params):
        m_data(TypeData::make_Generic({ name, 0, &params }))
    {}
    TypeRef(::std::string name):
        TypeRef(TagArg(), ::std::move(name))
    {}

    struct TagPath {};
    TypeRef(TagPath, Span sp, AST::Path path):
        m_span(mv$(sp)),
        m_data(TypeData::make_Path({ ::std::move(path) }))
    {}
    TypeRef(Span sp, AST::Path path):
        TypeRef(TagPath(), mv$(sp), mv$(path))
    {}
   
    TypeRef( Span sp, ::std::vector<::std::string> hrls, ::std::vector<AST::Path> traits ):
        m_span(mv$(sp)),
        m_data(TypeData::make_TraitObject({ mv$(hrls), ::std::move(traits) }))
    {}
    

    const Span& span() const { return m_span; }
 
    bool is_valid() const { return ! m_data.is_None(); }

    bool is_unbounded() const { return m_data.is_Any(); }
    bool is_wildcard() const { return m_data.is_Any(); }
    
    bool is_unit() const { return m_data.is_Unit(); }
    bool is_primitive() const { return m_data.is_Primitive(); }
    
    bool is_path() const { return m_data.is_Path(); }
    const AST::Path& path() const { return m_data.as_Path().path; }
    AST::Path& path() { return m_data.as_Path().path; }
    
    bool is_type_param() const { return m_data.is_Generic(); }
    const ::std::string& type_param() const { return m_data.as_Generic().name; }
    void set_type_params_ptr(const AST::GenericParams& p) { m_data.as_Generic().params = &p; };
    const AST::GenericParams* type_params_ptr() const {
        return reinterpret_cast<const AST::GenericParams*>( m_data.as_Generic().params );
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
        TU_MATCH_DEF(TypeData, (m_data), (e),
        ( throw ::std::runtime_error("Called inner_type on non-wrapper"); ),
        (Borrow,  return *e.inner; ),
        (Pointer, return *e.inner; ),
        (Array,   return *e.inner; )
        )
    }
    TypeRef& inner_type() {
        TU_MATCH_DEF(TypeData, (m_data), (e),
        ( throw ::std::runtime_error("Called inner_type on non-wrapper"); ),
        (Borrow,  return *e.inner; ),
        (Pointer, return *e.inner; ),
        (Array,   return *e.inner; )
        )
    }
    //::std::vector<TypeRef>& sub_types() { return m_inner_types; }
    //const ::std::vector<TypeRef>& sub_types() const { return m_inner_types; }
    
    //void add_trait(TypeRef trait) { assert(is_wildcard()); m_inner_types.push_back( ::std::move(trait) ); }
    //const ::std::vector<TypeRef>& traits() const { assert(is_wildcard()); return m_inner_types; }   

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
