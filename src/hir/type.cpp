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
        assert(!"Bad CoreType value");
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
        TU_MATCH(::HIR::TypeRef::TypePathBinding, (e.binding), (be),
        (Unbound, os << "/*U*/";),
        (Opaque, os << "/*O*/";),
        (Struct, os << "/*S*/";),
        (Enum, os << "/*E*/";)
        )
        ),
    (Generic,
        os << e.name << "/*";
        if( e.binding == 0xFFFF )
            os << "";
        else if( e.binding < 256 )
            os << "I:" << e.binding;
        else if( e.binding < 512 )
            os << "M:" << (e.binding - 256);
        else
            os << e.binding;
        os << "*/";
        ),
    (TraitObject,
        os << "(";
        os << e.m_trait;
        for(const auto& tr : e.m_markers)
            os << "+" << tr;
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
            return tpe == xpe;
            ),
        (UfcsInherent,
            if( *tpe.type != *xpe.type )
                return false;
            if( tpe.item != xpe.item )
                return false;
            return tpe.params == xpe.params;
            ),
        (UfcsKnown,
            if( *tpe.type != *xpe.type )
                return false;
            if( tpe.trait != xpe.trait )
                return false;
            if( tpe.item != xpe.item )
                return false;
            return tpe.params == xpe.params;
            ),
        (UfcsUnknown,
            if( *tpe.type != *xpe.type )
                return false;
            if( tpe.item != xpe.item )
                return false;
            return tpe.params == xpe.params;
            )
        )
        ),
    (Generic,
        return te.name == xe.name && te.binding == xe.binding;
        ),
    (TraitObject,
        if( te.m_trait != xe.m_trait )
            return false;
        if( te.m_markers.size() != xe.m_markers.size() )
            return false;
        for(unsigned int i = 0; i < te.m_markers.size(); i ++ ) {
            if( te.m_markers[i] != xe.m_markers[i] )
                return false;
        }
        return te.m_lifetime == xe.m_lifetime;
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
        //assert( te.m_rettype == xe.m_rettype );
        return true;
        )
    )
    throw "";
}
Ordering HIR::TypeRef::ord(const ::HIR::TypeRef& x) const
{
    Ordering    rv;
    
    ORD( static_cast<unsigned int>(m_data.tag()), static_cast<unsigned int>(x.m_data.tag()) );
    
    TU_MATCH(::HIR::TypeRef::Data, (m_data, x.m_data), (te, xe),
    (Infer,
        // TODO: Should comparing inferrence vars be an error?
        return ::ord( te.index, xe.index );
        ),
    (Diverge,
        return OrdEqual;
        ),
    (Primitive,
        return ::ord( static_cast<unsigned>(te), static_cast<unsigned>(xe) );
        ),
    (Path,
        ORD( (unsigned)te.path.m_data.tag(), (unsigned)xe.path.m_data.tag() );
        TU_MATCH(::HIR::Path::Data, (te.path.m_data, xe.path.m_data), (tpe, xpe),
        (Generic,
            return ::ord(tpe, xpe);
            ),
        (UfcsInherent,
            ORD(*tpe.type, *xpe.type);
            ORD(tpe.item, xpe.item);
            return ::ord(tpe.params, xpe.params);
            ),
        (UfcsKnown,
            ORD(*tpe.type, *xpe.type);
            ORD(tpe.trait, xpe.trait);
            ORD(tpe.item, xpe.item);
            return ::ord(tpe.params, xpe.params);
            ),
        (UfcsUnknown,
            ORD(*tpe.type, *xpe.type);
            ORD(tpe.item, xpe.item);
            return ::ord(tpe.params, xpe.params);
            )
        )
        ),
    (Generic,
        ORD(te.name, xe.name);
        if( (rv = ::ord(te.binding, xe.binding)) != OrdEqual )
            return rv;
        return OrdEqual;
        ),
    (TraitObject,
        ORD(te.m_trait, xe.m_trait);
        ORD(te.m_markers, xe.m_markers);
        return OrdEqual;
        //return ::ord(te.m_lifetime, xe.m_lifetime);
        ),
    (Array,
        ORD(*te.inner, *xe.inner);
        ORD(te.size_val, xe.size_val);
        if( te.size_val == ~0u )
            assert(!"TOD: Compre array types with non-resolved sizes");
        return OrdEqual;
        ),
    (Slice,
        return ::ord(*te.inner, *xe.inner);
        ),
    (Tuple,
        return ::ord(te, xe);
        ),
    (Borrow,
        ORD( static_cast<unsigned>(te.type), static_cast<unsigned>(xe.type) );
        return ::ord(*te.inner, *xe.inner);
        ),
    (Pointer,
        ORD( static_cast<unsigned>(te.type), static_cast<unsigned>(xe.type) );
        return ::ord(*te.inner, *xe.inner);
        ),
    (Function,
        ORD(te.is_unsafe, xe.is_unsafe);
        ORD(te.m_abi, xe.m_abi);
        ORD(te.m_arg_types, xe.m_arg_types);
        return ::ord(*te.m_rettype, *xe.m_rettype);
        ),
    (Closure,
        ORD( (::std::uintptr_t)te.node, xe.node);
        //assert( te.m_rettype == xe.m_rettype );
        return OrdEqual;
        )
    )
    throw "";
}


namespace {
    ::HIR::Compare match_generics_pp(const Span& sp, const ::HIR::PathParams& t, const ::HIR::PathParams& x, ::HIR::t_cb_resolve_type resolve_placeholder, ::HIR::t_cb_match_generics callback)
    {
        if( t.m_types.size() != x.m_types.size() ) {
            return ::HIR::Compare::Unequal;
        }
        
        auto rv = ::HIR::Compare::Equal;
        for(unsigned int i = 0; i < t.m_types.size(); i ++ )
        {
            rv &= t.m_types[i].match_test_generics_fuzz( sp, x.m_types[i], resolve_placeholder, callback );
            if( rv == ::HIR::Compare::Unequal )
                return rv;
        }
        
        return rv;
    }
}

void ::HIR::TypeRef::match_generics(const Span& sp, const ::HIR::TypeRef& x_in, t_cb_resolve_type resolve_placeholder, t_cb_match_generics callback) const {
    if( match_test_generics(sp, x_in, resolve_placeholder, callback) ) {
    }
    else {
        BUG(sp, "TypeRef::match_generics with mismatched parameters - " << *this << " and " << x_in);
    }
}
bool ::HIR::TypeRef::match_test_generics(const Span& sp, const ::HIR::TypeRef& x_in, t_cb_resolve_type resolve_placeholder, t_cb_match_generics callback) const
{
    return this->match_test_generics_fuzz(sp, x_in, resolve_placeholder, callback) == ::HIR::Compare::Equal;
}
::HIR::Compare HIR::TypeRef::match_test_generics_fuzz(const Span& sp, const ::HIR::TypeRef& x_in, t_cb_resolve_type resolve_placeholder, t_cb_match_generics callback) const
{
    if( m_data.is_Generic() ) {
        return callback(m_data.as_Generic().binding, x_in);
    }
    const auto& v = (this->m_data.is_Infer() ? resolve_placeholder(*this) : *this);
    const auto& x = (x_in.m_data.is_Infer() || x_in.m_data.is_Generic() ? resolve_placeholder(x_in) : x_in);
    TU_IFLET(::HIR::TypeRef::Data, x.m_data, Infer, xe,
        switch(xe.ty_class)
        {
        case ::HIR::InferClass::None:
            // - If right is generic infer, assume it's good
            //return true;
            return Compare::Fuzzy;
        case ::HIR::InferClass::Integer:
            TU_IFLET(::HIR::TypeRef::Data, v.m_data, Primitive, te,
                switch(te)
                {
                case ::HIR::CoreType::I8:    case ::HIR::CoreType::U8:
                case ::HIR::CoreType::I16:   case ::HIR::CoreType::U16:
                case ::HIR::CoreType::I32:   case ::HIR::CoreType::U32:
                case ::HIR::CoreType::I64:   case ::HIR::CoreType::U64:
                case ::HIR::CoreType::Isize: case ::HIR::CoreType::Usize:
                    return Compare::Fuzzy;
                    //return true;
                default:
                    DEBUG("- Fuzz fail");
                    return Compare::Unequal;
                }
            )
            break;
        case ::HIR::InferClass::Float:
            TU_IFLET(::HIR::TypeRef::Data, v.m_data, Primitive, te,
                switch(te)
                {
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    return Compare::Fuzzy;
                    //return true;
                default:
                    DEBUG("- Fuzz fail");
                    return Compare::Unequal;
                }
            )
            break;
        }
    )
    if( v.m_data.tag() != x.m_data.tag() ) {
        DEBUG("- Tag mismatch " << v << " and " << x);
        return Compare::Unequal;
    }
    TU_MATCH(::HIR::TypeRef::Data, (v.m_data, x.m_data), (te, xe),
    (Infer, throw "";),
    (Generic, throw "";),
    (Primitive,
        return (te == xe ? Compare::Equal : Compare::Unequal);
        ),
    (Diverge,
        return Compare::Equal;
        ),
    (Path,
        if( te.path.m_data.tag() != xe.path.m_data.tag() ) {
            return Compare::Unequal;
        }
        TU_MATCH(::HIR::Path::Data, (te.path.m_data, xe.path.m_data), (tpe, xpe),
        (Generic,
            if( tpe.m_path != xpe.m_path ) {
                return Compare::Unequal;
            }
            return match_generics_pp(sp, tpe.m_params, xpe.m_params, resolve_placeholder, callback);
            ),
        (UfcsKnown,
            auto cmp = tpe.type->match_test_generics_fuzz( sp, *xpe.type, resolve_placeholder, callback );
            if( tpe.trait.m_path != xpe.trait.m_path )
                return Compare::Unequal;
            cmp &= match_generics_pp(sp, tpe.trait.m_params, xpe.trait.m_params, resolve_placeholder, callback);
            if( tpe.item != xpe.item )
                return Compare::Unequal;
            cmp &= match_generics_pp(sp, tpe.params, xpe.params, resolve_placeholder, callback);
            return cmp;
            TODO(sp, "Path UfcsKnown - " << *this << " and " << x);
            ),
        (UfcsUnknown,
            auto cmp = tpe.type->match_test_generics_fuzz( sp, *xpe.type, resolve_placeholder, callback );
            if( tpe.item != xpe.item )
                return Compare::Unequal;
            cmp &= match_generics_pp(sp, tpe.params, xpe.params, resolve_placeholder, callback);
            return cmp;
            ),
        (UfcsInherent,
            auto cmp = tpe.type->match_test_generics_fuzz( sp, *xpe.type, resolve_placeholder, callback );
            if( tpe.item != xpe.item )
                return Compare::Unequal;
            cmp &= match_generics_pp(sp, tpe.params, xpe.params, resolve_placeholder, callback);
            return cmp;
            )
        )
        ),
    (TraitObject,
        TODO(sp, "TraitObject");
        ),
    (Array,
        return te.inner->match_test_generics_fuzz( sp, *xe.inner, resolve_placeholder, callback );
        ),
    (Slice,
        return te.inner->match_test_generics_fuzz( sp, *xe.inner, resolve_placeholder, callback );
        ),
    (Tuple,
        if( te.size() != xe.size() ) {
            return Compare::Unequal;
        }
        auto rv = Compare::Equal;
        for(unsigned int i = 0; i < te.size(); i ++ ) {
            rv &= te[i].match_test_generics_fuzz( sp, xe[i], resolve_placeholder, callback );
            if(rv == Compare::Unequal)
                return Compare::Unequal;
        }
        return rv;
        ),
    (Pointer,
        if( te.type != xe.type )
            return Compare::Unequal;
        return te.inner->match_test_generics_fuzz( sp, *xe.inner, resolve_placeholder, callback );
        ),
    (Borrow,
        if( te.type != xe.type )
            return Compare::Unequal;
        return te.inner->match_test_generics_fuzz( sp, *xe.inner, resolve_placeholder, callback );
        ),
    (Function,
        TODO(sp, "Function");
        ),
    (Closure,
        TODO(sp, "Closure");
        )
    )
    throw "";
}

::HIR::TypeRef::TypePathBinding HIR::TypeRef::TypePathBinding::clone() const {
    TU_MATCH(::HIR::TypeRef::TypePathBinding, (*this), (e),
    (Unbound, return ::HIR::TypeRef::TypePathBinding::make_Unbound({}); ),
    (Opaque , return ::HIR::TypeRef::TypePathBinding::make_Opaque({}); ),
    (Struct , return ::HIR::TypeRef::TypePathBinding(e); ),
    (Enum   , return ::HIR::TypeRef::TypePathBinding(e); )
    )
    assert(!"Fell off end of clone_binding");
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
            e.binding.clone()
            }) );
        ),
    (Generic,
        return ::HIR::TypeRef( Data::make_Generic(e) );
        ),
    (TraitObject,
        Data::Data_TraitObject  rv;
        rv.m_trait = e.m_trait.clone();
        for(const auto& trait : e.m_markers)
            rv.m_markers.push_back( trait.clone() );
        rv.m_lifetime = e.m_lifetime;
        return ::HIR::TypeRef( Data::make_TraitObject( mv$(rv) ) );
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
::HIR::Compare HIR::TypeRef::compare_with_placeholders(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder) const
{
    TRACE_FUNCTION_F(*this << " ?= " << x);
    const auto& left = (m_data.is_Infer() || m_data.is_Generic() ? resolve_placeholder(*this) : *this);
    //const auto& left = *this;
    const auto& right = (x.m_data.is_Infer() ? resolve_placeholder(x) : (x.m_data.is_Generic() ? resolve_placeholder(x) : x));
    
    // If the two types are the same ivar, return equal
    if( left.m_data.is_Infer() && left == right ) {
        return Compare::Equal;
    }
    
    // If left is infer
    TU_IFLET(::HIR::TypeRef::Data, left.m_data, Infer, e,
        switch(e.ty_class)
        {
        case ::HIR::InferClass::None:
            return Compare::Fuzzy;
        case ::HIR::InferClass::Integer:
            TU_IFLET( ::HIR::TypeRef::Data, right.m_data, Primitive, le,
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
            TU_IFLET( ::HIR::TypeRef::Data, right.m_data, Primitive, le,
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
    
    // If righthand side is infer, it's a fuzzy match (or not a match)
    TU_IFLET(::HIR::TypeRef::Data, right.m_data, Infer, e,
        switch( e.ty_class )
        {
        case ::HIR::InferClass::None:
            return Compare::Fuzzy;
        case ::HIR::InferClass::Integer:
            TU_IFLET( ::HIR::TypeRef::Data, left.m_data, Primitive, le,
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
            TU_IFLET( ::HIR::TypeRef::Data, left.m_data, Primitive, le,
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
    
    if( left.m_data.tag() != right.m_data.tag() ) {
        return Compare::Unequal;
    }
    TU_MATCH(::HIR::TypeRef::Data, (left.m_data, right.m_data), (le, re),
    (Infer, assert(!"infer");),
    (Diverge,
        return Compare::Equal;
        ),
    (Primitive,
        return (le == re ? Compare::Equal : Compare::Unequal);
        ),
    (Path,
        return le.path.compare_with_placeholders( sp, re.path, resolve_placeholder );
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
        return le.inner->compare_with_placeholders(sp, *re.inner, resolve_placeholder);
        ),
    (Slice,
        return le.inner->compare_with_placeholders(sp, *re.inner, resolve_placeholder);
        ),
    (Tuple,
        if( le.size() != re.size() )
            return Compare::Unequal;
        auto rv = Compare::Equal;
        for( unsigned int i = 0; i < le.size(); i ++ )
        {
            auto rv2 = le[i].compare_with_placeholders( sp, re[i], resolve_placeholder );
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
        return le.inner->compare_with_placeholders(sp, *re.inner, resolve_placeholder);
        ),
    (Pointer,
        if( le.type != re.type )
            return Compare::Unequal;
        return le.inner->compare_with_placeholders(sp, *re.inner, resolve_placeholder);
        ),
    (Function,
        if( le.m_abi != re.m_abi || le.is_unsafe != re.is_unsafe )
            return Compare::Unequal;
        if( le.m_arg_types.size() != re.m_arg_types.size() )
            return Compare::Unequal;
        auto rv = Compare::Equal;
        for( unsigned int i = 0; i < le.m_arg_types.size(); i ++ )
        {
            rv &= le.m_arg_types[i].compare_with_placeholders( sp, re.m_arg_types[i], resolve_placeholder );
            if( rv == Compare::Unequal )
                return Compare::Unequal;
        }
        rv &= le.m_rettype->compare_with_placeholders( sp, *re.m_rettype, resolve_placeholder );
        return rv;
        ),
    (Closure,
        if( le.m_arg_types.size() != re.m_arg_types.size() )
            return Compare::Unequal;
        auto rv = Compare::Equal;
        for( unsigned int i = 0; i < le.m_arg_types.size(); i ++ )
        {
            rv &= le.m_arg_types[i].compare_with_placeholders( sp, re.m_arg_types[i], resolve_placeholder );
            if( rv == Compare::Unequal )
                return Compare::Unequal;
        }
        rv &= le.m_rettype->compare_with_placeholders( sp, *re.m_rettype, resolve_placeholder );
        return rv;
        )
    )
    throw "";
}
