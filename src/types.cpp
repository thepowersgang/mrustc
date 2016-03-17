/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 * 
 * types.cpp
 * - Backing code for the TypeRef class
 *
 * Handles a chunk of type resolution (merging) and matching/comparing types
 */
#include "types.hpp"
#include "ast/ast.hpp"
#include "ast/crate.hpp"

/// Mappings from internal type names to the core type enum
static const struct {
    const char* name;
    enum eCoreType  type;
} CORETYPES[] = {
    {"bool", CORETYPE_BOOL},
    {"char", CORETYPE_CHAR},
    {"f32", CORETYPE_F32},
    {"f64", CORETYPE_F64},
    {"i16", CORETYPE_I16},
    {"i32", CORETYPE_I32},
    {"i64", CORETYPE_I64},
    {"i8", CORETYPE_I8},
    {"int", CORETYPE_INT},
    {"isize", CORETYPE_INT},
    {"u16", CORETYPE_U16},
    {"u32", CORETYPE_U32},
    {"u64", CORETYPE_U64},
    {"u8",  CORETYPE_U8},
    {"uint", CORETYPE_UINT},
    {"usize", CORETYPE_UINT},
};

enum eCoreType coretype_fromstring(const ::std::string& name)
{
    for(unsigned int i = 0; i < sizeof(CORETYPES)/sizeof(CORETYPES[0]); i ++)
    {
        if( name < CORETYPES[i].name )
            break;
        if( name == CORETYPES[i].name )
            return CORETYPES[i].type;
    }
    return CORETYPE_INVAL;
}

const char* coretype_name(const eCoreType ct ) {
    switch(ct)
    {
    case CORETYPE_INVAL:return "-";
    case CORETYPE_ANY:  return "_";
    case CORETYPE_CHAR: return "char";
    case CORETYPE_BOOL: return "bool";
    case CORETYPE_UINT: return "usize";
    case CORETYPE_INT:  return "isize";
    case CORETYPE_U8:   return "u8";
    case CORETYPE_I8:   return "i8";
    case CORETYPE_U16:  return "u16";
    case CORETYPE_I16:  return "i16";
    case CORETYPE_U32:  return "u32";
    case CORETYPE_I32:  return "i32";
    case CORETYPE_U64:  return "u64";
    case CORETYPE_I64:  return "i64";
    case CORETYPE_F32:  return "f32";
    case CORETYPE_F64:  return "f64";
    }
    DEBUG("Unknown core type?! " << ct);
    return "NFI";
}

Type_Function::Type_Function(const Type_Function& other):
    is_unsafe(other.is_unsafe),
    m_abi(other.m_abi),
    m_rettype( box$( TypeRef(*other.m_rettype) ) ),
    m_arg_types(other.m_arg_types)
{
}
SERIALISE_TYPE_A(Type_Function::, "Type_Function", {
    s.item( is_unsafe );
    s.item( m_abi );
    s.item( m_rettype );
    s.item( m_arg_types );
    })

Ordering Type_Function::ord(const Type_Function& x) const
{
    Ordering rv;
    
    rv = ::ord(m_abi, x.m_abi);
    if(rv != OrdEqual)  return rv;
    rv = ::ord(m_arg_types, x.m_arg_types);
    if(rv != OrdEqual)  return rv;
    return (*m_rettype).ord( *x.m_rettype );
}

TypeRef::TypeRef(const TypeRef& other)
{
    switch( other.m_data.tag() )
    {
    #define _COPY(VAR)  case TypeData::TAG_##VAR: m_data = TypeData::make_##VAR(other.m_data.as_##VAR()); break;
    #define _CLONE(VAR, code...)    case TypeData::TAG_##VAR: { auto& old = other.m_data.as_##VAR(); m_data = TypeData::make_##VAR(code); } break;
    _COPY(None)
    _COPY(Any)
    case TypeData::TAG_Macro:   assert( !"Copying an unexpanded type macro" );
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

/// Replace this type reference with a dereferenced version
bool TypeRef::deref(bool is_implicit)
{
    #define _(VAR, ...) case TypeData::TAG_##VAR: { auto &ent = m_data.as_##VAR(); (void)&ent; __VA_ARGS__ } break;
    switch(m_data.tag())
    {
    case TypeData::TAG_None:        throw ::std::runtime_error("Dereferencing ! - bugcheck");
    case TypeData::TAG_Macro:       throw ::std::runtime_error("Dereferencing unexpanded macro - bugcheck");
    case TypeData::TAG_Any:         throw ::std::runtime_error("Dereferencing _");
    case TypeData::TAG_Unit:        throw ::std::runtime_error("Dereferencing ()");
    case TypeData::TAG_Primitive:   throw ::std::runtime_error("Dereferencing a primtive type");
    case TypeData::TAG_Function:    throw ::std::runtime_error("Dereferencing a function");
    
    case TypeData::TAG_Generic:
        // TODO: Check for Deref<Output=?> bound
        throw ::std::runtime_error("TODO: Check for a Deref bound on generic");
    _(Borrow,
        *this = *ent.inner;
        return true;
        )
    _(Pointer,
        // raw pointers can't be implicitly dereferenced
        if( is_implicit )
            return false;
        *this = *ent.inner;
        return true;
        )
    case TypeData::TAG_Tuple:
    case TypeData::TAG_Array:
    case TypeData::TAG_Path:
        throw ::std::runtime_error("TODO: Search for an impl of Deref");
    case TypeData::TAG_TraitObject:
        throw ::std::runtime_error("TODO: TypeRef::deref on MULTIDST");
    }
    #undef _
    throw ::std::runtime_error("BUGCHECK: Fell off end of TypeRef::deref");
}

/// Merge the contents of the passed type with this type
/// 
/// \note Both types must be of the same form (i.e. both be tuples)
void TypeRef::merge_with(const TypeRef& other)
{
    // Ignore if other is wildcard
    //if( other.m_class == TypeRef::ANY ) {
    //    assert(other.m_inner_types.size() == 0 && "TODO: merge_with on bounded _");
    //    return;
    //}
   
    // If this is a wildcard, then replace with the other type 
    if( m_data.is_Any() ) {
        //if( m_inner_types.size() && m_inner_types.size() != other.m_inner_types.size() )
        //    throw ::std::runtime_error("TypeRef::merge_with - Handle bounded wildcards");
        *this = other;
        return;
    }
    
    // If classes don't match, then merge is impossible
    if( m_data.tag() != other.m_data.tag() )
        throw ::std::runtime_error("TypeRef::merge_with - Types not compatible");
    
    // If both types are concrete, then they must be the same
    if( is_concrete() && other.is_concrete() )
    {
        if( *this != other )
            throw ::std::runtime_error("TypeRef::merge_with - Types not compatible");
        return;
    }
    
    
    TU_MATCH(TypeData, (m_data, other.m_data), (ent, other_ent),
    (None,
        throw ::std::runtime_error("TypeRef::merge_with - Reached concrete/wildcard");
        ),
    (Macro,
        throw ::std::runtime_error("TypeRef::merge_with - Reached unexpanded macro");
        ),
    (Any,
        throw ::std::runtime_error("TypeRef::merge_with - Reached concrete/wildcard");
        ),
    (Unit,
        throw ::std::runtime_error("TypeRef::merge_with - Reached concrete/wildcard");
        ),
    (Primitive,
        throw ::std::runtime_error("TypeRef::merge_with - Reached concrete/wildcard");
        ),
    (Function,
        ent.info.m_rettype->merge_with( *other_ent.info.m_rettype );
        
        if( ent.info.m_arg_types.size() != other_ent.info.m_arg_types.size() )
            throw ::std::runtime_error("TypeRef::merge_with - Types not compatible [function sz]");
        for(unsigned int i = 0; i < ent.info.m_arg_types.size(); i ++)
        {
            ent.info.m_arg_types[i].merge_with( other_ent.info.m_arg_types[i] );
        }
        ),
    (Tuple,
        // Other is known not to be wildcard, and is also a tuple, so it must be the same size
        if( ent.inner_types.size() != other_ent.inner_types.size() )
            throw ::std::runtime_error("TypeRef::merge_with - Types not compatible [tuple sz]");
        for(unsigned int i = 0; i < ent.inner_types.size(); i ++)
        {
            ent.inner_types[i].merge_with( other_ent.inner_types[i] );
        }
        ),
    (Borrow,
        if( ent.is_mut != other_ent.is_mut )
            throw ::std::runtime_error("TypeRef::merge_with - Types not compatible [inner mut]");
        ent.inner->merge_with( *other_ent.inner );
        ),
    (Pointer,
        if( ent.is_mut != other_ent.is_mut )
            throw ::std::runtime_error("TypeRef::merge_with - Types not compatible [inner mut]");
        ent.inner->merge_with( *other_ent.inner );
        ),
    (Array,
        throw ::std::runtime_error("TODO: TypeRef::merge_with on ARRAY");
        ),
    (Generic,
        throw ::std::runtime_error("TODO: TypeRef::merge_with on GENERIC");
        ),
    (Path,
        throw ::std::runtime_error("TODO: TypeRef::merge_with on PATH");
        ),
    (TraitObject,
        throw ::std::runtime_error("TODO: TypeRef::merge_with on MULTIDST");
        )
    )
}

/// Resolve all Generic/Argument types to the value returned by the passed closure
/// 
/// Replaces every instance of a TypeRef::GENERIC with the value returned from the passed
/// closure.
void TypeRef::resolve_args(::std::function<TypeRef(const char*)> fcn)
{
    DEBUG("" << *this);
    #define _(VAR, ...) case TypeData::TAG_##VAR: { auto &ent = m_data.as_##VAR(); (void)&ent; __VA_ARGS__ } break;
    switch(m_data.tag())
    {
    _(None,
        throw ::std::runtime_error("TypeRef::resolve_args on !");
        )
    _(Macro,
        throw ::std::runtime_error("TypeRef::resolve_args on unexpanded macro");
        )
    // TODO: Is resolving args on an ANY an erorr?
    _(Any)
    _(Unit)
    _(Primitive)
    _(Function,
        ent.info.m_rettype->resolve_args(fcn);
        for( auto& t : ent.info.m_arg_types )
            t.resolve_args(fcn);
        )
    _(Tuple,
        for( auto& t : ent.inner_types )
            t.resolve_args(fcn);
        )
    _(Borrow,
        ent.inner->resolve_args(fcn);
        )
    _(Pointer,
        ent.inner->resolve_args(fcn);
        )
    _(Array,
        ent.inner->resolve_args(fcn);
        )
    _(Generic,
        *this = fcn(ent.name.c_str());
        )
    _(Path,
        ent.path.resolve_args(fcn);
        )
    _(TraitObject,
        for( auto& p : ent.traits )
            p.resolve_args(fcn);
        )
    }
    #undef _
}

/// Match this type against another type, calling the provided function for all generics found in this
///
/// \param other    Type containing (possibly) concrete types
/// \param fcn  Function to call for all generics (called with matching type from \a other)
/// This is used to handle extracting types passsed to methods/enum variants
void TypeRef::match_args(const TypeRef& other, ::std::function<void(const char*,const TypeRef&)> fcn) const
{
    // If this type is a generic, then call the closure with the other type
    if( m_data.tag() == TypeData::TAG_Generic ) {
        fcn( m_data.as_Generic().name.c_str(), other );
        return ;
    }
    
    // If the other type is a wildcard, early return
    // - TODO - Might want to restrict the other type to be of the same form as this type
    if( other.m_data.tag() == TypeData::TAG_Any )
        return;
    
    DEBUG("this = " << *this << ", other = " << other);
    
    // Any other case, it's a "pattern" match
    if( m_data.tag() != other.m_data.tag() )
        throw ::std::runtime_error("Type mismatch (class)");
    TU_MATCH(TypeData, (m_data, other.m_data), (ent, other_ent),
    (None,
        throw ::std::runtime_error("TypeRef::match_args on !");
        ),
    (Macro,
        throw CompileError::BugCheck("TypeRef::match_args - unexpanded macro");
        ),
    (Any,
        // Wait, isn't this an error?
        throw ::std::runtime_error("Encountered '_' in match_args");
        ),
    (Unit),
    (Primitive,
        // TODO: Should check if the type matches
        if( ent.core_type != other_ent.core_type )
            throw ::std::runtime_error("Type mismatch (core)");
        ),
    (Function,
        if( ent.info.m_abi != other_ent.info.m_abi )
            throw ::std::runtime_error("Type mismatch (function abi)");
        ent.info.m_rettype->match_args( *other_ent.info.m_rettype, fcn );
        
        if( ent.info.m_arg_types.size() != other_ent.info.m_arg_types.size() )
            throw ::std::runtime_error("Type mismatch (function size)");
        for(unsigned int i = 0; i < ent.info.m_arg_types.size(); i ++ )
            ent.info.m_arg_types[i].match_args( other_ent.info.m_arg_types[i], fcn );
        ),
    (Tuple,
        if( ent.inner_types.size() != other_ent.inner_types.size() )
            throw ::std::runtime_error("Type mismatch (tuple size)");
        for(unsigned int i = 0; i < ent.inner_types.size(); i ++ )
            ent.inner_types[i].match_args( other_ent.inner_types[i], fcn );
        ),
    (Borrow,
        if( ent.is_mut != other_ent.is_mut )
            throw ::std::runtime_error("Type mismatch (inner mutable)");
        ent.inner->match_args( *other_ent.inner, fcn );
        ),
    (Pointer,
        if( ent.is_mut != other_ent.is_mut )
            throw ::std::runtime_error("Type mismatch (inner mutable)");
        ent.inner->match_args( *other_ent.inner, fcn );
        ),
    (Array,
        ent.inner->match_args( *other_ent.inner, fcn );
        if(ent.size.get() || other_ent.size.get())
        {
            throw ::std::runtime_error("TODO: Sized array match_args");
        }
        ),
    (Generic,
        throw ::std::runtime_error("Encountered GENERIC in match_args");
        ),
    (Path,
        ent.path.match_args(other_ent.path, fcn);
        ),
    (TraitObject,
        throw ::std::runtime_error("TODO: TypeRef::match_args on MULTIDST");
        )
    )
}

bool TypeRef::impls_wildcard(const AST::Crate& crate, const AST::Path& trait) const
{
    TU_MATCH(TypeData, (m_data), (ent),
    (None,
        throw CompileError::BugCheck("TypeRef::impls_wildcard on !");
        ),
    (Macro,
        throw CompileError::BugCheck("TypeRef::impls_wildcard - unexpanded macro");
        ),
    (Any,
        throw CompileError::BugCheck("TypeRef::impls_wildcard on _");
        ),
    // Generics are an error?
    (Generic,
        // TODO: Include an annotation to the GenericParams structure relevant to this type
        // - Allows searching the params for the impl, without having to pass the params down
        throw CompileError::Todo("TypeRef::impls_wildcard - param");
        ),
    // Primitives always impl
    (Unit, return true; ),
    (Primitive, return true; ),
    // Functions are pointers (currently), so they implement the trait
    (Function, return true; ),
    // Pointers/arrays inherit directly
    (Borrow,
        return crate.find_impl(trait, *ent.inner, nullptr, nullptr);
        ),
    (Pointer,
        return crate.find_impl(trait, *ent.inner, nullptr, nullptr);
        ),
    (Array,
        return crate.find_impl(trait, *ent.inner, nullptr, nullptr);
        ),
    // Tuples just destructure
    (Tuple,
        for( const auto& fld : ent.inner_types )
        {
            if( !crate.find_impl(trait, fld, nullptr, nullptr) )
                return false;
        }
        return true;
        ),
    // Path types destructure
    (Path,
        // - structs need all fields to impl this trait (cache result)
        // - same for enums, tuples, arrays, pointers...
        // - traits check the Self bounds
        // CATCH: Need to handle recursion, keep a list of currently processing paths and assume true if found
        TU_MATCH_DEF(AST::PathBinding, (ent.path.binding()), (info),
        (
            throw CompileError::Todo("wildcard impls - auto determine path");
            ),
        (Struct,
            const auto &s = *info.struct_;
            GenericResolveClosure   resolve_fn( s.params(), ent.path.nodes().back().args() );
            TU_MATCH(::AST::StructData, (s.m_data), (e),
            (Struct,
                for(const auto& fld : e.ents)
                {
                    auto fld_ty = fld.m_type;
                    fld_ty.resolve_args( resolve_fn );
                    DEBUG("- Fld '" << fld.m_name << "' := " << fld.m_type << " => " << fld_ty);
                    if( !crate.find_impl(trait, fld_ty, nullptr, nullptr) )
                        return false;
                }
                ),
            (Tuple,
                for(const auto& fld : e.ents)
                {
                    auto fld_ty = fld.m_type;
                    fld_ty.resolve_args( resolve_fn );
                    DEBUG("- Fld ? := " << fld.m_type << " => " << fld_ty);
                    if( !crate.find_impl(trait, fld_ty, nullptr, nullptr) )
                        return false;
                }
                )
            )
            return true;
            ),
        (Enum,
            const auto& i = *info.enum_;
            GenericResolveClosure   resolve_fn( i.params(), ent.path.nodes().back().args() );
            for( const auto& var : i.variants() )
            {
                TU_MATCH(AST::EnumVariantData, (var.m_data), (e),
                (Value,
                    ),
                (Tuple,
                    for( const auto& ty : e.m_sub_types )
                    {
                        TypeRef real_ty = ty;
                        real_ty.resolve_args( resolve_fn );
                        DEBUG("- Var '" << var.m_name << "' := " << ty << " => " << real_ty);
                        if( !crate.find_impl(trait, real_ty, nullptr, nullptr) )
                            return false;
                    }
                    ),
                (Struct,
                    for( const auto& fld : e.m_fields )
                    {
                        auto fld_ty = fld.m_type;
                        fld_ty.resolve_args( resolve_fn );
                        DEBUG("- Fld '" << fld.m_name << "' := " << fld.m_type << " => " << fld_ty);
                        if( !crate.find_impl(trait, fld_ty, nullptr, nullptr) )
                            return false;
                    }
                    )
                )
            }
            return true;
            )
        )
        ),
    // MultiDST is special - It only impls if this trait is in the list
    //  (or if a listed trait requires/impls the trait)
    (TraitObject,
        throw CompileError::Todo("TypeRef::impls_wildcard - MULTIDST");
        )
    )
    throw CompileError::BugCheck("TypeRef::impls_wildcard - Fell off end");
}

/// Checks if the type is fully bounded
bool TypeRef::is_concrete() const
{
    #define _(VAR, ...) case TypeData::VAR: { const auto &ent = m_data.as_##VAR(); (void)&ent; __VA_ARGS__ } break;
    TU_MATCH(TypeData, (m_data), (ent),
    (None,
        throw ::std::runtime_error("TypeRef::is_concrete on !");
        ),
    (Macro, throw CompileError::BugCheck("TypeRef::is_concrete - unexpanded macro");),
    (Any, return false;),
    (Unit, return true; ),
    (Primitive, return true; ),
    (Function,
        if( not ent.info.m_rettype->is_concrete() )
            return false;
        for(const auto& t : ent.info.m_arg_types )
            if( not t.is_concrete() )
                return false;
        return true;
        ),
    (Tuple,
        for(const auto& t : ent.inner_types)
            if( not t.is_concrete() )
                return false;
        return true;
        ),
    (Borrow,
        return ent.inner->is_concrete();
        ),
    (Pointer,
        return ent.inner->is_concrete();
        ),
    (Array,
        return ent.inner->is_concrete();
        ),
    (Generic,
        // Well, I guess a generic param is "concrete"
        return true;
        ),
    (Path,
        return ent.path.is_concrete();
        ),
    (TraitObject,
        for(const auto& p : ent.traits )
            if( not p.is_concrete() )
                return false;
        return true;
        )
    )
    #undef _
    throw ::std::runtime_error( FMT("BUGCHECK - Invalid type class on " << *this) );
}

int TypeRef::equal_no_generic(const TypeRef& x) const
{
    //DEBUG(*this << ", " << x);
    if( m_data.tag() == TypeData::TAG_Generic ) //|| x.m_class == TypeRef::GENERIC )
        return 1;
    if( m_data.tag() != x.m_data.tag() )  return -1;

    TU_MATCH(TypeData, (m_data, x.m_data), (ent, x_ent),
    (None, return 0;),
    (Macro, throw CompileError::BugCheck("TypeRef::equal_no_generic - unexpanded macro");),
    (Unit, return 0;),
    (Any, return 0;),
    (Primitive,
        if( ent.core_type != x_ent.core_type )  return -1;
        return 0;
        ),
    (Function,
        if( ent.info.m_abi != x_ent.info.m_abi )    return -1;
        throw CompileError::Todo("TypeRef::equal_no_generic - FUNCTION");
        ),
    (Generic,
        throw CompileError::BugCheck("equal_no_generic - Generic should have been handled above");
        ),
    (Path,
        return ent.path.equal_no_generic( x_ent.path );
        ),
    (Borrow,
        if( ent.is_mut != x_ent.is_mut )
            return -1;
        return ent.inner->equal_no_generic( *x_ent.inner );
        ),
    (Pointer,
        if( ent.is_mut != x_ent.is_mut )
            return -1;
        return ent.inner->equal_no_generic( *x_ent.inner );
        ),
    (Array,
        if( ent.size.get() || x_ent.size.get() )
            throw CompileError::Todo("TypeRef::equal_no_generic - sized array");
        return ent.inner->equal_no_generic( *x_ent.inner );
        ),
    (Tuple,
        bool fuzzy = false;
        if( ent.inner_types.size() != x_ent.inner_types.size() )
            return -1;
        for( unsigned int i = 0; i < ent.inner_types.size(); i ++ )
        {
            int rv = ent.inner_types[i].equal_no_generic( x_ent.inner_types[i] );
            if(rv < 0)  return -1;
            if(rv > 0)  fuzzy = true;
        }
        return (fuzzy ? 1 : 0);
        ),
    (TraitObject,
        throw CompileError::Todo("TypeRef::equal_no_generic - MULTIDST");
        )
    )
    throw CompileError::BugCheck("equal_no_generic - Ran off end");
}
Ordering TypeRef::ord(const TypeRef& x) const
{
    Ordering    rv;
    
    rv = ::ord( (unsigned)m_data.tag(), (unsigned)x.m_data.tag() );
    if(rv != OrdEqual)  return rv;
    
    TU_MATCH(TypeData, (m_data, x.m_data), (ent, x_ent),
    (None, return OrdEqual;),
    (Macro, throw CompileError::BugCheck("TypeRef::ord - unexpanded macro");),
    (Any,  return OrdEqual;),
    (Unit, return OrdEqual;),
    (Primitive,
        return ::ord( (unsigned)ent.core_type, (unsigned)x_ent.core_type );
        ),
    (Function,
        return ent.info.ord( x_ent.info );
        ),
    (Tuple,
        return ::ord(ent.inner_types, x_ent.inner_types);
        ),
    (Borrow,
        rv = ::ord(ent.is_mut, x_ent.is_mut);
        if(rv != OrdEqual)  return rv;
        return (*ent.inner).ord(*x_ent.inner);
        ),
    (Pointer,
        rv = ::ord(ent.is_mut, x_ent.is_mut);
        if(rv != OrdEqual)  return rv;
        return (*ent.inner).ord(*x_ent.inner);
        ),
    (Array,
        rv = (*ent.inner).ord( *x_ent.inner );
        if(rv != OrdEqual)  return rv;
        if(ent.size.get())
        {
            throw ::std::runtime_error("TODO: Sized array comparisons");
        }
        return OrdEqual;
        ),
    (Generic,
        if( ent.params != x_ent.params )
        {
            DEBUG(*this << " == " << x);
            if( ent.params )   DEBUG("- (L) " << *ent.params);
            if( x_ent.params ) DEBUG("- (R) " << *x_ent.params);
            throw ::std::runtime_error("Can't compare mismatched generic types");
            //BUG(m_span, "Can't compare mismatched generic types");
        }
        else {
        }
        return ::ord(ent.name, x_ent.name);
        ),
    (Path,
        return ent.path.ord( x_ent.path );
        ),
    (TraitObject,
        return ::ord(ent.traits, x_ent.traits);
        )
    )
    throw ::std::runtime_error(FMT("BUGCHECK - Unhandled TypeRef class '" << m_data.tag() << "'"));
}

::std::ostream& operator<<(::std::ostream& os, const eCoreType ct) {
    return os << coretype_name(ct);
}

::std::ostream& operator<<(::std::ostream& os, const TypeRef& tr) {
    //os << "TypeRef(";
    #define _(VAR, ...) case TypeData::TAG_##VAR: { const auto &ent = tr.m_data.as_##VAR(); (void)&ent; __VA_ARGS__ } break;
    switch(tr.m_data.tag())
    {
    _(None,
        os << "!";
        )
    _(Any,
        os << "_";
        )
    _(Macro,
        os << ent.inv;
        )
    _(Unit,
        os << "()";
        )
    _(Primitive,
        os << tr.m_data.as_Primitive().core_type;
        )
    _(Function,
        if( ent.info.m_abi != "" )
            os << "extern \"" << ent.info.m_abi << "\" ";
        os << "fn (";
        for( const auto& arg : ent.info.m_arg_types )
            os << arg << ", ";
        os << ") -> " << *ent.info.m_rettype;
        )
    _(Tuple,
        //os << "TagTuple, {" << tr.m_inner_types << "}";
        os << "( ";
        for( const auto& it : ent.inner_types )
            os << it << ", ";
        os << ")";
        )
    _(Borrow,
        //os << "TagReference, " << (tr.m_is_inner_mutable ? "mut" : "const") << ", " << tr.m_inner_types[0];
        os << "&" << (ent.is_mut ? "mut " : "") << *ent.inner;
        )
    _(Pointer,
        //os << "TagPointer, " << (tr.m_is_inner_mutable ? "mut" : "const") << ", " << tr.m_inner_types[0];
        os << "*" << (ent.is_mut ? "mut" : "const") << " " << *ent.inner;
        )
    _(Array,
        os << "[" << *ent.inner;
        if( ent.size.get() )
            os << "; " << *ent.size;
        os << "]";
        )
    _(Generic,
        os << "/* arg */ " << ent.name << "/*"<<ent.level<<"*/";
        )
    _(Path,
        os << ent.path;
        )
    _(TraitObject,
        os << "(";
        for( const auto& it : ent.traits ) {
            if( &it != &ent.traits.front() )
                os << "+";
            os << it;
        }
        os << ")";
        )
    }
    #undef _
    //os << ")";
    return os;
}

void operator% (::Serialiser& s, eCoreType ct) {
    s << coretype_name(ct);
}
void operator% (::Deserialiser& d, eCoreType& ct) {
    ::std::string n;
    d.item(n);
    /* */if(n == "-")   ct = CORETYPE_INVAL;
    else if(n == "_")   ct = CORETYPE_ANY;
    else if(n == "bool")  ct = CORETYPE_BOOL;
    else if(n == "char")  ct = CORETYPE_CHAR;
    else if(n == "usize") ct = CORETYPE_UINT;
    else if(n == "isize") ct = CORETYPE_INT;
    else if(n == "u8")    ct = CORETYPE_U8;
    else if(n == "i8")    ct = CORETYPE_I8;
    else if(n == "u16")   ct = CORETYPE_U16;
    else if(n == "i16")   ct = CORETYPE_I16;
    else if(n == "u32")   ct = CORETYPE_U32;
    else if(n == "i32")   ct = CORETYPE_I32;
    else if(n == "u64")   ct = CORETYPE_U64;
    else if(n == "i64")   ct = CORETYPE_I64;
    else if(n == "f32")   ct = CORETYPE_F32;
    else if(n == "f64")   ct = CORETYPE_F64;
    else
        throw ::std::runtime_error("Deserialise failure - coretype " + n);
}
void operator%(Serialiser& s, TypeData::Tag c) {
    s << TypeData::tag_to_str(c);
}
void operator%(::Deserialiser& s, TypeData::Tag& c) {
    ::std::string   n;
    s.item(n);
    c = TypeData::tag_from_str(n);
}

::std::unique_ptr<TypeRef> TypeRef::from_deserialiser(Deserialiser& s) {
    TypeRef n;
    n.deserialise(s);
    return box$(n);
}

#define _S(VAR, ...)  case TypeData::TAG_##VAR: { const auto& ent = m_data.as_##VAR(); (void)&ent; __VA_ARGS__ } break;
#define _D(VAR, ...)  case TypeData::TAG_##VAR: { m_data = TypeData::make_null_##VAR(); auto& ent = m_data.as_##VAR(); (void)&ent; __VA_ARGS__ } break;
SERIALISE_TYPE(TypeRef::, "TypeRef", {
    s % m_data.tag();
    switch(m_data.tag())
    {
    _S(None)
    _S(Macro,
        s.item( ent.inv );
        )
    _S(Any)
    _S(Unit)
    _S(Primitive,
        s << coretype_name(ent.core_type);
        )
    _S(Function,
        s.item( ent.info );
        )
    _S(Tuple,
        s.item( ent.inner_types );
        )
    _S(Borrow,
        s.item( ent.is_mut );
        s.item( ent.inner );
        )
    _S(Pointer,
        s.item( ent.is_mut );
        s.item( ent.inner );
        )
    _S(Generic,
        s.item( ent.name );
        s.item( ent.level );
        )
    _S(Array,
        s.item( ent.inner );
        bool size_present = (ent.size.get() != 0);
        s.item( size_present );
        if(ent.size.get()) {
            s.item( ent.size );
        }
        )
    _S(Path,
        s.item( ent.path );
        )
    _S(TraitObject,
        s.item( ent.traits );
        )
    }
},{
    TypeData::Tag  tag;
    s % tag;
    switch(tag)
    {
    _D(None)
    _D(Any)
    _D(Unit)
    _D(Macro,
        s.item( ent.inv );
        )
    _D(Primitive,
        s % ent.core_type;
        )
    _D(Function,
        s.item( ent.info );
        )
    _D(Tuple,
        s.item( ent.inner_types );
        )
    _D(Borrow,
        s.item( ent.is_mut );
        s.item( ent.inner );
        )
    _D(Pointer,
        s.item( ent.is_mut );
        s.item( ent.inner );
        )
    _D(Generic,
        s.item( ent.name );
        s.item( ent.level );
        )
    _D(Array,
        s.item( ent.inner );
        bool size_present;
        s.item( size_present );
        if( size_present )
            ent.size = AST::ExprNode::from_deserialiser(s);
        else
            ent.size.reset();
        )
    _D(Path,
        s.item( ent.path );
        )
    _D(TraitObject,
        s.item( ent.traits );
        )
    }
})
#undef _D
#undef _S


void PrettyPrintType::print(::std::ostream& os) const
{
    #if 1
    os << m_type;
    #else
    switch(m_type.m_class)
    {
    case TypeRef::ANY:
        os << "_";
        if( m_type.m_inner_types.size() ) {
            os << "/* : " << m_type.m_inner_types << "*/";
        }
        break;
    case TypeRef::UNIT:
        os << "()";
        break;
    case TypeRef::PRIMITIVE:
        os << m_type.m_core_type;
        break;
    case TypeRef::FUNCTION:
        if( m_type.m_path[0].name() != "" )
            os << "extern \"" << m_type.m_path[0].name() << "\" ";
        os << "fn (";
        for( unsigned int i = 0; i < m_type.m_inner_types.size()-1; i ++ )
            os << m_type.m_inner_types[i].print_pretty() << ", ";
        os << ") -> " << m_type.m_inner_types.back().print_pretty();
        break;
    case TypeRef::TUPLE:
        os << "(";
        for(const auto& t : m_type.m_inner_types)
            os << t.print_pretty() << ",";
        os << ")";
        break;
    case TypeRef::REFERENCE:
        os << "&" << (m_type.m_is_inner_mutable ? "mut " : "") << m_type.m_inner_types[0].print_pretty();
        break;
    case TypeRef::POINTER:
        os << "*" << (m_type.m_is_inner_mutable ? "mut" : "const") << " " << m_type.m_inner_types[0].print_pretty();
        break;
    case TypeRef::ARRAY:
        os << "[" << m_type.m_inner_types[0].print_pretty() << ", " << m_type.m_size_expr << "]";
        break;
    case TypeRef::GENERIC:
        os << m_type.m_path[0].name();
        break;
    case TypeRef::PATH:
        os << m_type.m_path;
        break;
    }
    #endif
}

::std::ostream& operator<<(::std::ostream& os, const PrettyPrintType& v)
{
    v.print(os);
    return os;
}
