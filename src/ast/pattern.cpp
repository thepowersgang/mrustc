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
    (Float,
        switch(e.type)
        {
        case CORETYPE_BOOL:
            os << (e.value ? "true" : "false");
            break;
        case CORETYPE_ANY:
        case CORETYPE_F32:
        case CORETYPE_F64:
            os << e.value;
            break;
        default:
            BUG(Span(), "Hit integer in printing pattern literal");
            break;
        }
        ),
    (String,
        os << "\"" << e << "\"";
        ),
    (ByteString,
        os << "b\"" << e.v << "\"";
        ),
    (Named,
        os << e;
        )
    )
    return os;
}
::std::ostream& operator<<(::std::ostream& os, const Pattern::TuplePat& val)
{
    if( val.has_wildcard )
    {
        os << val.start;
        os << ".., ";
        os << val.end;
    }
    else
    {
        os << val.start;
        assert(val.end.size() == 0);
    }
    return os;
}
::std::ostream& operator<<(::std::ostream& os, const PatternBinding& pb)
{
    if( pb.m_mutable )
        os << "mut ";
    switch(pb.m_type)
    {
    case PatternBinding::Type::MOVE:    break;
    case PatternBinding::Type::REF:     os << "ref ";   break;
    case PatternBinding::Type::MUTREF:  os << "ref mut ";   break;
    }
    os << pb.m_name;
    return os;
}
::std::ostream& operator<<(::std::ostream& os, const Pattern& pat)
{
    for(const auto& pb : pat.m_bindings) {
        os << pb << " @ ";
    }
    TU_MATCH_HDRA( (pat.m_data), {)
    TU_ARMA(MaybeBind, ent) {
        os << ent.name << "?";
        }
    TU_ARMA(Macro, ent) {
        os << *ent.inv;
        }
    TU_ARMA(Any, ent) {
        os << "_";
        }
    TU_ARMA(Box, ent) {
        os << "box " << *ent.sub;
        }
    TU_ARMA(Ref, ent) {
        os << "&" << (ent.mut ? "mut " : "") << *ent.sub;
        }
    TU_ARMA(Value, ent) {
        os << ent.start;
        if( ! ent.end.is_Invalid() )
            os << " ..= " << ent.end;
        }
    TU_ARMA(ValueLeftInc, ent) {
        os << ent.start << " .. " << ent.end;
        }
    TU_ARMA(Tuple, ent) {
        os << "(" << ent << ")";
        }
    TU_ARMA(StructTuple, ent) {
        os << ent.path << " (" << ent.tup_pat << ")";
        }
    TU_ARMA(Struct, ent) {
        os << ent.path << " {";
        for(const auto& e : ent.sub_patterns) {
            os << e.attrs;
            os << e.name << ": " << e.pat;
            os << ",";
        }
        os << "}";
        if(ent.is_exhaustive)
            os << "..";
        }
    TU_ARMA(Slice, ent) {
        os << "[";
        os << ent.sub_pats;
        os << "]";
        }
    TU_ARMA(SplitSlice, ent) {
        os << "[";
        bool needs_comma = false;
        if(ent.leading.size()) {
            os << ent.leading;
            needs_comma = true;
        }

        if( needs_comma ) {
            os << ", ";
        }
        if( ent.extra_bind.is_valid() )
            os << ent.extra_bind;
        os << "..";
        needs_comma = true;

        if(ent.trailing.size()) {
            if( needs_comma ) {
                os << ", ";
            }
            os << ent.trailing;
        }
        os << "]";
        }
    TU_ARMA(Or, ent) {
        os << "(";
        for(const auto& e : ent)
            os << (&e == &ent.front() ? "" : " | ") << e;
        os << ")";
        }
    }
    return os;
}

Pattern::~Pattern()
{
}

AST::Pattern AST::Pattern::clone() const
{
    AST::Pattern    rv;
    rv.m_span = m_span;
    for(const auto& pb : m_bindings) {
        rv.m_bindings.push_back(pb);
    }

    struct H {
        static ::std::unique_ptr<Pattern> clone_sp(const ::std::unique_ptr<Pattern>& p) {
            return ::std::make_unique<Pattern>( p->clone() );
        }
        static ::std::vector<Pattern> clone_list(const ::std::vector<Pattern>& list) {
            ::std::vector<Pattern>  rv;
            rv.reserve(list.size());
            for(const auto& p : list)
                rv.push_back( p.clone() );
            return rv;
        }
        static TuplePat clone_tup(const TuplePat& p) {
            return TuplePat {
                H::clone_list(p.start),
                p.has_wildcard,
                H::clone_list(p.end)
                };
        }
        static AST::Pattern::Value clone_val(const AST::Pattern::Value& v) {
            TU_MATCH(::AST::Pattern::Value, (v), (e),
            (Invalid, return Value(e);),
            (Integer, return Value(e);),
            (Float, return Value(e);),
            (String, return Value(e);),
            (ByteString, return Value(e);),
            (Named, return Value::make_Named( AST::Path(e) );)
            )
            throw "";
        }
    };

    TU_MATCH_HDRA( (m_data), {)
    TU_ARMA(Any, e) {
        rv.m_data = Data::make_Any(e);
        }
    TU_ARMA(MaybeBind, e) {
        rv.m_data = Data::make_MaybeBind(e);
        }
    TU_ARMA(Macro, e) {
        rv.m_data = Data::make_Macro({ ::std::make_unique<AST::MacroInvocation>( e.inv->clone() ) });
        }
    TU_ARMA(Box, e) {
        rv.m_data = Data::make_Box({ H::clone_sp(e.sub) });
        }
    TU_ARMA(Ref, e) {
        rv.m_data = Data::make_Ref({ e.mut, H::clone_sp(e.sub) });
        }
    TU_ARMA(Value, e) {
        rv.m_data = Data::make_Value({ H::clone_val(e.start), H::clone_val(e.end) });
        }
    TU_ARMA(ValueLeftInc, e) {
        rv.m_data = Data::make_ValueLeftInc({ H::clone_val(e.start), H::clone_val(e.end) });
        }
    TU_ARMA(Tuple, e) {
        rv.m_data = Data::make_Tuple( H::clone_tup(e) );
        }
    TU_ARMA(StructTuple, e) {
        rv.m_data = Data::make_StructTuple({ ::AST::Path(e.path), H::clone_tup(e.tup_pat) });
        }
    TU_ARMA(Struct, e) {
        ::std::vector<AST::StructPatternEntry>  sps;
        for(const auto& sp : e.sub_patterns)
            sps.push_back(AST::StructPatternEntry { sp.attrs.clone(), sp.name, sp.pat.clone() });
        rv.m_data = Data::make_Struct({ ::AST::Path(e.path), mv$(sps) });
        }
    TU_ARMA(Slice, e) {
        rv.m_data = Data::make_Slice({ H::clone_list(e.sub_pats) });
        }
    TU_ARMA(SplitSlice, e) {
        rv.m_data = Data::make_SplitSlice({ H::clone_list(e.leading), e.extra_bind, H::clone_list(e.trailing) });
        }
    TU_ARMA(Or, e) {
        rv.m_data = Data::make_Or( H::clone_list(e) );
        }
    }

    return rv;
}

}   // namespace AST

