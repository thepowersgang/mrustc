
#include "static.hpp"

bool StaticTraitResolve::find_impl(
    const Span& sp,
    const ::HIR::Crate& crate, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& trait_params,
    const ::HIR::TypeRef& type,
    t_cb_find_impl found_cb
    )
{
    auto cb_ident = [](const auto&ty)->const auto&{return ty;};
    return crate.find_trait_impls(trait_path, type, cb_ident, [&](const auto& impl) {
        DEBUG("TODO: impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type << " where " << impl.m_params.fmt_bounds());
        
        ::std::vector< const ::HIR::TypeRef*> impl_params;
        impl_params.resize( impl.m_params.m_types.size() );
        
        auto cb = [&impl_params,&sp,cb_ident](auto idx, const auto& ty) {
            assert( idx < impl_params.size() );
            if( ! impl_params[idx] ) {
                impl_params[idx] = &ty;
                return ::HIR::Compare::Equal;
            }
            else {
                return impl_params[idx]->compare_with_placeholders(sp, ty, cb_ident);
            }
            };
        auto match = impl.m_type.match_test_generics_fuzz(sp, type, cb_ident, cb);
        assert( trait_params.m_types.size() == impl.m_trait_args.m_types.size() );
        for( unsigned int i = 0; i < impl.m_trait_args.m_types.size(); i ++ )
        {
            const auto& l = impl.m_trait_args.m_types[i];
            const auto& r = trait_params.m_types[i];
            match &= l.match_test_generics_fuzz(sp, r, cb_ident, cb);
        }
        if( match != ::HIR::Compare::Equal )
            return false;
        
        ::std::vector< ::HIR::TypeRef>  placeholders;
        for(unsigned int i = 0; i < impl_params.size(); i ++ ) {
            if( !impl_params[i] ) {
                if( placeholders.size() == 0 )
                    placeholders.resize(impl_params.size());
                placeholders[i] = ::HIR::TypeRef("impl_?", 2*256 + i);
            }
        }
        auto cb_match = [&](unsigned int idx, const auto& ty) {
            if( ty.m_data.is_Generic() && ty.m_data.as_Generic().binding == idx )
                return ::HIR::Compare::Equal;
            if( idx >> 8 == 2 ) {
                auto i = idx % 256;
                ASSERT_BUG(sp, !impl_params[i], "Placeholder to populated type returned");
                auto& ph = placeholders[i];
                if( ph.m_data.is_Generic() && ph.m_data.as_Generic().binding == idx ) {
                    DEBUG("Bind placeholder " << i << " to " << ty);
                    ph = ty.clone();
                    return ::HIR::Compare::Equal;
                }
                else {
                    TODO(sp, "Compare placeholder " << i << " " << ph << " == " << ty);
                }
            }
            else {
                return ::HIR::Compare::Unequal;
            }
            };
        auto cb_monomorph = [&](const auto& gt)->const auto& {
                const auto& ge = gt.m_data.as_Generic();
                ASSERT_BUG(sp, ge.binding >> 8 != 2, "");
                assert( ge.binding < impl_params.size() );
                if( !impl_params[ge.binding] ) {
                    return placeholders[ge.binding];
                }
                return *impl_params[ge.binding];
                };
        
        // Bounds
        for(const auto& bound : impl.m_params.m_bounds) {
            TU_MATCH_DEF(::HIR::GenericBound, (bound), (e),
            (
                ),
            (TraitBound,
                DEBUG("TODO: Trait bound " << e.type << " : " << e.trait);
                auto b_ty_mono = monomorphise_type_with(sp, e.type, cb_mono);
                DEBUG("- b_ty_mono = " << b_ty_mono);
                )
            )
        }
        
        return found_cb(impl_params, impl);
        });
}

