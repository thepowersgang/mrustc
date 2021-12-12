/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/pattern.cpp
 * - HIR Representation of patterns
 */
#include "pattern.hpp"

namespace HIR {
    ::std::ostream& operator<<(::std::ostream& os, const Pattern::Value& x) {
        TU_MATCH(Pattern::Value, (x), (e),
        (Integer,
            // TODO: Print with type (and signed-ness)
            os << e.value;
            ),
        (Float,
            // TODO: Print with type
            os << e.value;
            ),
        (String,
            os << "\"" << e << "\"";
            ),
        (ByteString,
            os << "b\"" << e.v << "\"";
            ),
        (Named,
            os << e.path;
            )
        )
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const PatternBinding& x) {
        if( x.m_mutable )
            os << "mut ";
        switch(x.m_type)
        {
        case PatternBinding::Type::Move:    break;
        case PatternBinding::Type::Ref:     os << "ref ";   break;
        case PatternBinding::Type::MutRef:  os << "ref mut ";   break;
        }
        os << x.m_name << "/*"<<x.m_slot<<"*/" << " @ ";
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const Pattern& x) {
        for(const auto& pb : x.m_bindings) {
            os << pb;
        }
        if( x.m_implicit_deref_count > 0 ) {
            os << "&*" << x.m_implicit_deref_count;
        }
        TU_MATCH_HDRA( (x.m_data), {)
        TU_ARMA(Any, e) {
            os << "_";
            }
        TU_ARMA(Box, e) {
            os << "box " << *e.sub;
            }
        TU_ARMA(Ref, e) {
            switch(e.type)
            {
            case BorrowType::Shared:    os << "&";  break;
            case BorrowType::Unique:    os << "&mut ";   break;
            case BorrowType::Owned:     os << "&move ";   break;
            }
            os << *e.sub;
            }
        TU_ARMA(Tuple, e) {
            os << "(";
            for(const auto& s : e.sub_patterns)
                os << s << ", ";
            os << ")";
            }
        TU_ARMA(SplitTuple, e) {
            os << "(";
            for(const auto& s : e.leading)
                os << s << ", ";
            os << ".., ";
            for(const auto& s : e.trailing)
                os << s << ", ";
            os << ")";
            }
        TU_ARMA(PathValue, e) {
            os << e.path;
            }
        TU_ARMA(PathTuple, e) {
            os << e.path;
            os << "(";
            for(const auto& s : e.leading)
                os << s << ", ";
            if(e.is_split)
            {
                os << "..";
                for(const auto& s : e.trailing)
                    os << ", " << s;
            }
            os << ")";
            }
        TU_ARMA(PathNamed, e) {
            os << e.path;
            os << "{ ";
            for(const auto& ns : e.sub_patterns)
                os << ns.first << ": " << ns.second << ", ";
            os << "}";
            }

        TU_ARMA(Value, e) {
            os << e.val;
            }
        TU_ARMA(Range, e) {
            if(e.start) os << *e.start;
            os << " .." << (e.is_inclusive ? "=" : "") << " ";
            if(e.end)   os << *e.end;
            }

        TU_ARMA(Slice, e) {
            os << "[";
            for(const auto& s : e.sub_patterns)
                os << s << ", ";
            os << "]";
            }
        TU_ARMA(SplitSlice, e) {
            os << "[ ";
            for(const auto& s : e.leading)
                os << s << ", ";
            if( e.extra_bind.is_valid() ) {
                os << e.extra_bind;
            }
            os << "..";
            for(const auto& s : e.trailing)
                os << ", " << s;
            os << " ]";
            }
        TU_ARMA(Or, e) {
            os << "(";
            for(size_t i = 0; i < e.size(); i ++)
            {
                if(i != 0)
                    os << "|";
                os << e[i];
            }
            os << ")";
            }
        }
        return os;
    }
}   // namespace HIR


namespace {
    ::std::vector< ::HIR::Pattern> clone_pat_vec(const ::std::vector< ::HIR::Pattern>& pats) {
        ::std::vector< ::HIR::Pattern>  rv;
        rv.reserve( pats.size() );
        for(const auto& pat : pats)
            rv.push_back( pat.clone() );
        return rv;
    }
    typedef ::std::vector< ::std::pair< RcString, ::HIR::Pattern> > pat_fields_t;
    pat_fields_t clone_pat_fields(const pat_fields_t& pats) {
        pat_fields_t    rv;
        rv.reserve( pats.size() );
        for(const auto& field : pats)
            rv.push_back( ::std::make_pair(field.first, field.second.clone()) );
        return rv;
    }

    ::HIR::Pattern::Value clone_patval(const ::HIR::Pattern::Value& val) {
        TU_MATCH(::HIR::Pattern::Value, (val), (e),
        (Integer,
            return ::HIR::Pattern::Value::make_Integer(e);
            ),
        (Float,
            return ::HIR::Pattern::Value::make_Float(e);
            ),
        (String,
            return ::HIR::Pattern::Value::make_String(e);
            ),
        (ByteString,
            return ::HIR::Pattern::Value(e);
            ),
        (Named,
            return ::HIR::Pattern::Value::make_Named({ e.path.clone(), e.binding });
            )
        )
        throw "";
    }
}   // namespace

namespace { ::HIR::Pattern::Data clone_pattern_data(const ::HIR::Pattern::Data& m_data)
{
    TU_MATCH_HDRA( (m_data), {)
    TU_ARMA(Any, e) {
        return ::HIR::Pattern::Data::make_Any({});
        }
    TU_ARMA(Box, e) {
        return ::HIR::Pattern::Data::make_Box({
            box$( e.sub->clone() )
            });
        }
    TU_ARMA(Ref, e) {
        return ::HIR::Pattern::Data::make_Ref({
            e.type, box$(e.sub->clone())
            });
        }
    TU_ARMA(Tuple, e) {
        return ::HIR::Pattern::Data::make_Tuple({
            clone_pat_vec(e.sub_patterns)
            });
        }
    TU_ARMA(SplitTuple, e) {
        return ::HIR::Pattern::Data::make_SplitTuple({
            clone_pat_vec(e.leading),
            clone_pat_vec(e.trailing),
            e.total_size
            });
        }
    TU_ARMA(PathValue, e) {
        return ::HIR::Pattern::Data::make_PathValue({
            e.path.clone(), e.binding.clone()
            });
        }
    TU_ARMA(PathTuple, e) {
        return ::HIR::Pattern::Data::make_PathTuple({
            e.path.clone(),
            e.binding.clone(),
            clone_pat_vec(e.leading),
            e.is_split,
            clone_pat_vec(e.trailing),
            e.total_size
            });
        }
    TU_ARMA(PathNamed, e) {
        return ::HIR::Pattern::Data::make_PathNamed({
            e.path.clone(),
            e.binding.clone(),
            clone_pat_fields(e.sub_patterns),
            e.is_exhaustive
            });
        }

    TU_ARMA(Value, e) {
        return ::HIR::Pattern::Data::make_Value({
            clone_patval(e.val)
            });
        }
    TU_ARMA(Range, e) {
        return ::HIR::Pattern::Data::make_Range({
            box$(clone_patval(*e.start)),
            box$(clone_patval(*e.end)),
            e.is_inclusive
            });
        }

    TU_ARMA(Slice, e) {
        return ::HIR::Pattern::Data::make_Slice({
            clone_pat_vec(e.sub_patterns)
            });
        }
    TU_ARMA(SplitSlice, e) {
        return ::HIR::Pattern::Data::make_SplitSlice({
            clone_pat_vec(e.leading),
            e.extra_bind,
            clone_pat_vec(e.trailing)
            });
        }
    TU_ARMA(Or, e)
        return clone_pat_vec(e);
    }

    throw "";
} }
::HIR::Pattern HIR::Pattern::clone() const
{
    auto rv = Pattern(m_bindings, clone_pattern_data(m_data));
    rv.m_implicit_deref_count = m_implicit_deref_count;
    return rv;
}


