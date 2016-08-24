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
::HIR::TypeRef ImplRef::get_impl_type() const
{
    Span    sp;
    TU_MATCH(Data, (this->m_data), (e),
    (TraitImpl,
        if( e.impl == nullptr ) {
            BUG(Span(), "nullptr");
        }
        return monomorphise_type_with(sp, e.impl->m_type, [&e](const auto& t)->const auto& {
            const auto& ge = t.m_data.as_Generic();
            return *e.params.at(ge.binding);
            });
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
        return monomorphise_path_params_with(sp, e.impl->m_trait_args, [&e](const auto& t)->const auto& {
            const auto& ge = t.m_data.as_Generic();
            return *e.params.at(ge.binding);
            }, true);
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
        return monomorphise_type_with(sp, e.impl->m_trait_args.m_types[idx], [&e](const auto& t)->const auto& {
            const auto& ge = t.m_data.as_Generic();
            return *e.params.at(ge.binding);
            }, true);
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
            auto cb_monomorph = [&](const auto& gt)->const auto& {
                const auto& ge = gt.m_data.as_Generic();
                assert(ge.binding < 256);
                assert(ge.binding < e.params.size());
                if( e.params[ge.binding] ) {
                    return *e.params[ge.binding];
                }
                else if( e.params_ph.size() && e.params_ph[ge.binding] != ::HIR::TypeRef() ) {
                    return e.params_ph[ge.binding];
                }
                else {
                    BUG(Span(), "Param #" << ge.binding << " " << ge.name << " isn't constrained for " << *this);
                }
                };
            return monomorphise_type_with(sp, tpl_ty, cb_monomorph);
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
