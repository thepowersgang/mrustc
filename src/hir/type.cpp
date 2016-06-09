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
        ),
    (Closure,
        os << "closure["<<e.node<<"](";
        for(const auto& t : e.m_arg_types)
            os << t << ", ";
        os << ") -> " << *e.m_rettype;
        )
    )
}

namespace {
    bool path_params_equal(const ::HIR::PathParams& t, const ::HIR::PathParams& x)
    {
        if( t.m_types.size() != x.m_types.size() )
            return false;
        for( unsigned int i = 0; i < t.m_types.size(); i ++ )
            if( !(t.m_types[i] == x.m_types[i]) )
                return false;
        return true;
    }
}

bool ::HIR::TypeRef::operator==(const ::HIR::TypeRef& x) const
{
    if( m_data.tag() != x.m_data.tag() )
        return false;
    
    TU_MATCH(::HIR::TypeRef::Data, (m_data, x.m_data), (te, xe),
    (Infer,
        // TODO: Should comparing inferrence vars be an error?
        return te.index == xe.index;
        ),
    (Diverge,
        return true;
        ),
    (Primitive,
        return te == xe;
        ),
    (Path,
        if( te.path.m_data.tag() != xe.path.m_data.tag() ) {
            return false;
        }
        TU_MATCH(::HIR::Path::Data, (te.path.m_data, xe.path.m_data), (tpe, xpe),
        (Generic,
            if( tpe.m_path != xpe.m_path )
                return false;
            return path_params_equal(tpe.m_params, xpe.m_params);
            ),
        (UfcsInherent,
            if( *tpe.type != *xpe.type )
                return false;
            if( tpe.item != xpe.item )
                return false;
            return path_params_equal(tpe.params, xpe.params);
            ),
        (UfcsKnown,
            if( *tpe.type != *xpe.type )
                return false;
            if( tpe.trait.m_path != xpe.trait.m_path )
                return false;
            if( !path_params_equal(tpe.trait.m_params, xpe.trait.m_params) )
                return false;
            if( tpe.item != xpe.item )
                return false;
            return path_params_equal(tpe.params, xpe.params);
            ),
        (UfcsUnknown,
            if( *tpe.type != *xpe.type )
                return false;
            if( tpe.item != xpe.item )
                return false;
            return path_params_equal(tpe.params, xpe.params);
            )
        )
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
        ),
    (Closure,
        if( te.node != xe.node )
            return false;
        assert( te.m_rettype == xe.m_rettype );
        return true;
        )
    )
    throw "";
}


typedef ::std::function<void(unsigned int, const ::HIR::TypeRef&)> t_cb_match_generics;

namespace {
    bool match_generics_pp(const Span& sp, const ::HIR::PathParams& t, const ::HIR::PathParams& x, ::HIR::t_cb_resolve_type resolve_placeholder, t_cb_match_generics callback)
    {
        if( t.m_types.size() != x.m_types.size() ) {
            return false;
        }
        
        for(unsigned int i = 0; i < t.m_types.size(); i ++ )
        {
            t.m_types[i].match_generics( sp, x.m_types[i], resolve_placeholder, callback );
        }
        
        return true;
    }
}

void ::HIR::TypeRef::match_generics(const Span& sp, const ::HIR::TypeRef& x_in, t_cb_resolve_type resolve_placeholder, t_cb_match_generics callback) const
{
    if( m_data.is_Infer() ) {
        BUG(sp, "Encountered '_' as this - " << *this);
    }
    if( m_data.is_Generic() ) {
        callback(m_data.as_Generic().binding, x_in);
        return ;
    }
    const auto& x = (x_in.m_data.is_Infer() || x_in.m_data.is_Generic() ? resolve_placeholder(x_in) : x_in);
    if( m_data.tag() != x.m_data.tag() ) {
        BUG(sp, "TypeRef::match_generics with mismatched forms - " << *this << " and " << x);
    }
    TU_MATCH(::HIR::TypeRef::Data, (m_data, x.m_data), (te, xe),
    (Infer, throw "";),
    (Generic, throw "";),
    (Primitive,
        ),
    (Diverge,
        ),
    (Path,
        if( te.path.m_data.tag() != xe.path.m_data.tag() ) {
            BUG(sp, "TypeRef::match_generics with mismatched forms - " << *this << " and " << x);
        }
        TU_MATCH(::HIR::Path::Data, (te.path.m_data, xe.path.m_data), (tpe, xpe),
        (Generic,
            if( tpe.m_path != xpe.m_path ) {
                BUG(sp, "TypeRef::match_generics with mismatched forms - " << *this << " and " << x);
            }
            if( !match_generics_pp(sp, tpe.m_params, xpe.m_params, resolve_placeholder, callback) ) {
                BUG(sp, "TypeRef::match_generics with mismatched forms - " << *this << " and " << x);
            }
            ),
        (UfcsKnown,
            TODO(sp, "Path UfcsKnown - " << *this << " and " << x);
            ),
        (UfcsUnknown,
            TODO(sp, "Path UfcsUnknown - " << *this << " and " << x);
            ),
        (UfcsInherent,
            TODO(sp, "Path UfcsInherent - " << *this << " and " << x);
            )
        )
        ),
    (TraitObject,
        TODO(sp, "TraitObject");
        ),
    (Array,
        te.inner->match_generics( sp, *xe.inner, resolve_placeholder, callback );
        ),
    (Slice,
        te.inner->match_generics( sp, *xe.inner, resolve_placeholder, callback );
        ),
    (Tuple,
        if( te.size() != xe.size() ) {
            BUG(sp, "TypeRef::match_generics with mismatched forms - " << *this << " and " << x);
        }
        for(unsigned int i = 0; i < te.size(); i ++ )
            te[i].match_generics( sp, xe[i], resolve_placeholder, callback );
        ),
    (Pointer,
        te.inner->match_generics( sp, *xe.inner, resolve_placeholder, callback );
        ),
    (Borrow,
        te.inner->match_generics( sp, *xe.inner, resolve_placeholder, callback );
        ),
    (Function,
        TODO(sp, "Function");
        ),
    (Closure,
        TODO(sp, "Closure");
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
        assert(!"Fell off end of clone_binding");
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
        ),
    (Closure,
        Data::Data_Closure  oe;
        oe.node = e.node;
        oe.m_rettype = box$( e.m_rettype->clone() );
        for(const auto& a : e.m_arg_types)
            oe.m_arg_types.push_back( a.clone() );
        return ::HIR::TypeRef(Data::make_Closure( mv$(oe) ));
        )
    )
    throw "";
}
::HIR::Compare HIR::TypeRef::compare_with_paceholders(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder) const
{
    TRACE_FUNCTION_F(*this << " ?= " << x);
    assert( !this->m_data.is_Infer() );
    const auto& right = (x.m_data.is_Infer() ? resolve_placeholder(x) : (x.m_data.is_Generic() ? resolve_placeholder(x) : x));
    
    // If righthand side is infer, it's a fuzzy match (or not a match)
    TU_IFLET(::HIR::TypeRef::Data, right.m_data, Infer, e,
        switch( e.ty_class )
        {
        case ::HIR::InferClass::None:
            return Compare::Fuzzy;
        case ::HIR::InferClass::Integer:
            TU_IFLET( ::HIR::TypeRef::Data, this->m_data, Primitive, le,
                switch(le)
                {
                case ::HIR::CoreType::I8:    case ::HIR::CoreType::U8:
                case ::HIR::CoreType::I16:   case ::HIR::CoreType::U16:
                case ::HIR::CoreType::I32:   case ::HIR::CoreType::U32:
                case ::HIR::CoreType::I64:   case ::HIR::CoreType::U64:
                case ::HIR::CoreType::Isize: case ::HIR::CoreType::Usize:
                    return Compare::Fuzzy;
                default:
                    return Compare::Unequal;
                }
            )
            else {
                return Compare::Unequal;
            }
        case ::HIR::InferClass::Float:
            TU_IFLET( ::HIR::TypeRef::Data, this->m_data, Primitive, le,
                switch(le)
                {
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    return Compare::Fuzzy;
                default:
                    return Compare::Unequal;
                }
            )
            else {
                return Compare::Unequal;
            }
        }
        throw "";
    )
    
    // If righthand is a type parameter, it can only match another type parameter
    // - See `(Generic,` below
    
    if( this->m_data.tag() != right.m_data.tag() ) {
        return Compare::Unequal;
    }
    TU_MATCH(::HIR::TypeRef::Data, (this->m_data, right.m_data), (le, re),
    (Infer, assert(!"infer");),
    (Diverge,
        return Compare::Equal;
        ),
    (Primitive,
        return (le == re ? Compare::Equal : Compare::Unequal);
        ),
    (Path,
        return le.path.compare_with_paceholders( sp, re.path, resolve_placeholder );
        ),
    (Generic,
        if( le.binding != re.binding )
            return Compare::Unequal;
        return Compare::Equal;
        ),
    (TraitObject,
        TODO(sp, "Compare " << *this << " and " << right);
        ),
    (Array,
        if( le.size_val != re.size_val )
            return Compare::Unequal;
        return le.inner->compare_with_paceholders(sp, *re.inner, resolve_placeholder);
        ),
    (Slice,
        return le.inner->compare_with_paceholders(sp, *re.inner, resolve_placeholder);
        ),
    (Tuple,
        if( le.size() != re.size() )
            return Compare::Unequal;
        auto rv = Compare::Equal;
        for( unsigned int i = 0; i < le.size(); i ++ )
        {
            auto rv2 = le[i].compare_with_paceholders( sp, re[i], resolve_placeholder );
            if( rv2 == Compare::Unequal )
                return Compare::Unequal;
            if( rv2 == Compare::Fuzzy )
                rv = Compare::Fuzzy;
        }
        return rv;
        ),
    (Borrow,
        if( le.type != re.type )
            return Compare::Unequal;
        return le.inner->compare_with_paceholders(sp, *re.inner, resolve_placeholder);
        ),
    (Pointer,
        if( le.type != re.type )
            return Compare::Unequal;
        return le.inner->compare_with_paceholders(sp, *re.inner, resolve_placeholder);
        ),
    (Function,
        TODO(sp, "Compare " << *this << " and " << right);
        ),
    (Closure,
        TODO(sp, "Compare " << *this << " and " << right);
        )
    )
    throw "";
}
