/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/impl_ref.cpp
 * - Reference to a trait implementation (either a bound or a impl block)
 */
#include "impl_ref.hpp"
#include <hir/hir.hpp>
#include "static.hpp"   // for monomorphise_type_with

bool ImplRef::more_specific_than(const ImplRef& other) const
{
    TU_MATCH(Data, (this->m_data), (te),
    (TraitImpl,
        if( te.impl == nullptr ) {
            return false;
        }
        TU_MATCH(Data, (other.m_data), (oe),
        (TraitImpl,
            if( oe.impl == nullptr ) {
                return true;
            }
            return te.impl->more_specific_than( *oe.impl );
            ),
        (BoundedPtr,
            return false;
            ),
        (Bounded,
            return false;
            )
        )
        ),
    (BoundedPtr,
        if( !other.m_data.is_BoundedPtr() )
            return false;
        const auto& oe = other.m_data.as_BoundedPtr();
        assert( *te.type == *oe.type );
        assert( *te.trait_args == *oe.trait_args );
        if( te.assoc->size() > oe.assoc->size() )
            return true;
        return false;
        ),
    (Bounded,
        if( !other.m_data.is_Bounded() )
            return false;
        const auto& oe = other.m_data.as_Bounded();
        assert( te.type == oe.type );
        assert( te.trait_args == oe.trait_args );
        if( te.assoc.size() > oe.assoc.size() )
            return true;
        return false;
        )
    )
    throw "";
}
bool ImplRef::overlaps_with(const ::HIR::Crate& crate, const ImplRef& other) const
{
    if( this->m_data.tag() != other.m_data.tag() )
        return false;
    TU_MATCH(Data, (this->m_data, other.m_data), (te, oe),
    (TraitImpl,
        if( te.impl != nullptr && oe.impl != nullptr )
            return te.impl->overlaps_with( crate, *oe.impl );
        ),
    (BoundedPtr,
        // TODO: Bounded and BoundedPtr are compatible
        if( *te.type != *oe.type )
            return false;
        if( *te.trait_args != *oe.trait_args )
            return false;
        // Don't check associated types
        return true;
        ),
    (Bounded,
        if( te.type != oe.type )
            return false;
        if( te.trait_args != oe.trait_args )
            return false;
        // Don't check associated types
        return true;
        )
    )
    return false;
}
bool ImplRef::has_magic_params() const
{
    if(const auto* e = m_data.opt_TraitImpl())
    {
        for(const auto& t : e->impl_params.m_types) {
            if( visit_ty_with(t, [](const ::HIR::TypeRef& t){ return t.data().is_Generic() && t.data().as_Generic().is_placeholder(); }) )
                return true;
        }
        for(const auto& v : e->impl_params.m_values) {
            if(v.is_Generic() && v.as_Generic().is_placeholder()) {
                return true;
            }
        }
    }
    return false;
}
bool ImplRef::type_is_specialisable(const char* name) const
{
    TU_MATCH_HDRA( (this->m_data), {)
    TU_ARMA(TraitImpl, e) {
        if( e.impl == nullptr ) {
            // No impl yet? This type is specialisable.
            return true;
        }
        auto it = e.impl->m_types.find(name);
        if( it == e.impl->m_types.end() ) {
            // If not present (which might happen during UFCS resolution), assume that it's not specialisable
            return false;
        }
        return it->second.is_specialisable;
        }
    TU_ARMA(BoundedPtr, e) {
        return false;
        }
    TU_ARMA(Bounded, E) {
        return false;
        }
    }
    throw "";
}

// Returns a closure to monomorphise including placeholders (if present)
ImplRef::Monomorph ImplRef::get_cb_monomorph_traitimpl(const Span& sp, const ::HIR::PathParams& params) const
{
    const auto& e = this->m_data.as_TraitImpl();
    return Monomorph(e, params);
}

::HIR::TypeRef ImplRef::Monomorph::get_type(const Span& sp, const ::HIR::GenericRef& ge) const /*override*/
{
    if( ge.is_self() )
    {
        // Store (or cache) a monomorphisation of Self, and error if this recurses
        if( this->ti.self_cache == ::HIR::TypeRef() ) {
            this->ti.self_cache = ::HIR::TypeRef::new_diverge();
            this->ti.self_cache = this->monomorph_type(sp, this->ti.impl->m_type);
        }
        else if( this->ti.self_cache == ::HIR::TypeRef::new_diverge() ) {
            // BUG!
            BUG(sp, "Use of `Self` in expansion of `Self`");
        }
        else {
        }
        return this->ti.self_cache.clone();
    }
    return MonomorphStatePtr(nullptr, &this->ti.impl_params, &this->params).get_type(sp, ge);
}
::HIR::ConstGeneric ImplRef::Monomorph::get_value(const Span& sp, const ::HIR::GenericRef& val) const /*override*/
{
    return MonomorphStatePtr(nullptr, &this->ti.impl_params, &this->params).get_value(sp, val);
}
::HIR::LifetimeRef ImplRef::Monomorph::get_lifetime(const Span& sp, const ::HIR::GenericRef& g) const /*override*/ {
    return MonomorphStatePtr(nullptr, &this->ti.impl_params, &this->params).get_lifetime(sp, g);
}

::HIR::TypeRef ImplRef::get_impl_type() const
{
    Span    sp;
    TU_MATCH_HDRA( (this->m_data), {)
    TU_ARMA(TraitImpl, e) {
        if( e.impl == nullptr ) {
            BUG(Span(), "nullptr");
        }
        return this->get_cb_monomorph_traitimpl(sp, {}).monomorph_type(sp, e.impl->m_type);
        }
    TU_ARMA(BoundedPtr, e) {
        // HRLs needed?
        return e.type->clone();
        }
    TU_ARMA(Bounded, e) {
        return e.type.clone();
        }
    }
    throw "";
}
::HIR::PathParams ImplRef::get_trait_params() const
{
    Span    sp;
    TU_MATCH_HDRA( (this->m_data), {)
    TU_ARMA(TraitImpl, e) {
        if( e.impl == nullptr ) {
            BUG(Span(), "nullptr");
        }
        return this->get_cb_monomorph_traitimpl(sp, {}).monomorph_path_params(sp, e.impl->m_trait_args, true);
        }
    TU_ARMA(BoundedPtr, e) {
        return MonomorphHrlsOnly(e.hrls ? e.hrls->make_empty_params(true) : HIR::PathParams())
            .monomorph_path_params(sp, *e.trait_args, true);
        }
    TU_ARMA(Bounded, e) {
        return MonomorphHrlsOnly(e.hrls.make_empty_params(true))
            .monomorph_path_params(sp, e.trait_args, true);
        }
    }
    throw "";
}
::HIR::TypeRef ImplRef::get_trait_ty_param(unsigned int idx) const
{
    Span    sp;
    TU_MATCH_HDRA( (this->m_data), {)
    TU_ARMA(TraitImpl, e) {
        if( e.impl == nullptr ) {
            BUG(Span(), "nullptr");
        }
        if( idx >= e.impl->m_trait_args.m_types.size() )
            return ::HIR::TypeRef();
        return this->get_cb_monomorph_traitimpl(sp, {}).monomorph_type(sp, e.impl->m_trait_args.m_types[idx]);
        }
    TU_ARMA(BoundedPtr, e) {
        if( idx >= e.trait_args->m_types.size() )
            return ::HIR::TypeRef();
        return MonomorphHrlsOnly(e.hrls ? e.hrls->make_empty_params(true) : HIR::PathParams())
            .monomorph_type(sp, e.trait_args->m_types.at(idx), true);
        }
    TU_ARMA(Bounded, e) {
        if( idx >= e.trait_args.m_types.size() )
            return ::HIR::TypeRef();
        return MonomorphHrlsOnly(e.hrls.make_empty_params(true))
            .monomorph_type(sp, e.trait_args.m_types.at(idx), true);
        }
    }
    throw "";
}

::HIR::TypeRef ImplRef::get_type(const char* name, const HIR::PathParams& params) const
{
    if( !name[0] )
        return ::HIR::TypeRef();
    static Span  sp;
    TU_MATCH_HDRA( (this->m_data), {)
    TU_ARMA(TraitImpl, e) {
        auto it = e.impl->m_types.find(name);
        if( it == e.impl->m_types.end() )
        {
            static const HIR::TypeRef ty_self = ::HIR::TypeRef::new_self();
            if( e.trait_ptr->m_types.count(name) && e.trait_ptr->m_types.at(name).m_default != HIR::TypeRef() ) {
                // Monomorph twice, first from trait to trait impl, second from trait impl to current
                auto def = MonomorphStatePtr(&ty_self, &e.impl->m_trait_args, nullptr).monomorph_type(sp, e.trait_ptr->m_types.at(name).m_default);
                return this->get_cb_monomorph_traitimpl(sp, params).monomorph_type(sp, def);
            }
            return ::HIR::TypeRef();
        }
        const ::HIR::TypeRef& tpl_ty = it->second.data;
        DEBUG("name=" << name << " tpl_ty=" << tpl_ty << " " << *this);
        if( monomorphise_type_needed(tpl_ty) ) {
            return this->get_cb_monomorph_traitimpl(sp, params).monomorph_type(sp, tpl_ty);
        }
        else {
            return tpl_ty.clone();
        }
        }
    TU_ARMA(BoundedPtr, e) {
        auto it = e.assoc->find(name);
        if(it == e.assoc->end())
            return ::HIR::TypeRef();
        ASSERT_BUG(Span(), !params.has_params(), "TODO: BoundedPtr with params?");
        return MonomorphHrlsOnly(e.hrls ? e.hrls->make_empty_params(true) : HIR::PathParams())
            .monomorph_type(sp, it->second.type, true);
        }
    TU_ARMA(Bounded, e) {
        auto it = e.assoc.find(name);
        if(it == e.assoc.end())
            return ::HIR::TypeRef();
        ASSERT_BUG(Span(), !params.has_params(), "TODO: BoundedPtr with params?");
        return MonomorphHrlsOnly(e.hrls.make_empty_params(true))
            .monomorph_type(sp, it->second.type, true);
        }
    }
    return ::HIR::TypeRef();
}

::std::ostream& operator<<(::std::ostream& os, const ImplRef& x)
{
    TU_MATCH_HDR( (x.m_data), { )
    TU_ARM(x.m_data, TraitImpl, e) {
        if( e.impl == nullptr ) {
            os << "none";
        }
        else {
            os << "impl";
            os << "(" << e.impl << ")";
            os << e.impl->m_params.fmt_args();
            os << " " << *e.trait_path << e.impl->m_trait_args << " for " << e.impl->m_type << e.impl->m_params.fmt_bounds();
            os << " {";
            for( unsigned int i = 0; i < e.impl->m_params.m_lifetimes.size(); i ++ ) {
                const auto& d = e.impl->m_params.m_lifetimes[i];
                os << d.m_name << " = ";
                if( e.impl_params.m_lifetimes[i] != HIR::LifetimeRef() ) {
                    os << e.impl_params.m_lifetimes[i];
                }
                else {
                    os << "?";
                }
                os << ",";
            }
            for( unsigned int i = 0; i < e.impl->m_params.m_types.size(); i ++ )
            {
                const auto& ty_d = e.impl->m_params.m_types[i];
                os << ty_d.m_name << " = ";
                if( e.impl_params.m_types[i] != HIR::TypeRef() ) {
                    os << e.impl_params.m_types[i];
                }
                else {
                    os << "?";
                }
                os << ",";
            }
            for( unsigned int i = 0; i < e.impl->m_params.m_values.size(); i ++ ) {
                const auto& d = e.impl->m_params.m_values[i];
                os << d.m_name << " = ";
                if( e.impl_params.m_values[i] != HIR::ConstGeneric() ) {
                    os << e.impl_params.m_values[i];
                }
                else {
                    os << "?";
                }
                os << ",";
            }
            for(const auto& aty : e.impl->m_types)
            {
                os << "Self::" << aty.first << " = " << aty.second.data << ",";
            }
            os << "}";
        }
        }
    TU_ARM(x.m_data, BoundedPtr, e) {
        assert(e.type);
        assert(e.trait_args);
        assert(e.assoc);
        os << "bound (ptr) " << *e.type << " : ?" << *e.trait_args << " + {" << *e.assoc << "}";
        }
    TU_ARM(x.m_data, Bounded, e) {
        os << "bound " << e.type << " : ?" << e.trait_args << " + {"<<e.assoc<<"}";
        }
    }
    return os;
}
