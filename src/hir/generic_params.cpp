/*
 */
#include "generic_params.hpp"

namespace HIR {
    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::GenericParams::PrintArgs& x)
    {
        if( x.gp.m_lifetimes.size() > 0 || x.gp.m_types.size() > 0 )
        {
            os << "<";
            for(const auto& lft : x.gp.m_lifetimes) {
                os << "'" << lft << ",";
            }
            for(const auto& typ : x.gp.m_types) {
                os << typ.m_name;
                if( ! typ.m_is_sized )
                    os << ": ?Sized";
                if( !typ.m_default.m_data.is_Infer() )
                    os << " = " << typ.m_default;
                os << ",";
            }
            os << ">";
        }
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::GenericParams::PrintBounds& x)
    {
        if( x.gp.m_bounds.size() > 0 )
        {
            os << " where ";
            bool comma_needed = false;
            for(const auto& b : x.gp.m_bounds)
            {
                if(comma_needed)
                    os << ", ";
                TU_MATCH(::HIR::GenericBound, (b), (e),
                (Lifetime,
                    os << "'" << e.test << ": '" << e.valid_for;
                    ),
                (TypeLifetime,
                    os << e.type << ": '" << e.valid_for;
                    ),
                (TraitBound,
                    os << e.type << ": " << e.trait.m_path;
                    ),
                (TypeEquality,
                    os << e.type << " = " << e.other_type;
                    )
                )
                comma_needed = true;
            }
        }
        return os;
    }
}
