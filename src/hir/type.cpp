/*
 */
#include "type.hpp"
#include <span.hpp>

namespace HIR {

    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::TypeRef& ty)
    {
        ty.fmt(os);
        return os;
    }

    ::std::ostream& operator<<(::std::ostream& os, const CoreType& ct)
    {
        switch(ct)
        {
        case CoreType::Usize:   return os << "usize";
        case CoreType::Isize:   return os << "isize";
        case CoreType::U8:  return os << "u8";
        case CoreType::I8:  return os << "i8";
        case CoreType::U16: return os << "u16";
        case CoreType::I16: return os << "i16";
        case CoreType::U32: return os << "u32";
        case CoreType::I32: return os << "i32";
        case CoreType::U64: return os << "u64";
        case CoreType::I64: return os << "i64";
        
        case CoreType::F32: return os << "f32";
        case CoreType::F64: return os << "f64";
        
        case CoreType::Bool:    return os << "bool";
        case CoreType::Char:    return os << "char";
        case CoreType::Str:     return os << "str";
        }
        return os;
    }
}

void ::HIR::TypeRef::fmt(::std::ostream& os) const
{
    TU_MATCH(::HIR::TypeRef::Data, (m_data), (e),
    (Infer,
        os << "_";
        if( e.index != ~0u )  os << "/*" << e.index << "*/";
        ),
    (Diverge,
        os << "!";
        ),
    (Primitive,
        os << e;
        ),
    (Path,
        os << e.path;
        ),
    (Generic,
        os << e.name << "/*#" << e.binding << "*/";
        ),
    (TraitObject,
        os << "(";
        os << e.m_traits;
        if( e.m_lifetime.name != "" )
            os << "+ '" << e.m_lifetime.name;
        os << ")";
        ),
    (Array,
        os << "[" << *e.inner << "; ";
        if( e.size_val != ~0u )
            os << e.size_val;
        else
            os << "/*sz*/";
        os << "]";
        ),
    (Slice,
        os << "[" << *e.inner << "]";
        ),
    (Tuple,
        os << "(";
        for(const auto& t : e)
            os << t << ", ";
        os << ")";
        ),
    (Borrow,
        switch(e.type)
        {
        case ::HIR::BorrowType::Shared: os << "&";  break;
        case ::HIR::BorrowType::Unique: os << "&mut ";  break;
        case ::HIR::BorrowType::Owned:  os << "&move "; break;
        }
        os << *e.inner;
        ),
    (Pointer,
        if( e.is_mut ) {
            os << "*mut ";
        }
        else {
            os << "*const ";
        }
        os << *e.inner;
        ),
    (Function,
        if( e.is_unsafe ) {
            os << "unsafe ";
        }
        if( e.m_abi != "" ) {
            os << "extern \"" << e.m_abi << "\" ";
        }
        os << "fn(";
        for(const auto& t : e.m_arg_types)
            os << t << ", ";
        os << ") -> " << *e.m_rettype;
        )
    )
}

namespace {
    ::HIR::TypeRef::TypePathBinding clone_binding(const ::HIR::TypeRef::TypePathBinding& x) {
        TU_MATCH(::HIR::TypeRef::TypePathBinding, (x), (e),
        (Unbound, return ::HIR::TypeRef::TypePathBinding::make_Unbound({}); ),
        (Opaque , return ::HIR::TypeRef::TypePathBinding::make_Opaque({}); ),
        (Struct , return ::HIR::TypeRef::TypePathBinding(e); ),
        (Enum   , return ::HIR::TypeRef::TypePathBinding(e); )
        )
        throw "";
    }
}

::HIR::TypeRef HIR::TypeRef::clone() const
{
    TU_MATCH(::HIR::TypeRef::Data, (m_data), (e),
    (Infer,
        return ::HIR::TypeRef( Data::make_Infer(e) );
        ),
    (Diverge,
        return ::HIR::TypeRef( Data::make_Diverge({}) );
        ),
    (Primitive,
        return ::HIR::TypeRef( Data::make_Primitive(e) );
        ),
    (Path,
        return ::HIR::TypeRef( Data::make_Path({
            e.path.clone(),
            clone_binding(e.binding)
            }) );
        ),
    (Generic,
        return ::HIR::TypeRef( Data::make_Generic(e) );
        ),
    (TraitObject,
        ::std::vector< ::HIR::GenericPath>  traits;
        for(const auto& trait : e.m_traits)
            traits.push_back( trait.clone() );
        return ::HIR::TypeRef( Data::make_TraitObject({
            mv$(traits),
            e.m_lifetime
            }) );
        ),
    (Array,
        if( e.size_val == ~0u ) {
            BUG(Span(), "Attempting to clone array with unknown size - " << *this);
        }
        return ::HIR::TypeRef( Data::make_Array({
            box$( e.inner->clone() ),
            ::HIR::ExprPtr(),
            e.size_val
            }) );
        ),
    (Slice,
        return ::HIR::TypeRef( Data::make_Slice({
            box$( e.inner->clone() )
            }) );
        ),
    (Tuple,
        ::std::vector< ::HIR::TypeRef>  types;
        for(const auto& t : e)
            types.push_back( t.clone() );
        return ::HIR::TypeRef( Data::make_Tuple(mv$(types)) );
        ),
    (Borrow,
        return ::HIR::TypeRef( Data::make_Borrow({e.type, box$(e.inner->clone())}) );
        ),
    (Pointer,
        return ::HIR::TypeRef( Data::make_Pointer({e.is_mut, box$(e.inner->clone())}) );
        ),
    (Function,
        FunctionType    ft;
        ft.is_unsafe = e.is_unsafe;
        ft.m_abi = e.m_abi;
        ft.m_rettype = box$( e.m_rettype->clone() );
        for(const auto& a : e.m_arg_types)
            ft.m_arg_types.push_back( a.clone() );
        return ::HIR::TypeRef(Data::make_Function( mv$(ft) ));
        )
    )
    throw "";
}
