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

::std::ostream& operator<<(::std::ostream& os, const Pattern& pat)
{
    os << "Pattern(" << pat.m_binding << " @ ";
    TU_MATCH(Pattern::Data, (pat.m_data), (ent),
    (Any,
        os << "_";
        ),
    (MaybeBind,
        os << "?";
        ),
    (Box,
        os << "box " << *ent.sub;
        ),
    (Ref,
        os << "&" << (ent.mut ? "mut " : "") << *ent.sub;
        ),
    (Value,
        os << *ent.start;
        if( ent.end.get() )
            os << " ... " << *ent.end;
        ),
    (Tuple,
        os << "(" << ent.sub_patterns << ")";
        ),
    (StructTuple,
        os << ent.path << " (" << ent.sub_patterns << ")";
        ),
    (Struct,
        os << ent.path << " {" << ent.sub_patterns << "}";
        )
    )
    os << ")";
    return os;
}
void operator%(Serialiser& s, Pattern::Data::Tag c) {
    s << Pattern::Data::tag_to_str(c);
}
void operator%(::Deserialiser& s, Pattern::Data::Tag& c) {
    ::std::string   n;
    s.item(n);
    c = Pattern::Data::tag_from_str(n);
}
#define _D(VAR, ...)  case Pattern::Data::TAG_##VAR: { m_data = Pattern::Data::make_null_##VAR(); auto& ent = m_data.as_##VAR(); (void)&ent; __VA_ARGS__ } break;
SERIALISE_TYPE(Pattern::, "Pattern", {
    s.item(m_binding);
    s % m_data.tag();
    TU_MATCH(Pattern::Data, (m_data), (e),
    (Any,
        ),
    (MaybeBind,
        ),
    (Box,
        s << e.sub;
        ),
    (Ref,
        s << e.mut;
        s << e.sub;
        ),
    (Value,
        s << e.start;
        s << e.end;
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
    _D(Box,
        s.item( ent.sub );
        )
    _D(Ref,
        s.item( ent.mut );
        s.item( ent.sub );
        )
    _D(Value,
        s.item( ent.start );
        s.item( ent.end );
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
    }
});

}

