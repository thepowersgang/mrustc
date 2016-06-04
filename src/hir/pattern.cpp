
#include "pattern.hpp"

namespace HIR {
    ::std::ostream& operator<<(::std::ostream& os, const Pattern::Value& x) {
        TU_MATCH(Pattern::Value, (x), (e),
        (Integer,
            // TODO: Print with type
            os << e.value;
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
    ::std::ostream& operator<<(::std::ostream& os, const Pattern& x) {
        if( x.m_binding.is_valid() ) {
            if( x.m_binding.m_mutable )
                os << "mut ";
            switch(x.m_binding.m_type)
            {
            case PatternBinding::Type::Move:    break;
            case PatternBinding::Type::Ref:     os << "ref ";   break;
            case PatternBinding::Type::MutRef:  os << "ref mut ";   break;
            }
            os << x.m_binding.m_name << " @ ";
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
        (StructTuple,
            os << e.path;
            os << "(";
            for(const auto& s : e.sub_patterns)
                os << s << ", ";
            os << ")";
            ),
        (StructTupleWildcard,
            os << e.path;
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
        
        (EnumTuple,
            os << e.path;
            os << "(";
            for(const auto& s : e.sub_patterns)
                os << s << ", ";
            os << ")";
            ),
        (EnumTupleWildcard,
            os << e.path;
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
            os << "[";
            for(const auto& s : e.leading)
                os << s << ", ";
            if( e.extra_bind.is_valid() ) {
                os << e.extra_bind.m_name << " @ ";
            }
            os << ".. ";
            for(const auto& s : e.trailing)
                os << s << ", ";
            os << "]";
            )
        )
        return os;
    }
}

