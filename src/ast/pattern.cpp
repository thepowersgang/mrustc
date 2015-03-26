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
    switch(pat.m_data.tag())
    {
    case Pattern::Data::Any:
        os << "_";
        break;
    case Pattern::Data::MaybeBind:
        os << "?";
        break;
    case Pattern::Data::Ref:
        os << "&" << (pat.m_data.as_Ref().mut ? "mut " : "") << *pat.m_data.as_Ref().sub;
        break;
    case Pattern::Data::Value:
        os << *pat.m_data.as_Value().start;
        if( pat.m_data.as_Value().end.get() )
            os << " ... " << *pat.m_data.as_Value().end;
        break;
    case Pattern::Data::Tuple:
        os << "(" << pat.m_data.as_Tuple().sub_patterns << ")";
        break;
    case Pattern::Data::StructTuple:
        os << pat.m_data.as_StructTuple().path << " (" << pat.m_data.as_StructTuple().sub_patterns << ")";
        break;
    case Pattern::Data::Struct:
        os << pat.m_data.as_Struct().path << " {" << pat.m_data.as_Struct().sub_patterns << "}";
        break;
    }
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
SERIALISE_TYPE(Pattern::, "Pattern", {
    s.item(m_binding);
    s % m_data.tag();
    switch(m_data.tag())
    {
    case Pattern::Data::Any:
        break;
    case Pattern::Data::MaybeBind:
        break;
    case Pattern::Data::Ref:
        s << m_data.as_Ref().mut;
        s << m_data.as_Ref().sub;
        break;
    case Pattern::Data::Value:
        s << m_data.as_Value().start;
        s << m_data.as_Value().end;
        break;
    case Pattern::Data::Tuple:
        s << m_data.as_Tuple().sub_patterns;
        break;
    case Pattern::Data::StructTuple:
        s << m_data.as_StructTuple().path;
        s << m_data.as_StructTuple().sub_patterns;
        break;
    case Pattern::Data::Struct:
        s << m_data.as_Struct().path;
        s << m_data.as_Struct().sub_patterns;
        break;
    }
},{
    s.item(m_binding);
    Pattern::Data::Tag  tag;
    s % tag;
    switch(tag)
    {
    case Pattern::Data::Any:
        m_data = Pattern::Data::make_null_Any();
        break;
    case Pattern::Data::MaybeBind:
        m_data = Pattern::Data::make_null_MaybeBind();
        break;
    case Pattern::Data::Ref:
        m_data = Pattern::Data::make_null_Ref();
        s.item( m_data.as_Ref().mut );
        s.item( m_data.as_Ref().sub );
        break;
    case Pattern::Data::Value:
        m_data = Pattern::Data::make_null_Value();
        s.item( m_data.as_Value().start );
        s.item( m_data.as_Value().end );
        break;
    case Pattern::Data::Tuple:
        m_data = Pattern::Data::make_null_Tuple();
        s.item( m_data.as_Tuple().sub_patterns );
        break;
    case Pattern::Data::StructTuple:
        m_data = Pattern::Data::make_null_StructTuple();
        s.item( m_data.as_StructTuple().path );
        s.item( m_data.as_StructTuple().sub_patterns );
        break;
    case Pattern::Data::Struct:
        m_data = Pattern::Data::make_null_Struct();
        s.item( m_data.as_Struct().path );
        s.item( m_data.as_Struct().sub_patterns );
        break;
    }
});

}

