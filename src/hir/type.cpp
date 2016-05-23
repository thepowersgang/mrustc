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
        ),
    (Diverge,
        os << "!";
        ),
    (Primitive,
        os << e;
        ),
    (Path,
        os << e;
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
        os << "[" << *e.inner << "; " << "/*sz*/" << "]";
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
::HIR::TypeRef HIR::TypeRef::clone() const
{
    TU_MATCH(::HIR::TypeRef::Data, (m_data), (e),
    (Infer,
        return ::HIR::TypeRef( Data::make_Infer({}) );
        ),
    (Diverge,
        return ::HIR::TypeRef( Data::make_Diverge({}) );
        ),
    (Primitive,
        return ::HIR::TypeRef( Data::make_Primitive(e) );
        ),
    (Path,
        return ::HIR::TypeRef( Data::make_Path(e.clone()) );
        ),
    (Generic,
        return ::HIR::TypeRef( Data::make_Generic(e) );
        ),
    (TraitObject,
        TODO(Span(), "TypeRef::clone() - this = " << *this);
        ),
    (Array,
        TODO(Span(), "TypeRef::clone() - this = " << *this);
        //return ::HIR::TypeRef( Data::make_Array({
        //    box$( e.inner->clone() ),
        //    /* huh */
        //    }) );
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
        TODO(Span(), "TypeRef::clone() - this = " << *this);
        )
    )
    throw "";
}
