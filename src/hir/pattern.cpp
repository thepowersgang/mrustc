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
        if( x.m_binding.is_valid() ) {
            os << x.m_binding;
        }
        TU_MATCH(Pattern::Data, (x.m_data), (e),
        (Any,
            os << "_";
            ),
        (Box,
            os << "box " << *e.sub;
            ),
        (Ref,
            switch(e.type)
            {
            case BorrowType::Shared:    os << "&";  break;
            case BorrowType::Unique:    os << "&mut ";   break;
            case BorrowType::Owned:     os << "&move ";   break;
            }
            os << *e.sub;
            ),
        (Tuple,
            os << "(";
            for(const auto& s : e.sub_patterns)
                os << s << ", ";
            os << ")";
            ),
        (SplitTuple,
            os << "(";
            for(const auto& s : e.leading)
                os << s << ", ";
            os << ".., ";
            for(const auto& s : e.trailing)
                os << s << ", ";
            os << ")";
            ),
        (StructValue,
            os << e.path;
            ),
        (StructTuple,
            os << e.path;
            os << "(";
            for(const auto& s : e.sub_patterns)
                os << s << ", ";
            os << ")";
            ),
        (Struct,
            os << e.path;
            os << "{ ";
            for(const auto& ns : e.sub_patterns)
                os << ns.first << ": " << ns.second << ", ";
            os << "}";
            ),

        (Value,
            os << e.val;
            ),
        (Range,
            os << e.start << " ... " << e.end;
            ),

        (EnumValue,
            os << e.path;
            ),
        (EnumTuple,
            os << e.path;
            os << "(";
            for(const auto& s : e.sub_patterns)
                os << s << ", ";
            os << ")";
            ),
        (EnumStruct,
            os << e.path;
            os << "{ ";
            for(const auto& ns : e.sub_patterns)
                os << ns.first << ": " << ns.second << ", ";
            os << "}";
            ),
        (Slice,
            os << "[";
            for(const auto& s : e.sub_patterns)
                os << s << ", ";
            os << "]";
            ),
        (SplitSlice,
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
            )
        )
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
    ::std::vector< ::std::pair< ::std::string, ::HIR::Pattern> > clone_pat_fields(const ::std::vector< ::std::pair< ::std::string, ::HIR::Pattern> >& pats) {
        ::std::vector< ::std::pair< ::std::string, ::HIR::Pattern> >    rv;
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

::HIR::Pattern HIR::Pattern::clone() const
{
    TU_MATCH(::HIR::Pattern::Data, (m_data), (e),
    (Any,
        return Pattern(m_binding, Data::make_Any({}));
        ),
    (Box,
        return Pattern(m_binding, Data::make_Box({
            box$( e.sub->clone() )
            }));
        ),
    (Ref,
        return Pattern(m_binding, Data::make_Ref({
            e.type, box$(e.sub->clone())
            }));
        ),
    (Tuple,
        return Pattern(m_binding, Data::make_Tuple({
            clone_pat_vec(e.sub_patterns)
            }));
        ),
    (SplitTuple,
        return Pattern(m_binding, Data::make_SplitTuple({
            clone_pat_vec(e.leading),
            clone_pat_vec(e.trailing),
            e.total_size
            }));
        ),
    (StructValue,
        return Pattern(m_binding, Data::make_StructValue({
            e.path.clone(), e.binding
            }));
        ),
    (StructTuple,
        return Pattern(m_binding, Data::make_StructTuple({
            e.path.clone(),
            e.binding,
            clone_pat_vec(e.sub_patterns)
            }));
        ),
    (Struct,
        return Pattern(m_binding, Data::make_Struct({
            e.path.clone(),
            e.binding,
            clone_pat_fields(e.sub_patterns),
            e.is_exhaustive
            }));
        ),

    (Value,
        return Pattern(m_binding, Data::make_Value({
            clone_patval(e.val)
            }));
        ),
    (Range,
        return Pattern(m_binding, Data::make_Range({
            clone_patval(e.start),
            clone_patval(e.end)
            }));
        ),

    (EnumValue,
        return Pattern(m_binding, Data::make_EnumValue({ e.path.clone(), e.binding_ptr, e.binding_idx }));
        ),
    (EnumTuple,
        return Pattern(m_binding, Data::make_EnumTuple({
            e.path.clone(),
            e.binding_ptr,
            e.binding_idx,
            clone_pat_vec(e.sub_patterns)
            }));
        ),
    (EnumStruct,
        return Pattern(m_binding, Data::make_EnumStruct({
            e.path.clone(),
            e.binding_ptr,
            e.binding_idx,
            clone_pat_fields(e.sub_patterns),
            e.is_exhaustive
            }));
        ),
    (Slice,
        return Pattern(m_binding, Data::make_Slice({
            clone_pat_vec(e.sub_patterns)
            }));
        ),
    (SplitSlice,
        return Pattern(m_binding, Data::make_SplitSlice({
            clone_pat_vec(e.leading),
            e.extra_bind,
            clone_pat_vec(e.trailing)
            }));
        )
    )

    throw "";
}


