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
        if( e.index != ~0u || e.ty_class != ::HIR::InferClass::None ) {
            os << "/*";
            if(e.index != ~0u)  os << e.index;
            switch(e.ty_class)
            {
            case ::HIR::InferClass::None:   break;
            case ::HIR::InferClass::Float:  os << ":f"; break;
            case ::HIR::InferClass::Integer:os << ":i"; break;
            }
            os << "*/";
        }
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
        switch(e.type)
        {
        case ::HIR::BorrowType::Shared: os << "*const ";  break;
        case ::HIR::BorrowType::Unique: os << "*mut ";  break;
        case ::HIR::BorrowType::Owned:  os << "*move "; break;
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
bool ::HIR::TypeRef::operator==(const ::HIR::TypeRef& x) const
{
    if( m_data.tag() != x.m_data.tag() )
        return false;
    
    TU_MATCH(::HIR::TypeRef::Data, (m_data, x.m_data), (te, xe),
    (Infer,
        // TODO: Should comparing inferrence vars be an error?
        return true;
        ),
    (Diverge,
        return true;
        ),
    (Primitive,
        return te == xe;
        ),
    (Path,
        assert(!"TODO: Compare path types");
        ),
    (Generic,
        return te.name == xe.name && te.binding == xe.binding;
        ),
    (TraitObject,
        assert(!"TODO: Compare trait object types");
        ),
    (Array,
        if( *te.inner != *xe.inner )
            return false;
        if( xe.size_val != te.size_val )
            return false;
        if( te.size_val == ~0u )
            assert(!"TOD: Compre array types with non-resolved sizes");
        return true;
        ),
    (Slice,
        return *te.inner == *xe.inner;
        ),
    (Tuple,
        if( te.size() != xe.size() )
            return false;
        for(unsigned int i = 0; i < te.size(); i ++ ) {
            if( te[i] != xe[i] )
                return false;
        }
        return true;
        ),
    (Borrow,
        if( te.type != xe.type )
            return false;
        return *te.inner == *xe.inner;
        ),
    (Pointer,
        if( te.type != xe.type )
            return false;
        return *te.inner == *xe.inner;
        ),
    (Function,
        if( te.is_unsafe != xe.is_unsafe )
            return false;
        if( te.m_abi != xe.m_abi )
            return false;
        if( te.m_arg_types.size() != xe.m_arg_types.size() )
            return false;
        for(unsigned int i = 0; i < te.m_arg_types.size(); i ++ ) {
            if( te.m_arg_types[i] != xe.m_arg_types[i] )
                return false;
        }
        return te.m_rettype == xe.m_rettype;
        )
    )
    throw "";
}
void ::HIR::TypeRef::match_generics(const Span& sp, const ::HIR::TypeRef& x, ::std::function<void(unsigned int, const ::HIR::TypeRef&)> callback) const
{
    if( m_data.is_Infer() ) {
        BUG(sp, "");
    }
    if( m_data.is_Generic() ) {
        callback(m_data.as_Generic().binding, x);
        return ;
    }
    if( m_data.tag() != x.m_data.tag() ) {
        BUG(sp, "");
    }
    TU_MATCH(::HIR::TypeRef::Data, (m_data, x.m_data), (te, xe),
    (Infer, throw "";),
    (Generic, throw "";),
    (Primitive,
        ),
    (Diverge,
        ),
    (Path,
        TODO(sp, "Path");
        ),
    (TraitObject,
        TODO(sp, "TraitObject");
        ),
    (Array,
        te.inner->match_generics( sp, *xe.inner, callback );
        ),
    (Slice,
        te.inner->match_generics( sp, *xe.inner, callback );
        ),
    (Tuple,
        if( te.size() != xe.size() ) {
            BUG(sp, "");
        }
        for(unsigned int i = 0; i < te.size(); i ++ )
            te[i].match_generics( sp, xe[i], callback );
        ),
    (Pointer,
        te.inner->match_generics( sp, *xe.inner, callback );
        ),
    (Borrow,
        te.inner->match_generics( sp, *xe.inner, callback );
        ),
    (Function,
        TODO(sp, "Function");
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
        return ::HIR::TypeRef( Data::make_Pointer({e.type, box$(e.inner->clone())}) );
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
