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
        return true;
        ),
    (Bounded,
        return true;
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
        ),
    (Bounded,
        )
    )
    return false;
}
bool ImplRef::type_is_specialisable(const char* name) const
{
    TU_MATCH(Data, (this->m_data), (e),
    (TraitImpl,
        if( e.impl == nullptr ) {
            // No impl yet? This type is specialisable.
            return true;
        }
        //TODO(Span(), "type_is_specializable - Impl = " << *this << ", Type = " << name);
        auto it = e.impl->m_types.find(name);
        if( it == e.impl->m_types.end() ) {
            TODO(Span(), "Handle missing type in " << *this << ", name = " << name);
            return false;
        }
        return it->second.is_specialisable;
        ),
    (BoundedPtr,
        return false;
        ),
    (Bounded,
        return false;
        )
    )
    throw "";
}

// Returns a closure to monomorphise including placeholders (if present)
::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)> ImplRef::get_cb_monomorph_traitimpl(const Span& sp) const
{
    const auto& e = this->m_data.as_TraitImpl();
    return [this,&e,&sp](const auto& gt)->const ::HIR::TypeRef& {
        const auto& ge = gt.m_data.as_Generic();
        if( ge.binding == 0xFFFF ) {
            // Store (or cache) a monomorphisation of Self, and error if this recurses
            if( e.self_cache == ::HIR::TypeRef() ) {
                e.self_cache = ::HIR::TypeRef::new_diverge();
                e.self_cache = monomorphise_type_with(sp, e.impl->m_type, this->get_cb_monomorph_traitimpl(sp));
            }
            else if( e.self_cache == ::HIR::TypeRef::new_diverge() ) {
                // BUG!
                BUG(sp, "Use of `Self` in expansion of `Self`");
            }
            else {
            }
            return e.self_cache;
        }
        ASSERT_BUG(sp, ge.binding < 256, "Binding in " << gt << " out of range (>=256)");
        ASSERT_BUG(sp, ge.binding < e.params.size(), "Binding in " << gt << " out of range (>= " << e.params.size() << ")");
        if( e.params[ge.binding] ) {
            return *e.params[ge.binding];
        }
        else if( e.params_ph.size() && e.params_ph[ge.binding] != ::HIR::TypeRef() ) {
            return e.params_ph[ge.binding];
        }
        else {
            BUG(sp, "Param #" << ge.binding << " " << ge.name << " isn't constrained for " << *this);
        }
        };
}

::HIR::TypeRef ImplRef::get_impl_type() const
{
    Span    sp;
    TU_MATCH(Data, (this->m_data), (e),
    (TraitImpl,
        if( e.impl == nullptr ) {
            BUG(Span(), "nullptr");
        }
        return monomorphise_type_with(sp, e.impl->m_type, this->get_cb_monomorph_traitimpl(sp));
        ),
    (BoundedPtr,
        return e.type->clone();
        ),
    (Bounded,
        return e.type.clone();
        )
    )
    throw "";
}
::HIR::PathParams ImplRef::get_trait_params() const
{
    Span    sp;
    TU_MATCH(Data, (this->m_data), (e),
    (TraitImpl,
        if( e.impl == nullptr ) {
            BUG(Span(), "nullptr");
        }

        return monomorphise_path_params_with(sp, e.impl->m_trait_args, this->get_cb_monomorph_traitimpl(sp), true);
        ),
    (BoundedPtr,
        return e.trait_args->clone();
        ),
    (Bounded,
        return e.trait_args.clone();
        )
    )
    throw "";
}
::HIR::TypeRef ImplRef::get_trait_ty_param(unsigned int idx) const
{
    Span    sp;
    TU_MATCH(Data, (this->m_data), (e),
    (TraitImpl,
        if( e.impl == nullptr ) {
            BUG(Span(), "nullptr");
        }
        if( idx >= e.impl->m_trait_args.m_types.size() )
            return ::HIR::TypeRef();
        return monomorphise_type_with(sp, e.impl->m_trait_args.m_types[idx], this->get_cb_monomorph_traitimpl(sp), true);
        ),
    (BoundedPtr,
        if( idx >= e.trait_args->m_types.size() )
            return ::HIR::TypeRef();
        return e.trait_args->m_types.at(idx).clone();
        ),
    (Bounded,
        if( idx >= e.trait_args.m_types.size() )
            return ::HIR::TypeRef();
        return e.trait_args.m_types.at(idx).clone();
        )
    )
    throw "";
    TODO(Span(), "");
}

::HIR::TypeRef ImplRef::get_type(const char* name) const
{
    if( !name[0] )
        return ::HIR::TypeRef();
    static Span  sp;
    TU_MATCH(Data, (this->m_data), (e),
    (TraitImpl,
        DEBUG("name=" << name << " " << *this);
        auto it = e.impl->m_types.find(name);
        if( it == e.impl->m_types.end() )
            return ::HIR::TypeRef();
        const ::HIR::TypeRef& tpl_ty = it->second.data;
        if( monomorphise_type_needed(tpl_ty) ) {
            return monomorphise_type_with(sp, tpl_ty, this->get_cb_monomorph_traitimpl(sp));
        }
        else {
            return tpl_ty.clone();
        }
        ),
    (BoundedPtr,
        auto it = e.assoc->find(name);
        if(it == e.assoc->end())
            return ::HIR::TypeRef();
        return it->second.clone();
        ),
    (Bounded,
        auto it = e.assoc.find(name);
        if(it == e.assoc.end())
            return ::HIR::TypeRef();
        return it->second.clone();
        )
    )
    return ::HIR::TypeRef();
}

::std::ostream& operator<<(::std::ostream& os, const ImplRef& x)
{
    TU_MATCH(ImplRef::Data, (x.m_data), (e),
    (TraitImpl,
        if( e.impl == nullptr ) {
            os << "none";
        }
        else {
            os << "impl";
            os << "(" << e.impl << ")";
            if( e.impl->m_params.m_types.size() )
            {
                os << "<";
                for( unsigned int i = 0; i < e.impl->m_params.m_types.size(); i ++ )
                {
                    const auto& ty_d = e.impl->m_params.m_types[i];
                    os << ty_d.m_name;
                    os << ",";
                }
                os << ">";
            }
            os << " " << *e.trait_path << e.impl->m_trait_args << " for " << e.impl->m_type << e.impl->m_params.fmt_bounds();
            os << " {";
            for( unsigned int i = 0; i < e.impl->m_params.m_types.size(); i ++ )
            {
                const auto& ty_d = e.impl->m_params.m_types[i];
                os << ty_d.m_name << " = ";
                if( e.params[i] ) {
                    os << *e.params[i];
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
        ),
    (BoundedPtr,
        os << "bound (ptr) " << *e.type << " : ?" << *e.trait_args << " + {" << *e.assoc << "}";
        ),
    (Bounded,
        os << "bound " << e.type << " : ?" << e.trait_args << " + {"<<e.assoc<<"}";
        )
    )
    return os;
}
