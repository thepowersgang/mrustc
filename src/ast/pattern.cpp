/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/pattern.cpp
 * - AST::Pattern support/implementation code
 */
#include "../common.hpp"
#include "ast.hpp"
#include "pattern.hpp"

namespace AST {

::std::ostream& operator<<(::std::ostream& os, const Pattern::Value& val)
{
    TU_MATCH(Pattern::Value, (val), (e),
    (Invalid,
        os << "/*BAD PAT VAL*/";
        ),
    (Integer,
        switch(e.type)
        {
        case CORETYPE_BOOL:
            os << (e.value ? "true" : "false");
            break;
        case CORETYPE_F32:
        case CORETYPE_F64:
            BUG(Span(), "Hit F32/f64 in printing pattern literal");
            break;
        default:
            os << e.value;
            break;
        }
        ),
    (String,
        os << "\"" << e << "\"";
        ),
    (Named,
        os << e;
        )
    )
    return os;
}
::std::ostream& operator<<(::std::ostream& os, const Pattern& pat)
{
    os << "Pattern(";
    if(pat.m_binding != "")
        os << pat.m_binding << " @ ";
    TU_MATCH(Pattern::Data, (pat.m_data), (ent),
    (MaybeBind,
        os << ent.name << "?";
        ),
    (Macro,
        os << *ent.inv;
        ),
    (Any,
        os << "_";
        ),
    (Box,
        os << "box " << *ent.sub;
        ),
    (Ref,
        os << "&" << (ent.mut ? "mut " : "") << *ent.sub;
        ),
    (Value,
        os << ent.start;
        if( ! ent.end.is_Invalid() )
            os << " ... " << ent.end;
        ),
    (Tuple,
        os << "(" << ent.sub_patterns << ")";
        ),
    (StructTuple,
        os << ent.path << " (" << ent.sub_patterns << ")";
        ),
    (Struct,
        os << ent.path << " {" << ent.sub_patterns << "}";
        ),
    (Slice,
        os << "[";
        bool needs_comma = false;
        if(ent.leading.size()) {
            os << ent.leading;
            needs_comma = true;
        }
        if(ent.extra_bind.size() > 0) {
            if( needs_comma ) {
                os << ", ";
            }
            if(ent.extra_bind != "_")
                os << ent.extra_bind;
            os << "..";
            needs_comma = true;
        }
        if(ent.trailing.size()) {
            if( needs_comma ) {
                os << ", ";
            }
            os << ent.trailing;
        }
        os << "]";
        )
    )
    os << ")";
    return os;
}
void operator%(Serialiser& s, Pattern::Value::Tag c) {
    s << Pattern::Value::tag_to_str(c);
}
void operator%(::Deserialiser& s, Pattern::Value::Tag& c) {
    ::std::string   n;
    s.item(n);
    c = Pattern::Value::tag_from_str(n);
}
void operator%(::Serialiser& s, const Pattern::Value& v) {
    s % v.tag();
    TU_MATCH(Pattern::Value, (v), (e),
    (Invalid, ),
    (Integer,
        s % e.type;
        s.item( e.value );
        ),
    (String,
        s.item( e );
        ),
    (Named,
        s.item(e);
        )
    )
}
void operator%(::Deserialiser& s, Pattern::Value& v) {
    Pattern::Value::Tag  tag;
    s % tag;
    switch(tag)
    {
    case Pattern::Value::TAG_Invalid:
        v = Pattern::Value::make_Invalid({});
        break;
    case Pattern::Value::TAG_Integer: {
        enum eCoreType ct;  s % ct;
        uint64_t val;  s.item( val );
        v = Pattern::Value::make_Integer({ct, val});
        break; }
    case Pattern::Value::TAG_String: {
        ::std::string val;
        s.item( val );
        v = Pattern::Value::make_String(val);
        break; }
    case Pattern::Value::TAG_Named: {
        ::AST::Path val;
        s.item( val );
        v = Pattern::Value::make_Named(val);
        break; }
    }
}

void operator%(Serialiser& s, Pattern::Data::Tag c) {
    s << Pattern::Data::tag_to_str(c);
}
void operator%(::Deserialiser& s, Pattern::Data::Tag& c) {
    ::std::string   n;
    s.item(n);
    c = Pattern::Data::tag_from_str(n);
}
#define _D(VAR, ...)  case Pattern::Data::TAG_##VAR: { m_data = Pattern::Data::make_##VAR({}); auto& ent = m_data.as_##VAR(); (void)&ent; __VA_ARGS__ } break;
SERIALISE_TYPE(Pattern::, "Pattern", {
    s.item(m_binding);
    s % m_data.tag();
    TU_MATCH(Pattern::Data, (m_data), (e),
    (Any,
        ),
    (MaybeBind,
        ),
    (Macro,
        s.item( e.inv );
        ),
    (Box,
        s << e.sub;
        ),
    (Ref,
        s << e.mut;
        s << e.sub;
        ),
    (Value,
        s % e.start;
        s % e.end;
        ),
    (Tuple,
        s << e.sub_patterns;
        ),
    (StructTuple,
        s << e.path;
        s << e.sub_patterns;
        ),
    (Struct,
        s << e.path;
        s << e.sub_patterns;
        ),
    (Slice,
        s << e.leading;
        s << e.extra_bind;
        s << e.trailing;
        )
    )
},{
    s.item(m_binding);
    Pattern::Data::Tag  tag;
    s % tag;
    switch(tag)
    {
    _D(Any, )
    _D(MaybeBind,
        )
    _D(Macro,
        s.item( ent.inv );
        )
    _D(Box,
        s.item( ent.sub );
        )
    _D(Ref,
        s.item( ent.mut );
        s.item( ent.sub );
        )
    _D(Value,
        s % ent.start;
        s % ent.end;
        )
    _D(Tuple,
        s.item( ent.sub_patterns );
        )
    _D(StructTuple,
        s.item( ent.path );
        s.item( ent.sub_patterns );
        )
    _D(Struct,
        s.item( ent.path );
        s.item( ent.sub_patterns );
        )
    _D(Slice,
        s.item( ent.leading );
        s.item( ent.extra_bind );
        s.item( ent.trailing );
        )
    }
});

}

