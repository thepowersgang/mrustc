/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/resolve_common.cpp
 * - Common components of both "static" and "dynamic" type checking
 */
#include "resolve_common.hpp"
#include "monomorph.hpp"    // MonomorphStatePtr

void TraitResolveCommon::prep_indexes(const Span& sp)
{
    TRACE_FUNCTION_F("");

    if(m_impl_generics) DEBUG("m_impl_generics = " << m_impl_generics->fmt_args() << m_impl_generics->fmt_bounds());
    if(m_item_generics) DEBUG("m_item_generics = " << m_item_generics->fmt_args() << m_item_generics->fmt_bounds());

    m_type_equalities.clear();
    m_trait_bounds.clear();

    this->iterate_bounds([&](const HIR::GenericBound& b)->bool {
        TU_MATCH_HDRA( (b), { )
        default:
            break;
            TU_ARMA(TraitBound, be) {
                this->prep_indexes__add_trait_bound(sp, be.hrtbs.get(), be.type.clone(), be.trait.clone());
            }
            TU_ARMA(TypeEquality, be) {
                DEBUG("Equality - " << be.type << " = " << be.other_type);
                this->prep_indexes__add_equality(sp, nullptr, be.type.clone(), be.other_type.clone());
            }
        }
        return false;
        });
    DEBUG(m_trait_bounds.size() << " trait bounds");
    for(const auto& tb : m_trait_bounds)
    {
        DEBUG(tb.first.first << " : " << tb.first.second << " - " << tb.second.assoc);
    }
}
void TraitResolveCommon::prep_indexes__add_equality(const Span& sp, const ::HIR::GenericParams* hrtbs, ::HIR::TypeRef long_ty, ::HIR::TypeRef short_ty)
{
    DEBUG("ADD " << long_ty << " => " << short_ty);
    if( !hrtbs ) {
        static const HIR::GenericParams empty_hrtbs;
        hrtbs = &empty_hrtbs;
    }
    // TODO: Sort the two types by "complexity" (most of the time long >= short)
    this->m_type_equalities.insert(::std::make_pair( mv$(long_ty), CachedEquality { hrtbs->clone(), mv$(short_ty) } ));
}
void TraitResolveCommon::prep_indexes__add_trait_bound(const Span& sp, const ::HIR::GenericParams* outer_hrtbs, ::HIR::TypeRef type, ::HIR::TraitPath trait_path, bool add_parents/*=true*/)
{
    TRACE_FUNCTION_F(type << " : " << trait_path);

    auto get_or_add_trait_bound = [&](const HIR::GenericParams* hrbs, const HIR::GenericPath& trait_path)->CachedBound& {
        auto it = m_trait_bounds.find(std::make_pair(std::ref(type), std::ref(trait_path)));
        if( it != m_trait_bounds.end() ) {
            DEBUG("[get_or_add_trait_bound] Use " << FMT_CB(os, if(hrbs) os << "for" << hrbs->fmt_args() << " ";) << trait_path);
            return it->second;
        }
        DEBUG("[get_or_add_trait_bound] Add " << FMT_CB(os, if(outer_hrtbs) os << "for" << outer_hrtbs->fmt_args() << " ";) << " ?: " << FMT_CB(os, if(hrbs) os << "for" << hrbs->fmt_args() << " ";) << trait_path);
        auto& rv = m_trait_bounds[std::make_pair(type.clone(), trait_path.clone())];
        if( outer_hrtbs && !outer_hrtbs->is_empty() ) {
            rv.hrbs = outer_hrtbs->clone();
        }
        if( hrbs && !hrbs->is_empty() ) {
            rv.hrbs = hrbs->clone();
        }
        rv.trait_ptr = &m_crate.get_trait_by_path(sp, trait_path.m_path);
        return rv;
    };
    auto push_type = [&](const RcString& name, const HIR::GenericParams* hrbs, const HIR::TraitPath::AtyEqual& atye) {
        auto& b = get_or_add_trait_bound(hrbs, atye.source_trait);
        b.assoc.insert(std::make_pair(name, atye.clone()));
    };

    auto& trait_params = trait_path.m_path.m_params;
    auto monomorph = MonomorphStatePtr(&type, &trait_params, nullptr);

    const auto& trait = m_crate.get_trait_by_path(sp, trait_path.m_path.m_path);
#if 1
    while(trait_params.m_types.size() < trait.m_params.m_types.size()) {
        trait_params.m_types.push_back(monomorph.monomorph_type(sp, trait.m_params.m_types[trait_params.m_types.size()].m_default));
    }
#endif

    get_or_add_trait_bound(trait_path.m_hrtbs.get(), trait_path.m_path);
    for( const auto& tb : trait_path.m_type_bounds )
    {
        DEBUG("Equality (TB) - <" << type << " as " << tb.second.source_trait << ">::" << tb.first << " = " << tb.second);
        push_type(tb.first, trait_path.m_hrtbs.get(), tb.second);

        auto ty_l = ::HIR::TypeRef::new_path( ::HIR::Path( type.clone(), tb.second.source_trait.clone(), tb.first ), ::HIR::TypePathBinding::make_Opaque({}) );
        prep_indexes__add_equality( sp, trait_path.m_hrtbs.get(), mv$(ty_l), tb.second.type.clone() );
    }
    // ATY Trait bounds
    for( const auto& tb : trait_path.m_trait_bounds )
    {
        for(const auto& trait : tb.second.traits) {
            auto ty_l = ::HIR::TypeRef::new_path(
                ::HIR::Path( type.clone(), tb.second.source_trait.clone(), tb.first ),
                ::HIR::TypePathBinding::make_Opaque({})
            );
            DEBUG("Bound (TB) - <" << type << " as " << tb.second.source_trait << ">::" << tb.first << " : " << trait);
            prep_indexes__add_trait_bound(sp, outer_hrtbs, std::move(ty_l), trait.clone());
        }
    }

    for(const auto& a_ty : trait.m_types)
    {
        // if no bounds, don't bother making the type
        if( a_ty.second.m_trait_bounds.empty() )
            continue ;

        auto ty_a = ::HIR::TypeRef::new_path(
            ::HIR::Path( type.clone(), trait_path.m_path.clone(), a_ty.first ),
            ::HIR::TypePathBinding::make_Opaque({})
        );

        for( const auto& a_ty_b : a_ty.second.m_trait_bounds )
        {
            DEBUG("(Assoc) " << a_ty_b);
            auto trait_mono = monomorph.monomorph_traitpath(sp, a_ty_b, false);
            for( auto& tb : trait_mono.m_type_bounds ) {
                DEBUG("Equality (ATB) - <" << ty_a << " as " << tb.second.source_trait << ">::" << tb.first << " = " << tb.second);

                auto ty_l = ::HIR::TypeRef::new_path(
                    ::HIR::Path( ty_a.clone(), tb.second.source_trait.clone(), tb.first ),
                    ::HIR::TypePathBinding::make_Opaque({})
                );

                // TODO: what if `trait_mono` has HRLs too?
                if( outer_hrtbs && trait_mono.m_hrtbs ) {
                    TODO(sp, "Double-layerd HRLs - outer=" << outer_hrtbs->fmt_args() << " and inner=" << trait_mono.m_hrtbs->fmt_args());
                }
                auto* inner_hrtbs = outer_hrtbs ? outer_hrtbs : a_ty_b.m_hrtbs.get();
                prep_indexes__add_equality( sp, inner_hrtbs, mv$(ty_l), std::move(tb.second.type) );
            }
        }
    }

    for(const auto& st : trait.m_all_parent_traits)
    {
        DEBUG("(Parent) " << st);
        prep_indexes__add_trait_bound(sp, outer_hrtbs, type.clone(), monomorph.monomorph_traitpath(sp, st, false), /*add_parents*/false);
    }
}

/// Obtain the type for a given constant parameter
const ::HIR::TypeRef& TraitResolveCommon::get_const_param_type(const Span& sp, unsigned binding) const
{
    const HIR::GenericParams* p;
    switch(binding >> 8)
    {
    case 0: // impl level
        p = m_impl_generics;
        break;
    case 1: // method level
        p = m_item_generics;
        break;
    default:
        TODO(sp, "Typecheck const generics - look up the type");
    }
    auto slot = binding & 0xFF;
    if(!p) {
        if(m_impl_generics) DEBUG("Impl: " << m_impl_generics->fmt_args());
        if(m_item_generics) DEBUG("Item: " << m_item_generics->fmt_args());
    }
    ASSERT_BUG(sp, p, "No generic list for " << (binding>>8) << ":" << slot);
    ASSERT_BUG(sp, slot < p->m_values.size(), "Generic param index out of range");
    return p->m_values.at(slot).m_type;
}
::std::ostream& operator<<(::std::ostream& s, const TraitResolveCommon::CachedEquality& x)
{
    if( !x.hrbs.is_empty() ) {
        s << "for" << x.hrbs.fmt_args() << " ";
    }
    s << x.ty;
    return s;
}
