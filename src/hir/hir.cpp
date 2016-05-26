/*
 */
#include "hir.hpp"

namespace HIR {
    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::Literal& v)
    {
        TU_MATCH(::HIR::Literal, (v), (e),
        (Invalid,
            os << "!";
            ),
        (List,
            os << "[";
            for(const auto& val : e)
                os << " " << val << ",";
            os << " ]";
            ),
        (Integer,
            os << e;
            ),
        (Float,
            os << e;
            ),
        (String,
            os << "\"" << e << "\"";
            )
        )
        return os;
    }
}
