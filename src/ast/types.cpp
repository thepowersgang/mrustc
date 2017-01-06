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
#include <ast/crate.hpp>
#include <ast/expr.hpp>

/// Mappings from internal type names to the core type enum
static const struct {
    const char* name;
    enum eCoreType  type;
} CORETYPES[] = {
    // NOTE: Sorted
    {"_", CORETYPE_ANY},
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
    {"str", CORETYPE_STR},
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
    case CORETYPE_INVAL:return "INVAL";
    case CORETYPE_ANY:  return "_";
    case CORETYPE_CHAR: return "char";
    case CORETYPE_STR:  return "str";
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
    m_rettype( box$( other.m_rettype->clone() ) )
{
    for( const auto& at : other.m_arg_types )
        m_arg_types.push_back( at.clone() );
}

Ordering Type_Function::ord(const Type_Function& x) const
{
    Ordering rv;

    rv = ::ord(m_abi, x.m_abi);
    if(rv != OrdEqual)  return rv;
    rv = ::ord(m_arg_types, x.m_arg_types);
    if(rv != OrdEqual)  return rv;
    return (*m_rettype).ord( *x.m_rettype );
}

TypeRef::~TypeRef()
{
}

TypeRef TypeRef::clone() const
{
    struct H {
        static ::std::vector< ::TypeRef> clone_ty_vec(const ::std::vector<TypeRef>& x) {
            ::std::vector<TypeRef>  rv;
            rv.reserve(x.size());
            for(const auto& t : x)
                rv.push_back( t.clone() );
            return rv;
        }
    };
    switch( m_data.tag() )
    {
    case TypeData::TAGDEAD: assert(!"Copying a destructed type");
    #define _COPY(VAR)  case TypeData::TAG_##VAR: return TypeRef(m_span, TypeData::make_##VAR(m_data.as_##VAR()) ); break;
    #define _CLONE(VAR, code...)    case TypeData::TAG_##VAR: { auto& old = m_data.as_##VAR(); return TypeRef(m_span, TypeData::make_##VAR(code) ); } break;
    _COPY(None)
    _COPY(Any)
    _COPY(Bang)
    case TypeData::TAG_Macro:   assert( !"Copying an unexpanded type macro" );
    _COPY(Unit)
    _COPY(Primitive)
    _COPY(Function)
    _CLONE(Tuple, { H::clone_ty_vec(old.inner_types) })
    _CLONE(Borrow,  { old.is_mut, box$(old.inner->clone()) })
    _CLONE(Pointer, { old.is_mut, box$(old.inner->clone()) })
    _CLONE(Array, { box$(old.inner->clone()), old.size })
    _COPY(Generic)
    _COPY(Path)
    _COPY(TraitObject)
    _COPY(ErasedType)
    #undef _COPY
    #undef _CLONE
    }
    throw "";
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
    (Bang, return OrdEqual;),
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
        return ::ord(ent.name, x_ent.name);
        ),
    (Path,
        return ent.path.ord( x_ent.path );
        ),
    (TraitObject,
        return ::ord(ent.traits, x_ent.traits);
        ),
    (ErasedType,
        return ::ord(ent.traits, x_ent.traits);
        )
    )
    throw ::std::runtime_error(format("BUGCHECK - Unhandled TypeRef class '", m_data.tag(), "'"));
}

::std::ostream& operator<<(::std::ostream& os, const eCoreType ct) {
    return os << coretype_name(ct);
}

::std::ostream& operator<<(::std::ostream& os, const TypeRef& tr) {
    //os << "TypeRef(";
    #define _(VAR, ...) case TypeData::TAG_##VAR: { const auto &ent = tr.m_data.as_##VAR(); (void)&ent; __VA_ARGS__ } break;
    switch(tr.m_data.tag())
    {
    case TypeData::TAGDEAD: throw "";
    _(None,
        os << "!!";
        )
    _(Any,
        os << "_";
        )
    _(Bang,
        os << "!";
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
        os << "/* arg */ " << ent.name << "/*"<<ent.index<<"*/";
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
    _(ErasedType,
        os << "impl ";
        for( const auto& it : ent.traits ) {
            if( &it != &ent.traits.front() )
                os << "+";
            os << it;
        }
        os << "";
        )
    }
    #undef _
    //os << ")";
    return os;
}


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
