/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/generic_params.hpp
 * - HIR version of generic definition blocks
 */
#include "generic_params.hpp"
#include "type.hpp"

namespace HIR {
    ::std::ostream& operator<<(::std::ostream& os, const GenericBound& x)
    {
        TU_MATCH(::HIR::GenericBound, (x), (e),
        (Lifetime,
            os << e.test << ": " << e.valid_for;
            ),
        (TypeLifetime,
            os << e.type << ": " << e.valid_for;
            ),
        (TraitBound,
            os << e.type << ": " << e.trait/*.m_path*/;
            ),
        (TypeEquality,
            os << e.type << " = " << e.other_type;
            )
        )
        return os;
    }

    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::GenericParams::PrintArgs& x)
    {
        if( x.gp.m_lifetimes.size() > 0 || x.gp.m_types.size() > 0 || x.gp.m_values.size() > 0 )
        {
            os << "<";
            for(const auto& lft : x.gp.m_lifetimes) {
                os << "'" << lft.m_name << ",";
            }
            for(const auto& typ : x.gp.m_types) {
                os << typ.m_name;
                if( ! typ.m_is_sized )
                    os << ": ?Sized";
                if( !typ.m_default.data().is_Infer() )
                    os << " = " << typ.m_default;
                os << ",";
            }
            if(!x.gp.m_values.empty())
                os << "const ";
            for(const auto& val_p : x.gp.m_values) {
                os << val_p.m_name;
                os << ": ";
                os << val_p.m_type;
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
                os << b;
                comma_needed = true;
            }
        }
        return os;
    }
}
Ordering HIR::GenericBound::ord(const HIR::GenericBound& b) const
{
    if( this->tag() != b.tag() )
        return this->tag() < b.tag() ? OrdLess : OrdGreater;
    TU_MATCHA( (*this,b), (ae,be),
    (Lifetime,
        auto cmp = ::ord( ae.test, be.test );
        if(cmp != OrdEqual) return cmp;
        cmp = ::ord( ae.valid_for, be.valid_for );
        if(cmp != OrdEqual) return cmp;
        ),
    (TypeLifetime,
        auto cmp = ae.type.ord( be.type );
        if(cmp != OrdEqual) return cmp;
        cmp = ::ord( ae.valid_for, be.valid_for );
        if(cmp != OrdEqual) return cmp;
        ),
    (TraitBound,
        auto cmp = ae.type.ord( be.type );
        if(cmp != OrdEqual) return cmp;
        cmp = ae.trait.ord( be.trait );
        if(cmp != OrdEqual) return cmp;
        ),
    (TypeEquality,
        auto cmp = ae.type.ord( be.type );
        if(cmp != OrdEqual) return cmp;
        cmp = ae.other_type.ord( be.other_type );
        if(cmp != OrdEqual) return cmp;
        )
    )
    return OrdEqual;
}


HIR::PathParams HIR::GenericParams::make_nop_params(unsigned level, bool lifetimes_only/*=false*/) const
{
    HIR::PathParams rv;
    for(const auto& t : this->m_lifetimes) {
        rv.m_lifetimes.push_back(HIR::LifetimeRef( 256*level + rv.m_lifetimes.size() ));
    }
    assert( !lifetimes_only || this->m_types .empty() );
    assert( !lifetimes_only || this->m_values.empty() );
    for(const auto& t : this->m_types) {
        rv.m_types.push_back(HIR::TypeRef(t.m_name, 256*level + rv.m_types.size()));
    }
    for(const auto& t : this->m_values) {
        rv.m_values.push_back(HIR::GenericRef(t.m_name, 256*level + rv.m_values.size()));
    }
    return rv;
}

::HIR::GenericParams HIR::GenericParams::clone() const
{
    ::HIR::GenericParams    rv;
    rv.m_types.reserve(m_types.size());
    for(const auto& type : m_types)
    {
        rv.m_types.push_back(::HIR::TypeParamDef {
            type.m_name,
            type.m_default.clone(),
            type.m_is_sized
            });
    }
    rv.m_values.reserve(m_values.size());
    for(const auto& type : m_values)
    {
        rv.m_values.push_back(::HIR::ValueParamDef {
            type.m_name,
            type.m_type.clone()
            });
    }
    rv.m_lifetimes = m_lifetimes;
    rv.m_bounds.reserve(m_bounds.size());
    for(const auto& bound : m_bounds)
    {
        rv.m_bounds.push_back(bound.clone());
    }
    return rv;
}

::HIR::GenericBound HIR::GenericBound::clone() const
{
    TU_MATCH_HDRA( (*this), {)
    TU_ARMA(Lifetime, e) {
        return ::HIR::GenericBound::make_Lifetime(e);
        }
    TU_ARMA(TypeLifetime, e) {
        return ::HIR::GenericBound::make_TypeLifetime({
            e.type.clone(),
            e.valid_for
            });
        }
    TU_ARMA(TraitBound, e) {
        return ::HIR::GenericBound::make_TraitBound({
            e.hrtbs ? box$(e.hrtbs->clone()) : nullptr,
            e.type.clone(),
            e.trait.clone()
            });
        }/*
    TU_ARMA(NotTrait, e) {
        return ::HIR::GenericBound::make_NotTrait({
            e.type.clone(),
            e.trait.clone()
            });
        }*/
    TU_ARMA(TypeEquality, e) {
        return ::HIR::GenericBound::make_TypeEquality({
            e.type.clone(),
            e.other_type.clone()
            });
        }
    }
    throw "Unreachable";
}
