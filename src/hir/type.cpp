/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/type.cpp
 * - HIR Type helper code
 */
#include "type.hpp"
#include <span.hpp>
#include "expr.hpp" // Hack for cloning array types

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
        case CoreType::U128: return os << "u128";
        case CoreType::I128: return os << "i128";

        case CoreType::F32: return os << "f32";
        case CoreType::F64: return os << "f64";

        case CoreType::Bool:    return os << "bool";
        case CoreType::Char:    return os << "char";
        case CoreType::Str:     return os << "str";
        }
        assert(!"Bad CoreType value");
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const BorrowType& bt)
    {
        switch(bt)
        {
        case BorrowType::Owned:     return os << "Owned";
        case BorrowType::Unique:    return os << "Unique";
        case BorrowType::Shared:    return os << "Shared";
        }
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const ArraySize& x)
    {
        TU_MATCH_HDRA( (x), { )
        TU_ARMA(Unevaluated, se) {
            os << se;
            }
        TU_ARMA(Known, se)
            os << se;
        }
        return os;
    }
}

void HIR::GenericRef::fmt(std::ostream& os) const
{
    os << this->name << "/*";
    if( this->binding == GENERIC_Self )
        os << "";
    else {
        switch(this->group())
        {
        case 0: os << "I:" << this->idx();  break;
        case 1: os << "M:" << this->idx();  break;
        case 2: os << "P:" << this->idx();  break;
        default:
            os << this->binding;
            break;
        }
    }
    os << "*/";
}

Ordering HIR::ArraySize::ord(const HIR::ArraySize& x) const
{
    if(this->tag() != x.tag())
        return ::ord( static_cast<unsigned>(this->tag()), static_cast<unsigned>(x.tag()) );
    TU_MATCH_HDRA( (*this, x), {)
    TU_ARMA(Unevaluated, tse, xse)
        return ::ord(tse, xse);
    TU_ARMA(Known, tse, xse)
        return ::ord(tse, xse);
    }
    throw "";
}

HIR::ArraySize HIR::ArraySize::clone() const
{
    TU_MATCH_HDRA( (*this), {)
    TU_ARMA(Unevaluated, se)
        return se.clone();
    TU_ARMA(Known, se)
        return se;
    }
    throw "";
}

void ::HIR::TypeRef::fmt(::std::ostream& os) const
{
    if(!m_ptr) {
        os << "NULL";
        return ;
    }

    thread_local static std::vector<const HIR::TypeInner*>  s_recurse_stack;
    for(const auto* p : s_recurse_stack) {
        if( p == m_ptr ) {
            os << "RECURSE";
            return ;
        }
    }
    struct _ {
        _(const HIR::TypeInner* ptr) {
            s_recurse_stack.push_back(ptr);
        }
        ~_() {
            s_recurse_stack.pop_back();
        }
    } h(m_ptr);

    //os << "{" << m_ptr << "}";
    TU_MATCH_HDRA( (data()), { )
    TU_ARMA(Infer, e) {
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
        }
    TU_ARMA(Diverge, e) {
        os << "!";
        }
    TU_ARMA(Primitive, e) {
        os << e;
        }
    TU_ARMA(Path, e) {
        os << e.path;
        TU_MATCH(::HIR::TypePathBinding, (e.binding), (be),
        (Unbound, os << "/*?*/";),
        (Opaque, os << "/*O*/";),
        (ExternType, os << "/*X*/";),
        (Struct, os << "/*S*/";),
        (Union, os << "/*U*/";),
        (Enum, os << "/*E*/";)
        )
        }
    TU_ARMA(Generic, e) {
        os << e;
        }
    TU_ARMA(TraitObject, e) {
        os << "dyn (";
        if( e.m_trait.m_path != ::HIR::GenericPath() )
        {
            os << e.m_trait;
        }
        for(const auto& tr : e.m_markers)
            os << "+" << tr;
        if( e.m_lifetime != LifetimeRef::new_static() )
            os << "+" << e.m_lifetime;
        os << ")";
        }
    TU_ARMA(ErasedType, e) {
        os << "impl ";
        for(const auto& tr : e.m_traits) {
            if( &tr != &e.m_traits[0] )
                os << "+";
            os << tr;
        }
        if( e.m_lifetime != LifetimeRef::new_static() )
            os << "+" << e.m_lifetime;
        os << "/*" << e.m_origin << "#" << e.m_index << "*/";
        }
    TU_ARMA(Array, e) {
        os << "[" << e.inner << "; " << e.size << "]";
        }
    TU_ARMA(Slice, e) {
        os << "[" << e.inner << "]";
        }
    TU_ARMA(Tuple, e) {
        os << "(";
        for(const auto& t : e)
            os << t << ", ";
        os << ")";
        }
    TU_ARMA(Borrow, e) {
        switch(e.type)
        {
        case ::HIR::BorrowType::Shared: os << "&";  break;
        case ::HIR::BorrowType::Unique: os << "&mut ";  break;
        case ::HIR::BorrowType::Owned:  os << "&move "; break;
        }
        os << e.inner;
        }
    TU_ARMA(Pointer, e) {
        switch(e.type)
        {
        case ::HIR::BorrowType::Shared: os << "*const ";  break;
        case ::HIR::BorrowType::Unique: os << "*mut ";  break;
        case ::HIR::BorrowType::Owned:  os << "*move "; break;
        }
        os << e.inner;
        }
    TU_ARMA(Function, e) {
        if( e.is_unsafe ) {
            os << "unsafe ";
        }
        if( e.m_abi != "" ) {
            os << "extern \"" << e.m_abi << "\" ";
        }
        os << "fn(";
        for(const auto& t : e.m_arg_types)
            os << t << ", ";
        os << ") -> " << e.m_rettype;
        }
    TU_ARMA(Closure, e) {
        os << "closure["<<e.node<<"]";
        os << "(";
        for(const auto& t : e.m_arg_types)
            os << t << ", ";
        os << ") -> " << e.m_rettype;
        }
    TU_ARMA(Generator, e) {
        os << "generator["<<e.node<<"]";
        }
    }
}

bool ::HIR::TypeRef::operator==(const ::HIR::TypeRef& x) const
{
    if( m_ptr == x.m_ptr )
        return true;
    
    if( !m_ptr || !x.m_ptr )
        return false;
    if( data().tag() != x.data().tag() )
        return false;

    TU_MATCH(::HIR::TypeData, (data(), x.data()), (te, xe),
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
        return te.path == xe.path;
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
    (ErasedType,
        return te.m_origin == xe.m_origin;
        ),
    (Array,
        if( te.inner != xe.inner )
            return false;
        if( xe.size != te.size )
            return false;
        return true;
        ),
    (Slice,
        return te.inner == xe.inner;
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
        return te.inner == xe.inner;
        ),
    (Pointer,
        if( te.type != xe.type )
            return false;
        return te.inner == xe.inner;
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
        ),
    (Generator,
        return te.node == xe.node;
        )
    )
    throw "";
}
Ordering HIR::TypeRef::ord(const ::HIR::TypeRef& x) const
{
    Ordering    rv;

    if( &data() == &x.data() )
        return OrdEqual;

    ORD( static_cast<unsigned int>(data().tag()), static_cast<unsigned int>(x.data().tag()) );

    TU_MATCH(::HIR::TypeData, (data(), x.data()), (te, xe),
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
        return ::ord( te.path, xe.path );
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
    (ErasedType,
        ORD(te.m_origin, xe.m_origin);
        ORD(te.m_traits, xe.m_traits);
        return OrdEqual;
        ),
    (Array,
        ORD(te.inner, xe.inner);
        ORD(te.size, xe.size);
        return OrdEqual;
        ),
    (Slice,
        return ::ord(te.inner, xe.inner);
        ),
    (Tuple,
        return ::ord(te, xe);
        ),
    (Borrow,
        ORD( static_cast<unsigned>(te.type), static_cast<unsigned>(xe.type) );
        return ::ord(te.inner, xe.inner);
        ),
    (Pointer,
        ORD( static_cast<unsigned>(te.type), static_cast<unsigned>(xe.type) );
        return ::ord(te.inner, xe.inner);
        ),
    (Function,
        ORD(te.is_unsafe, xe.is_unsafe);
        ORD(te.m_abi, xe.m_abi);
        ORD(te.m_arg_types, xe.m_arg_types);
        return ::ord(te.m_rettype, xe.m_rettype);
        ),
    (Closure,
        ORD( reinterpret_cast<::std::uintptr_t>(te.node), reinterpret_cast<::std::uintptr_t>(xe.node) );
        //assert( te.m_rettype == xe.m_rettype );
        return OrdEqual;
        ),
    (Generator,
        ORD( reinterpret_cast<::std::uintptr_t>(te.node), reinterpret_cast<::std::uintptr_t>(xe.node) );
        return OrdEqual;
        )
    )
    throw "";
}
#if 0
bool ::HIR::TypeRef::contains_generics() const
{
    struct H {
        static bool vec_contains_generics(const ::std::vector<TypeRef>& v) {
            for( const auto& t : v )
                if( t.contains_generics() )
                    return true;
            return false;
        }
    };
    TU_MATCH(::HIR::TypeData, (data()), (te),
    (Infer,
        return false;
        ),
    (Diverge,
        return false;
        ),
    (Primitive,
        return false;
        ),
    (Path,
        TU_MATCH(::HIR::Path::Data, (te.path.m_data), (tpe),
        (Generic,
            return H::vec_contains_generics( tpe.m_params.m_types );
            ),
        (UfcsInherent,
            if( tpe.type->contains_generics() )
                return true;
            TODO(Span(), "UfcsInherent");
            ),
        (UfcsKnown,
            TODO(Span(), "UfcsKnown");
            ),
        (UfcsUnknown,
            TODO(Span(), "UfcsUnknown");
            )
        )
        ),
    (Generic,
        return true;
        ),
    (TraitObject,
        TODO(Span(), "TraitObject");
        ),
    (ErasedType,
        TODO(Span(), "ErasedType");
        ),
    (Array,
        return te.inner->contains_generics();
        ),
    (Slice,
        return te.inner->contains_generics();
        ),
    (Tuple,
        return H::vec_contains_generics(te);
        ),
    (Borrow,
        return te.inner->contains_generics();
        ),
    (Pointer,
        return te.inner->contains_generics();
        ),
    (Function,
        return H::vec_contains_generics(te.m_arg_types) || te.m_rettype->contains_generics();
        ),
    (Closure,
        return H::vec_contains_generics(te.m_arg_types) || te.m_rettype->contains_generics();
        )
    )
    throw "";
}
#endif


namespace {
    ::HIR::Compare match_generics_pp(const Span& sp, const ::HIR::PathParams& t, const ::HIR::PathParams& x, ::HIR::t_cb_resolve_type resolve_placeholder, ::HIR::MatchGenerics& callback)
    {
        return t.match_test_generics_fuzz(sp, x, resolve_placeholder, callback);
    }
    ::HIR::Compare match_values(const Span& sp, const ::HIR::ConstGeneric& t, const ::HIR::ConstGeneric& x, ::HIR::MatchGenerics& callback)
    {
        // LHS generic: call callback
        if( const auto* e = t.opt_Generic() ) {
            return callback.match_val(*e, x);
        }

        // Either are infer, check for exact match or return fuzzy
        if(const auto* xep = x.opt_Infer())
        {
            const auto& xe = *xep;

            if( xe.index != ~0u && t.is_Infer() && t.as_Infer().index == xe.index )
            {
                return ::HIR::Compare::Equal;
            }

            return ::HIR::Compare::Fuzzy;
        }
        if(const auto* tep = t.opt_Infer())
        {
            const auto& te = *tep;
            ASSERT_BUG(sp, te.index != ~0u, "Encountered ivar for `this` - " << t);
            return ::HIR::Compare::Fuzzy;
        }

        if( t.tag() != x.tag() )
        {
            return ::HIR::Compare::Unequal;
        }

        TU_MATCH_HDRA( (t,x), { )
        TU_ARMA(Infer, te,xe) throw "Unreachable";
        TU_ARMA(Unevaluated, te,xe) {
            if(te == xe) {
                return ::HIR::Compare::Equal;
            }
            }
        TU_ARMA(Generic, te,xe) throw "Unreachable";
        TU_ARMA(Evaluated, te, xe)
            return *te == *xe ? ::HIR::Compare::Equal : ::HIR::Compare::Unequal;
        }
	throw "Unreachable";
    }
}

bool ::HIR::TypeRef::match_test_generics(const Span& sp, const ::HIR::TypeRef& x_in, t_cb_resolve_type resolve_placeholder, ::HIR::MatchGenerics& callback) const
{
    return this->match_test_generics_fuzz(sp, x_in, resolve_placeholder, callback) == ::HIR::Compare::Equal;
}
::HIR::Compare HIR::TypeRef::match_test_generics_fuzz(const Span& sp, const ::HIR::TypeRef& x_in, t_cb_resolve_type resolve_placeholder, ::HIR::MatchGenerics& callback) const
{
    if( const auto* e = data().opt_Generic() ) {
        return callback.match_ty(*e, x_in, resolve_placeholder);
    }
    const auto& v = (this->data().is_Infer() ? resolve_placeholder(*this) : *this);
    const auto& x = (x_in.data().is_Infer() || x_in.data().is_Generic() ? resolve_placeholder(x_in) : x_in);
    TRACE_FUNCTION_F(*this << ", " << x_in << " -- " << v << ", " << x);
    // If `x` is an ivar - This can be a fuzzy match.
    if(const auto* xep = x.data().opt_Infer())
    {
        const auto& xe = *xep;
        // - If type inferrence is active (i.e. this ivar has an index), AND both `v` and `x` refer to the same ivar slot
        if( xe.index != ~0u && v.data().is_Infer() && v.data().as_Infer().index == xe.index )
        {
            // - They're equal (no fuzzyness about it)
            return Compare::Equal;
        }
        switch(xe.ty_class)
        {
        case ::HIR::InferClass::None:
            // TODO: Have another callback (optional?) that allows the caller to equate `v` somehow
            // - Very niche?
            return Compare::Fuzzy;
        case ::HIR::InferClass::Integer:
            if(const auto* te = v.data().opt_Primitive())
            {
                switch(*te)
                {
                case ::HIR::CoreType::I8:    case ::HIR::CoreType::U8:
                case ::HIR::CoreType::I16:   case ::HIR::CoreType::U16:
                case ::HIR::CoreType::I32:   case ::HIR::CoreType::U32:
                case ::HIR::CoreType::I64:   case ::HIR::CoreType::U64:
                case ::HIR::CoreType::I128:  case ::HIR::CoreType::U128:
                case ::HIR::CoreType::Isize: case ::HIR::CoreType::Usize:
                    return Compare::Fuzzy;
                    //return true;
                default:
                    DEBUG("- Fuzz fail");
                    return Compare::Unequal;
                }
            }
            break;
        case ::HIR::InferClass::Float:
            if(const auto* te = v.data().opt_Primitive())
            {
                switch(*te)
                {
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    return Compare::Fuzzy;
                    //return true;
                default:
                    DEBUG("- Fuzz fail");
                    return Compare::Unequal;
                }
            }
            break;
        }
    }

    if(const auto* tep = v.data().opt_Infer())
    {
        const auto& te = *tep;
        // TODO: Restrict this block with a flag so it panics if an ivar is seen when not expected
        ASSERT_BUG(sp, te.index != ~0u, "Encountered ivar for `this` - " << v);

        switch(te.ty_class)
        {
        case ::HIR::InferClass::None:
            // TODO: Have another callback (optional?) that allows the caller to equate `v` somehow
            // - Very niche?
            return Compare::Fuzzy;
        case ::HIR::InferClass::Integer:
            if(const auto* xe = x.data().opt_Primitive())
            {
                switch(*xe)
                {
                case ::HIR::CoreType::I8:    case ::HIR::CoreType::U8:
                case ::HIR::CoreType::I16:   case ::HIR::CoreType::U16:
                case ::HIR::CoreType::I32:   case ::HIR::CoreType::U32:
                case ::HIR::CoreType::I64:   case ::HIR::CoreType::U64:
                case ::HIR::CoreType::I128:  case ::HIR::CoreType::U128:
                case ::HIR::CoreType::Isize: case ::HIR::CoreType::Usize:
                    return Compare::Fuzzy;
                default:
                    DEBUG("- Fuzz fail");
                    return Compare::Unequal;
                }
            }
            break;
        case ::HIR::InferClass::Float:
            if(const auto* xe = x.data().opt_Primitive())
            {
                switch(*xe)
                {
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    return Compare::Fuzzy;
                default:
                    DEBUG("- Fuzz fail");
                    return Compare::Unequal;
                }
            }
            break;
        }
    }
#if 1
    thread_local static std::vector<const HIR::TypeInner*>  s_recurse_stack;
    for(const auto* p : s_recurse_stack) {
        if( p == m_ptr ) {
            DEBUG("Recursion");
            ASSERT_BUG(sp, &v == &x, "Recursion with unequal type pointers");
            return HIR::Compare::Equal;
        }
    }
    struct _ {
        _(const HIR::TypeInner* ptr) {
            s_recurse_stack.push_back(ptr);
        }
        ~_() {
            s_recurse_stack.pop_back();
        }
    } h(m_ptr);
#else
    // NOTE: This doesn't allow matching identical types (which can be desirable)
    if( &v == &x ) {
        DEBUG("Pointer equality");
        return HIR::Compare::Equal;
    }
#endif

    if( v.data().tag() != x.data().tag() ) {
        // HACK: If the path is Opaque, return a fuzzy match.
        // - This works around an impl selection bug.
        if( v.data().is_Path() && v.data().as_Path().binding.is_Opaque() ) {
            DEBUG("- Fuzzy match due to opaque - " << v << " = " << x);
            return Compare::Fuzzy;
        }
        // HACK: If RHS is unbound, fuzz it
        if( x.data().is_Path() && x.data().as_Path().binding.is_Unbound() ) {
            DEBUG("- Fuzzy match due to unbound - " << v << " = " << x);
            return Compare::Fuzzy;
        }
        if( v.data().is_Path() && v.data().as_Path().binding.is_Unbound() ) {
            DEBUG("- Fuzzy match due to unbound - " << v << " = " << x);
            return Compare::Fuzzy;
        }
        // HACK: If the RHS is a placeholder generic, allow it.
        if( x.data().is_Generic() && (x.data().as_Generic().binding >> 8) == 2 ) {
            DEBUG("- Fuzzy match due to placeholder - " << v << " = " << x);
            return Compare::Fuzzy;
        }
        DEBUG("- Tag mismatch " << v << " and " << x);
        return Compare::Unequal;
    }
    TU_MATCH_HDRA( (v.data(), x.data()), { )
    TU_ARMA(Infer, te, xe) {
        // Both sides are infer
        switch(te.ty_class)
        {
        case ::HIR::InferClass::None:
            return Compare::Fuzzy;
        default:
            switch(xe.ty_class)
            {
            case ::HIR::InferClass::None:
                return Compare::Fuzzy;
            default:
                if( te.ty_class != xe.ty_class )
                    return Compare::Unequal;
                return Compare::Fuzzy;
            }
        }
        }
    TU_ARMA(Generic, te, xe) throw "";
    TU_ARMA(Primitive, te, xe) {
        return (te == xe ? Compare::Equal : Compare::Unequal);
        }
    TU_ARMA(Diverge, te, xe) {
        return Compare::Equal;
        }
    TU_ARMA(Path, te, xe) {
        ::HIR::Compare  rv = Compare::Unequal;
        if( te.path.m_data.tag() != xe.path.m_data.tag() ) {
            rv = Compare::Unequal;
        }
        else {
            TU_MATCH_HDRA((te.path.m_data, xe.path.m_data), {)
            TU_ARMA(Generic, tpe, xpe) {
                if( tpe.m_path != xpe.m_path ) {
                    rv = Compare::Unequal;
                }
                else {
                    rv = match_generics_pp(sp, tpe.m_params, xpe.m_params, resolve_placeholder, callback);
                }
                }
            TU_ARMA(UfcsKnown, tpe, xpe) {
                rv = tpe.type.match_test_generics_fuzz( sp, xpe.type, resolve_placeholder, callback );
                if( tpe.trait.m_path != xpe.trait.m_path )
                    rv = Compare::Unequal;
                rv &= match_generics_pp(sp, tpe.trait.m_params, xpe.trait.m_params, resolve_placeholder, callback);
                if( tpe.item != xpe.item )
                    rv = Compare::Unequal;
                rv &= match_generics_pp(sp, tpe.params, xpe.params, resolve_placeholder, callback);
                }
            TU_ARMA(UfcsUnknown, tpe, xpe) {
                rv = tpe.type.match_test_generics_fuzz( sp, xpe.type, resolve_placeholder, callback );
                if( tpe.item != xpe.item )
                    rv = Compare::Unequal;
                rv &= match_generics_pp(sp, tpe.params, xpe.params, resolve_placeholder, callback);
                }
            TU_ARMA(UfcsInherent, tpe, xpe) {
                rv = tpe.type.match_test_generics_fuzz( sp, xpe.type, resolve_placeholder, callback );
                if( tpe.item != xpe.item )
                    rv = Compare::Unequal;
                rv &= match_generics_pp(sp, tpe.params, xpe.params, resolve_placeholder, callback);
                }
            }
        }

        if( rv == ::HIR::Compare::Unequal ) {
            if( te.binding.is_Unbound() || xe.binding.is_Unbound() ) {
                rv = ::HIR::Compare::Fuzzy;
            }
            if( te.binding.is_Opaque() ) {
                DEBUG("- Fuzzy match due to opaque");
                return Compare::Fuzzy;
            }
        }
        return rv;
        }
    TU_ARMA(TraitObject, te, xe) {
        if( te.m_trait.m_path.m_path != xe.m_trait.m_path.m_path ) {
            return Compare::Unequal;
        }
        if( te.m_markers.size() != xe.m_markers.size() ) {
            return Compare::Unequal;
        }
        auto cmp = match_generics_pp(sp, te.m_trait.m_path.m_params, xe.m_trait.m_path.m_params, resolve_placeholder, callback);
        for(unsigned int i = 0; i < te.m_markers.size(); i ++)
        {
            cmp &= match_generics_pp(sp, te.m_markers[i].m_params, xe.m_markers[i].m_params, resolve_placeholder, callback);
        }

        auto it_l = te.m_trait.m_type_bounds.begin();
        auto it_r = xe.m_trait.m_type_bounds.begin();
        while( it_l != te.m_trait.m_type_bounds.end() && it_r != xe.m_trait.m_type_bounds.end() )
        {
            if( it_l->first != it_r->first ) {
                return Compare::Unequal;
            }
            cmp &= it_l->second.type .match_test_generics_fuzz( sp, it_r->second.type, resolve_placeholder, callback );
            ++ it_l;
            ++ it_r;
        }

        if( it_l != te.m_trait.m_type_bounds.end() || it_r != xe.m_trait.m_type_bounds.end() ) {
            return Compare::Unequal;
        }
        return cmp;
        }
    TU_ARMA(ErasedType, te, xe) {
        if( te.m_origin != xe.m_origin )
            return Compare::Unequal;
        return Compare::Equal;
        }
    TU_ARMA(Array, te, xe) {
        auto rv = Compare::Equal;
        if( const auto* tse = te.size.opt_Unevaluated() )
        {
            HIR::ConstGeneric   v;
            if( xe.size.opt_Known() ) {
                rv &= match_values(sp, *tse, EncodedLiteralPtr( EncodedLiteral::make_usize(xe.size.as_Known()) ), callback);
            }
            else {
                rv &= match_values(sp, *tse, xe.size.as_Unevaluated(), callback );
            }
        }
        else if( te.size != xe.size ) {
            return Compare::Unequal;
        }
        return te.inner.match_test_generics_fuzz( sp, xe.inner, resolve_placeholder, callback );
        }
    TU_ARMA(Slice, te, xe) {
        return te.inner.match_test_generics_fuzz( sp, xe.inner, resolve_placeholder, callback );
        }
    TU_ARMA(Tuple, te, xe) {
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
        }
    TU_ARMA(Pointer, te, xe) {
        if( te.type != xe.type )
            return Compare::Unequal;
        return te.inner.match_test_generics_fuzz( sp, xe.inner, resolve_placeholder, callback );
        }
    TU_ARMA(Borrow, te, xe) {
        if( te.type != xe.type )
            return Compare::Unequal;
        return te.inner.match_test_generics_fuzz( sp, xe.inner, resolve_placeholder, callback );
        }
    TU_ARMA(Function, te, xe) {
        if( te.is_unsafe != xe.is_unsafe )
            return Compare::Unequal;
        if( te.m_abi != xe.m_abi )
            return Compare::Unequal;
        if( te.m_arg_types.size() != xe.m_arg_types.size() )
            return Compare::Unequal;
        auto rv = Compare::Equal;
        for( unsigned int i = 0; i < te.m_arg_types.size(); i ++ ) {
            rv &= te.m_arg_types[i] .match_test_generics_fuzz( sp, xe.m_arg_types[i], resolve_placeholder, callback );
            if( rv == Compare::Unequal )
                return rv;
        }
        rv &= te.m_rettype.match_test_generics_fuzz( sp, xe.m_rettype, resolve_placeholder, callback );
        return rv;
        }
    TU_ARMA(Closure, te, xe) {
        if( te.node != xe.node )
            return Compare::Unequal;
        return Compare::Equal;
        }
    TU_ARMA(Generator, te, xe) {
        if( te.node != xe.node )
            return Compare::Unequal;
        return Compare::Equal;
        }
    }
    throw "";
}

::HIR::TypePathBinding HIR::TypePathBinding::clone() const {
    TU_MATCH(::HIR::TypePathBinding, (*this), (e),
    (Unbound, return ::HIR::TypePathBinding::make_Unbound({}); ),
    (Opaque , return ::HIR::TypePathBinding::make_Opaque({}); ),
    (ExternType, return ::HIR::TypePathBinding(e); ),
    (Struct, return ::HIR::TypePathBinding(e); ),
    (Union , return ::HIR::TypePathBinding(e); ),
    (Enum  , return ::HIR::TypePathBinding(e); )
    )
    assert(!"Fell off end of clone_binding");
    throw "";
}
bool HIR::TypePathBinding::operator==(const HIR::TypePathBinding& x) const
{
    if( this->tag() != x.tag() )
        return false;
    TU_MATCH(::HIR::TypePathBinding, (*this, x), (te, xe),
    (Unbound, return true;),
    (Opaque, return true;),
    (ExternType, return te == xe;),
    (Struct, return te == xe;),
    (Union , return te == xe;),
    (Enum  , return te == xe;)
    )
    throw "";
}

const ::HIR::TraitMarkings* HIR::TypePathBinding::get_trait_markings() const
{
    const ::HIR::TraitMarkings* markings_ptr = nullptr;
    TU_MATCHA( (*this), (tpb),
    (Unbound,   ),
    (Opaque,   ),
    (ExternType, markings_ptr = &tpb->m_markings; ),
    (Struct, markings_ptr = &tpb->m_markings; ),
    (Union,  markings_ptr = &tpb->m_markings; ),
    (Enum,   markings_ptr = &tpb->m_markings; )
    )
    return markings_ptr;
}

::HIR::TypeRef HIR::TypeRef::clone() const
{
    return HIR::TypeRef(*this);
}
::HIR::TypeRef HIR::TypeRef::clone_shallow() const
{
    TU_MATCH_HDRA( (data()), {)
    TU_ARMA(Infer, e) {
        return ::HIR::TypeRef( TypeData::make_Infer(e) );
        }
    TU_ARMA(Diverge, e) {
        return ::HIR::TypeRef( TypeData::make_Diverge({}) );
        }
    TU_ARMA(Primitive, e) {
        return ::HIR::TypeRef( TypeData::make_Primitive(e) );
        }
    TU_ARMA(Path, e) {
        return ::HIR::TypeRef( TypeData::make_Path({
            e.path.clone(),
            e.binding.clone()
            }) );
        }
    TU_ARMA(Generic, e) {
        return ::HIR::TypeRef( TypeData::make_Generic(e) );
        }
    TU_ARMA(TraitObject, e) {
        TypeData::Data_TraitObject  rv;
        rv.m_trait = e.m_trait.clone();
        for(const auto& trait : e.m_markers)
            rv.m_markers.push_back( trait.clone() );
        rv.m_lifetime = e.m_lifetime;
        return ::HIR::TypeRef( TypeData::make_TraitObject( mv$(rv) ) );
        }
    TU_ARMA(ErasedType, e) {
        ::std::vector< ::HIR::TraitPath>    traits;
        traits.reserve( e.m_traits.size() );
        for(const auto& trait : e.m_traits)
            traits.push_back( trait.clone() );
        return ::HIR::TypeRef( TypeData::make_ErasedType({
            e.m_origin.clone(), e.m_index,
            e.m_is_sized,
            mv$(traits),
            e.m_lifetime
            }) );
        }
    TU_ARMA(Array, e) {
        return ::HIR::TypeRef( TypeData::make_Array({ e.inner.clone(), e.size.clone() }) );
        }
    TU_ARMA(Slice, e) {
        return ::HIR::TypeRef( TypeData::make_Slice({ e.inner.clone() }) );
        }
    TU_ARMA(Tuple, e) {
        ::std::vector< ::HIR::TypeRef>  types;
        for(const auto& t : e)
            types.push_back( t.clone() );
        return ::HIR::TypeRef( TypeData::make_Tuple(mv$(types)) );
        }
    TU_ARMA(Borrow, e) {
        return ::HIR::TypeRef( TypeData::make_Borrow({e.lifetime, e.type, e.inner.clone()}) );
        }
    TU_ARMA(Pointer, e) {
        return ::HIR::TypeRef( TypeData::make_Pointer({e.type, e.inner.clone()}) );
        }
    TU_ARMA(Function, e) {
        FunctionType    ft {
            e.is_unsafe,
            e.m_abi,
            e.m_rettype.clone(),
            {}
            };
        for(const auto& a : e.m_arg_types)
            ft.m_arg_types.push_back( a.clone() );
        return ::HIR::TypeRef(TypeData::make_Function( mv$(ft) ));
        }
    TU_ARMA(Closure, e) {
        TypeData::Data_Closure  oe;
        oe.node = e.node;
        oe.m_rettype = e.m_rettype.clone();
        for(const auto& a : e.m_arg_types)
            oe.m_arg_types.push_back( a.clone() );
        return ::HIR::TypeRef(TypeData::make_Closure( mv$(oe) ));
        }
    TU_ARMA(Generator, e) {
        TypeData::Data_Generator    oe;
        oe.node = e.node;
        return ::HIR::TypeRef(TypeData::make_Generator( mv$(oe) ));
        }
    }
    throw "";
}
::HIR::Compare HIR::TypeRef::compare_with_placeholders(const Span& sp, const ::HIR::TypeRef& x, t_cb_resolve_type resolve_placeholder) const
{
    //TRACE_FUNCTION_F(*this << " ?= " << x);
    const auto& left = (data().is_Infer() || data().is_Generic() ? resolve_placeholder(*this) : *this);
    //const auto& left = *this;
    const auto& right = (x.data().is_Infer() ? resolve_placeholder(x) : (x.data().is_Generic() ? resolve_placeholder(x) : x));

    // If the two types are the same ivar, return equal
    if( left.data().is_Infer() && left == right ) {
        return Compare::Equal;
    }

    // Unbound paths and placeholder generics
    if( left.data().tag() != right.data().tag() ) {
        if( left.data().is_Path() && left.data().as_Path().binding.is_Unbound() ) {
            return Compare::Fuzzy;
        }
        if( right.data().is_Path() && right.data().as_Path().binding.is_Unbound() ) {
            return Compare::Fuzzy;
        }
        if( left.data().is_Generic() && (left.data().as_Generic().binding >> 8) == 2 ) {
            return Compare::Fuzzy;
        }
        if( right.data().is_Generic() && (right.data().as_Generic().binding >> 8) == 2 ) {
            return Compare::Fuzzy;
        }
    }

    // If left is infer
    if(const auto* e = left.data().opt_Infer() )
    {
        switch(e->ty_class)
        {
        case ::HIR::InferClass::None:
            return Compare::Fuzzy;
        case ::HIR::InferClass::Integer:
            TU_MATCH_HDRA( (right.data()), {)
            default:
                return Compare::Unequal;
            TU_ARMA(Primitive, re) {
                switch(re)
                {
                case ::HIR::CoreType::I8:    case ::HIR::CoreType::U8:
                case ::HIR::CoreType::I16:   case ::HIR::CoreType::U16:
                case ::HIR::CoreType::I32:   case ::HIR::CoreType::U32:
                case ::HIR::CoreType::I64:   case ::HIR::CoreType::U64:
                case ::HIR::CoreType::I128:  case ::HIR::CoreType::U128:
                case ::HIR::CoreType::Isize: case ::HIR::CoreType::Usize:
                    return Compare::Fuzzy;
                default:
                    return Compare::Unequal;
                }
                }
            TU_ARMA(Infer, re) {
                switch(re.ty_class)
                {
                case ::HIR::InferClass::None:
                case ::HIR::InferClass::Integer:
                    return Compare::Fuzzy;
                case ::HIR::InferClass::Float:
                    return Compare::Unequal;
                }
                }
            TU_ARMA(Path, re) {
                return re.binding.is_Unbound() ? Compare::Fuzzy : Compare::Unequal;
                }
            }
        case ::HIR::InferClass::Float:
            TU_MATCH_HDRA( (right.data()), {)
            default:
                return Compare::Unequal;
            TU_ARMA(Primitive, re) {
                switch(re)
                {
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    return Compare::Fuzzy;
                default:
                    return Compare::Unequal;
                }
                }
            TU_ARMA(Infer, re) {
                switch(re.ty_class)
                {
                case ::HIR::InferClass::None:
                case ::HIR::InferClass::Float:
                    return Compare::Fuzzy;
                case ::HIR::InferClass::Integer:
                    return Compare::Unequal;
                }
                }
            TU_ARMA(Path, re) {
                return re.binding.is_Unbound() ? Compare::Fuzzy : Compare::Unequal;
                }
            }
        }
        throw "";
    }

    // If righthand side is infer, it's a fuzzy match (or not a match)
    if(const auto* re = right.data().opt_Infer())
    {
        switch( re->ty_class )
        {
        case ::HIR::InferClass::None:
            return Compare::Fuzzy;
        case ::HIR::InferClass::Integer:
            TU_MATCH_HDRA( (left.data()), {)
            default:
                return Compare::Unequal;
            TU_ARMA(Primitive, le) {
                switch(le)
                {
                case ::HIR::CoreType::I8:    case ::HIR::CoreType::U8:
                case ::HIR::CoreType::I16:   case ::HIR::CoreType::U16:
                case ::HIR::CoreType::I32:   case ::HIR::CoreType::U32:
                case ::HIR::CoreType::I64:   case ::HIR::CoreType::U64:
                case ::HIR::CoreType::I128:  case ::HIR::CoreType::U128:
                case ::HIR::CoreType::Isize: case ::HIR::CoreType::Usize:
                    return Compare::Fuzzy;
                default:
                    return Compare::Unequal;
                }
                }
            TU_ARMA(Path, le) {
                return le.binding.is_Unbound() ? Compare::Fuzzy : Compare::Unequal;
                }
            }
        case ::HIR::InferClass::Float:
            TU_MATCH_HDRA( (left.data()), {)
            default:
                return Compare::Unequal;
            TU_ARMA(Primitive, le) {
                switch(le)
                {
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    return Compare::Fuzzy;
                default:
                    return Compare::Unequal;
                }
                }
            TU_ARMA(Path, le) {
                return le.binding.is_Unbound() ? Compare::Fuzzy : Compare::Unequal;
                }
            }
        }
        throw "";
    }

    // If righthand is a type parameter, it can only match another type parameter
    // - See `(Generic,` below

    if( left.data().tag() != right.data().tag() ) {
        return Compare::Unequal;
    }
    TU_MATCH_HDRA( (left.data(), right.data()), {)
    TU_ARMA(Infer, le, re) { assert(!"infer"); }
    TU_ARMA(Diverge, le, re) {
        return Compare::Equal;
        }
    TU_ARMA(Primitive, le, re) {
        return (le == re ? Compare::Equal : Compare::Unequal);
        }
    TU_ARMA(Path, le, re) {
        auto rv = le.path.compare_with_placeholders( sp, re.path, resolve_placeholder );
        if( rv == ::HIR::Compare::Unequal ) {
            if( le.binding.is_Unbound() || re.binding.is_Unbound() ) {
                rv = ::HIR::Compare::Fuzzy;
            }
        }
        return rv;
        }
    TU_ARMA(Generic, le, re) {
        if( le.binding != re.binding ) {
            if( (le.binding >> 8) == 2 )
                return Compare::Fuzzy;
            if( (re.binding >> 8) == 2 )
                return Compare::Fuzzy;
            return Compare::Unequal;
        }
        return Compare::Equal;
        }
    TU_ARMA(TraitObject, le, re) {
        if( le.m_markers.size() != re.m_markers.size() )
            return Compare::Unequal;
        auto rv = le.m_trait .compare_with_placeholders( sp, re.m_trait, resolve_placeholder );
        if( rv == Compare::Unequal )
            return rv;
        for( unsigned int i = 0; i < le.m_markers.size(); i ++ )
        {
            auto rv2 = le.m_markers[i] .compare_with_placeholders( sp, re.m_markers[i], resolve_placeholder );
            if( rv2 == Compare::Unequal )
                return Compare::Unequal;
            if( rv2 == Compare::Fuzzy )
                rv = Compare::Fuzzy;
        }
        return rv;
        }
    TU_ARMA(ErasedType, le, re) {
        auto rv = le.m_origin .compare_with_placeholders( sp, le.m_origin, resolve_placeholder );
        return rv;
        //TODO(sp, "ErasedType");
        }
    TU_ARMA(Array, le, re) {
        if( le.size != re.size )
            return Compare::Unequal;
        return le.inner.compare_with_placeholders(sp, re.inner, resolve_placeholder);
        }
    TU_ARMA(Slice, le, re) {
        return le.inner.compare_with_placeholders(sp, re.inner, resolve_placeholder);
        }
    TU_ARMA(Tuple, le, re) {
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
        }
    TU_ARMA(Borrow, le, re) {
        if( le.type != re.type )
            return Compare::Unequal;
        return le.inner.compare_with_placeholders(sp, re.inner, resolve_placeholder);
        }
    TU_ARMA(Pointer, le, re) {
        if( le.type != re.type )
            return Compare::Unequal;
        return le.inner.compare_with_placeholders(sp, re.inner, resolve_placeholder);
        }
    TU_ARMA(Function, le, re) {
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
        rv &= le.m_rettype.compare_with_placeholders( sp, re.m_rettype, resolve_placeholder );
        return rv;
        }
    TU_ARMA(Closure, le, re) {
        if( le.node != re.node )
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
        rv &= le.m_rettype.compare_with_placeholders( sp, re.m_rettype, resolve_placeholder );
        return rv;
        }
    TU_ARMA(Generator, le, re) {
        if( le.node != re.node )
            return Compare::Unequal;
        return Compare::Equal;
        }
    }
    throw "";
}
