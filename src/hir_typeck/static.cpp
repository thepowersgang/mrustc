/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/static.cpp
 * - Non-inferred type checking
 */
#include "static.hpp"
#include <algorithm>
#include <hir/expr.hpp>

void StaticTraitResolve::prep_indexes()
{
    static Span sp_AAA;
    const Span& sp = sp_AAA;

    TRACE_FUNCTION_F("");

    m_copy_cache.clear();

    auto add_equality = [&](::HIR::TypeRef long_ty, ::HIR::TypeRef short_ty){
        DEBUG("[prep_indexes] ADD " << long_ty << " => " << short_ty);
        // TODO: Sort the two types by "complexity" (most of the time long >= short)
        this->m_type_equalities.insert(::std::make_pair( mv$(long_ty), mv$(short_ty) ));
        };

    this->iterate_bounds([&](const auto& b)->bool {
        TU_MATCH_DEF(::HIR::GenericBound, (b), (be),
        (
            ),
        (TraitBound,
            DEBUG("[prep_indexes] `" << be.type << " : " << be.trait);
            for( const auto& tb : be.trait.m_type_bounds ) {
                DEBUG("[prep_indexes] Equality (TB) - <" << be.type << " as " << be.trait.m_path << ">::" << tb.first << " = " << tb.second);
                auto ty_l = ::HIR::TypeRef( ::HIR::Path( be.type.clone(), be.trait.m_path.clone(), tb.first ) );
                ty_l.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});

                add_equality( mv$(ty_l), tb.second.clone() );
            }

            const auto& trait_params = be.trait.m_path.m_params;
            auto cb_mono = [&](const auto& ty)->const ::HIR::TypeRef& {
                const auto& ge = ty.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    return be.type;
                }
                else if( ge.binding < 256 ) {
                    unsigned idx = ge.binding % 256;
                    ASSERT_BUG(sp, idx < trait_params.m_types.size(), "Generic binding out of range in trait " << be.trait);
                    return trait_params.m_types[idx];
                }
                else {
                    BUG(sp, "Unknown generic binding " << ty);
                }
                };

            const auto& trait = m_crate.get_trait_by_path(sp, be.trait.m_path.m_path);
            for(const auto& a_ty : trait.m_types)
            {
                ::HIR::TypeRef ty_a;
                for( const auto& a_ty_b : a_ty.second.m_trait_bounds ) {
                    DEBUG("[prep_indexes] (Assoc) " << a_ty_b);
                    auto trait_mono = monomorphise_traitpath_with(sp, a_ty_b, cb_mono, false);
                    for( auto& tb : trait_mono.m_type_bounds ) {
                        if( ty_a == ::HIR::TypeRef() ) {
                            ty_a = ::HIR::TypeRef( ::HIR::Path( be.type.clone(), be.trait.m_path.clone(), a_ty.first ) );
                            ty_a.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                        }
                        DEBUG("[prep_indexes] Equality (ATB) - <" << ty_a << " as " << a_ty_b.m_path << ">::" << tb.first << " = " << tb.second);

                        auto ty_l = ::HIR::TypeRef( ::HIR::Path( ty_a.clone(), trait_mono.m_path.clone(), tb.first ) );
                        ty_l.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});

                        add_equality( mv$(ty_l), mv$(tb.second) );
                    }
                }
            }
            ),
        (TypeEquality,
            DEBUG("Equality - " << be.type << " = " << be.other_type);
            add_equality( be.type.clone(), be.other_type.clone() );
            )
        )
        return false;
        });
}

const ::HIR::TypeRef& StaticTraitResolve::get_const_param_type(const Span& sp, unsigned binding) const
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
    ASSERT_BUG(sp, p, "No generic list");
    ASSERT_BUG(sp, slot < p->m_values.size(), "Generic param index out of range");
    return p->m_values.at(slot).m_type;
}

bool StaticTraitResolve::find_impl(
    const Span& sp,
    const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
    const ::HIR::TypeRef& type,
    t_cb_find_impl found_cb,
    bool dont_handoff_to_specialised
    ) const
{
    TRACE_FUNCTION_F(trait_path << FMT_CB(os, if(trait_params) { os << *trait_params; } else { os << "<?>"; }) << " for " << type);
    auto cb_ident = [](const ::HIR::TypeRef&ty)->const ::HIR::TypeRef& { return ty; };

    static ::HIR::PathParams    null_params;
    static ::std::map<RcString, ::HIR::TypeRef>    null_assoc;

    if( !dont_handoff_to_specialised ) {
        if( trait_path == m_lang_Copy ) {
            if( this->type_is_copy(sp, type) ) {
                return found_cb( ImplRef(&type, &null_params, &null_assoc), false );
            }
        }
        else if( TARGETVER_1_29 && trait_path == m_lang_Clone ) {
            // NOTE: Duplicated check for enumerate
            if( type.m_data.is_Tuple() || type.m_data.is_Array() || type.m_data.is_Function() || type.m_data.is_Closure()
                    || TU_TEST1(type.m_data, Path, .is_closure()) )
            {
                if( this->type_is_clone(sp, type) ) {
                    return found_cb( ImplRef(&type, &null_params, &null_assoc), false );
                }
            }
        }
        else if( trait_path == m_lang_Sized ) {
            if( this->type_is_sized(sp, type) ) {
                return found_cb( ImplRef(&type, &null_params, &null_assoc), false );
            }
        }
        else if( trait_path == m_lang_Unsize ) {
            ASSERT_BUG(sp, trait_params, "TODO: Support no params for Unzie");
            const auto& dst_ty = trait_params->m_types.at(0);
            if( this->can_unsize(sp, dst_ty, type) ) {
                return found_cb( ImplRef(&type, trait_params, &null_assoc), false );
            }
        }
    }

    if(const auto* e = type.m_data.opt_Generic() )
    {
        if( (e->binding >> 8) == 2 )
        {
            // TODO: If the type is a magic placeholder, assume it impls the specified trait.
            // TODO: Restructure so this knows that the placehlder impls the impl-provided bounds.
            return found_cb( ImplRef(&type, trait_params, &null_assoc), false );
        }
    }

    // --- MAGIC IMPLS ---
    // TODO: There should be quite a few more here, but laziness
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Function, e,
        if( trait_path == m_lang_Fn || trait_path == m_lang_FnMut || trait_path == m_lang_FnOnce ) {
            if( trait_params )
            {
                const auto& des_arg_tys = trait_params->m_types.at(0).m_data.as_Tuple();
                if( des_arg_tys.size() != e.m_arg_types.size() ) {
                    return false;
                }
                for(unsigned int i = 0; i < des_arg_tys.size(); i ++)
                {
                    if( des_arg_tys[i] != e.m_arg_types[i] ) {
                        return false;
                    }
                }
            }
            else
            {
                trait_params = &null_params;
            }
            ::std::map< RcString, ::HIR::TypeRef>  assoc;
            assoc.insert( ::std::make_pair("Output", e.m_rettype->clone()) );
            return found_cb( ImplRef(type.clone(), trait_params->clone(), mv$(assoc)), false );
        }
    )
    if(const auto* e = type.m_data.opt_Closure())
    {
        if( trait_path == m_lang_Fn || trait_path == m_lang_FnMut || trait_path == m_lang_FnOnce )
        {
            if( trait_params )
            {
                const auto& des_arg_tys = trait_params->m_types.at(0).m_data.as_Tuple();
                if( des_arg_tys.size() != e->m_arg_types.size() ) {
                    return false;
                }
                for(unsigned int i = 0; i < des_arg_tys.size(); i ++)
                {
                    if( des_arg_tys[i] != e->m_arg_types[i] ) {
                        return false;
                    }
                }
            }
            else
            {
                trait_params = &null_params;
            }
            switch( e->node->m_class )
            {
            case ::HIR::ExprNode_Closure::Class::Unknown:
                break;
            case ::HIR::ExprNode_Closure::Class::NoCapture:
                break;
            case ::HIR::ExprNode_Closure::Class::Once:
                if( trait_path == m_lang_FnMut )
                    return false;
            case ::HIR::ExprNode_Closure::Class::Mut:
                if( trait_path == m_lang_Fn )
                    return false;
            case ::HIR::ExprNode_Closure::Class::Shared:
                break;
            }
            ::std::map< RcString, ::HIR::TypeRef>  assoc;
            assoc.insert( ::std::make_pair("Output", e->m_rettype->clone()) );
            return found_cb( ImplRef(type.clone(), trait_params->clone(), mv$(assoc)), false );
        }
    }

    // ----
    // TraitObject traits and supertraits
    // ----
    TU_IFLET( ::HIR::TypeRef::Data, type.m_data, TraitObject, e,
        if( trait_path == e.m_trait.m_path.m_path )
        {
            if( !trait_params || e.m_trait.m_path.m_params == *trait_params )
            {
                return found_cb( ImplRef(&type, &e.m_trait.m_path.m_params, &e.m_trait.m_type_bounds), false );
            }
        }
        // Markers too
        for( const auto& mt : e.m_markers )
        {
            if( trait_path == mt.m_path ) {
                if( !trait_params || mt.m_params == *trait_params )
                {
                    static ::std::map< RcString, ::HIR::TypeRef>  types;
                    return found_cb( ImplRef(&type, &mt.m_params, &types), false );
                }
            }
        }

        // - Check if the desired trait is a supertrait of this.
        // TODO: What if `trait_params` is nullptr?
        bool rv = false;
        bool is_supertrait = trait_params && this->find_named_trait_in_trait(sp, trait_path,*trait_params, *e.m_trait.m_trait_ptr, e.m_trait.m_path.m_path,e.m_trait.m_path.m_params, type,
            [&](const auto& i_params, const auto& i_assoc) {
                // Invoke callback with a proper ImplRef
                ::std::map< RcString, ::HIR::TypeRef> assoc_clone;
                for(const auto& e : i_assoc)
                    assoc_clone.insert( ::std::make_pair(e.first, e.second.clone()) );
                // HACK! Just add all the associated type bounds (only inserted if not already present)
                for(const auto& e2 : e.m_trait.m_type_bounds)
                    assoc_clone.insert( ::std::make_pair(e2.first, e2.second.clone()) );
                auto ir = ImplRef(type.clone(), i_params.clone(), mv$(assoc_clone));
                DEBUG("- ir = " << ir);
                rv = found_cb( mv$(ir), false );
                return false;
            });
        if( is_supertrait )
        {
            return rv;
        }
    )
    // --- / ---
    TU_IFLET( ::HIR::TypeRef::Data, type.m_data, ErasedType, e,
        for(const auto& trait : e.m_traits)
        {
            if( trait_path == trait.m_path.m_path && (!trait_params || trait.m_path.m_params == *trait_params) )
            {
                return found_cb( ImplRef(&type, &trait.m_path.m_params, &trait.m_type_bounds), false );
            }

            // TODO: What if `trait_params` is nullptr?
            bool rv = false;
            bool is_supertrait = trait_params && this->find_named_trait_in_trait(sp, trait_path,*trait_params, *trait.m_trait_ptr, trait.m_path.m_path,trait.m_path.m_params, type,
                [&](const auto& i_params, const auto& i_assoc) {
                    // Invoke callback with a proper ImplRef
                    ::std::map< RcString, ::HIR::TypeRef> assoc_clone;
                    for(const auto& e : i_assoc)
                        assoc_clone.insert( ::std::make_pair(e.first, e.second.clone()) );
                    // HACK! Just add all the associated type bounds (only inserted if not already present)
                    for(const auto& e2 : trait.m_type_bounds)
                        assoc_clone.insert( ::std::make_pair(e2.first, e2.second.clone()) );
                    auto ir = ImplRef(type.clone(), i_params.clone(), mv$(assoc_clone));
                    DEBUG("- ir = " << ir);
                    rv = found_cb( mv$(ir), false );
                    return false;
                });
            if( is_supertrait )
            {
                return rv;
            }
        }
    )

    // ---
    // If this type is an opaque UfcsKnown - check bounds
    // ---
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Path, e,
        if( e.binding.is_Opaque() )
        {
            ASSERT_BUG(sp, e.path.m_data.is_UfcsKnown(), "Opaque bound type wasn't UfcsKnown - " << type);
            const auto& pe = e.path.m_data.as_UfcsKnown();
            DEBUG("Checking bounds on definition of " << pe.item << " in " << pe.trait);

            // If this associated type has a bound of the desired trait, return it.
            const auto& trait_ref = m_crate.get_trait_by_path(sp, pe.trait.m_path);
            ASSERT_BUG(sp, trait_ref.m_types.count( pe.item ) != 0, "Trait " << pe.trait.m_path << " doesn't contain an associated type " << pe.item);
            const auto& aty_def = trait_ref.m_types.find(pe.item)->second;

            auto monomorph_cb = monomorphise_type_get_cb(sp, &*pe.type, &pe.trait.m_params, nullptr, nullptr);

            auto check_bound = [&](const ::HIR::TraitPath& bound) {
                const auto& b_params = bound.m_path.m_params;
                ::HIR::PathParams   params_mono_o;
                const auto& b_params_mono = (monomorphise_pathparams_needed(b_params) ? params_mono_o = monomorphise_path_params_with(sp, b_params, monomorph_cb, false) : b_params);
                DEBUG("[find_impl] : " << bound.m_path.m_path << b_params_mono);

                if( bound.m_path.m_path == trait_path )
                {
                    if( !trait_params || b_params_mono == *trait_params )
                    {
                        if( &b_params_mono == &params_mono_o || ::std::any_of(bound.m_type_bounds.begin(), bound.m_type_bounds.end(), [&](const auto& x){ return monomorphise_type_needed(x.second); }) )
                        {
                            ::std::map< RcString, ::HIR::TypeRef>  atys;
                            if( ! bound.m_type_bounds.empty() )
                            {
                                for(const auto& tb : bound.m_type_bounds)
                                {
                                    auto aty = monomorphise_type_with(sp, tb.second, monomorph_cb, false);
                                    expand_associated_types(sp, aty);
                                    atys.insert(::std::make_pair( tb.first, mv$(aty) ));
                                }
                            }
                            if( found_cb( ImplRef(type.clone(), mv$(params_mono_o), mv$(atys)), false ) )
                                return true;
                            params_mono_o = monomorphise_path_params_with(sp, b_params, monomorph_cb, false);
                        }
                        else
                        {
                            if( found_cb( ImplRef(&type, &bound.m_path.m_params, &bound.m_type_bounds), false ) )
                                return true;
                        }
                    }
                }

                bool ret = trait_params && this->find_named_trait_in_trait(sp,  trait_path, *trait_params,  *bound.m_trait_ptr,  bound.m_path.m_path, b_params_mono, type,
                    [&](const auto& i_params, const auto& i_assoc) {
                        if( i_params != *trait_params )
                            return false;
                        DEBUG("impl " << trait_path << i_params << " for " << type << " -- desired " << trait_path << *trait_params);
                        return found_cb( ImplRef(type.clone(), i_params.clone(), {}), false );
                    });
                return ret;
                };

            for(const auto& bound : aty_def.m_trait_bounds)
            {
                if( check_bound(bound) )
                    return true;
            }

            // Check `where` clauses on the trait too
            for(const auto& bound : trait_ref.m_params.m_bounds)
            {
                if( !bound.is_TraitBound() )   continue;
                const auto& be = bound.as_TraitBound();

                DEBUG("be.type = " << be.type);
                if( !be.type.m_data.is_Path() )
                    continue;
                if( !be.type.m_data.as_Path().path.m_data.is_UfcsKnown() )
                    continue ;
                {
                    const auto& pe2 = be.type.m_data.as_Path().path.m_data.as_UfcsKnown();
                    if( *pe2.type != ::HIR::TypeRef("Self",GENERIC_Self) )
                        continue ;
                    if( pe2.trait.m_path != pe.trait.m_path )
                        continue ;
                    if( pe2.item != pe.item )
                        continue ;
                }

                if( check_bound(be.trait) )
                    return true;
            }

            DEBUG("- No bounds matched");
        }
    )
    // --- /UfcsKnown ---

    bool ret;

    // TODO: A bound can imply something via its associated types. How deep can this go?
    // E.g. `T: IntoIterator<Item=&u8>` implies `<T as IntoIterator>::IntoIter : Iterator<Item=&u8>`
    ret = this->iterate_bounds([&](const auto& b) {
        return this->find_impl__check_bound(sp, trait_path, trait_params, type, found_cb,  b);
        });
    if(ret)
        return true;

    if( m_crate.get_trait_by_path(sp, trait_path).m_is_marker )
    {
        struct H {
            static bool find_impl__auto_trait_check(const StaticTraitResolve& self,
                    const Span& sp, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params, const ::HIR::TypeRef& type,
                    t_cb_find_impl found_cb,
                    const ::HIR::MarkerImpl& impl, bool& out_rv
                )
            {
                DEBUG("- Auto " << (impl.is_positive ? "Pos" : "Neg")
                    << " impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type << " " << impl.m_params.fmt_bounds());
                if (impl.is_positive)
                {
                    return self.find_impl__check_crate_raw(sp, trait_path, trait_params, type, impl.m_params, impl.m_trait_args, impl.m_type,
                        [&](auto impl_params, auto placeholders, auto cmp)->bool {
                            //rv = found_cb( ImplRef(impl_params, trait_path, impl, mv$(placeholders)), (cmp == ::HIR::Compare::Fuzzy) );
                            out_rv = found_cb(ImplRef(&type, trait_params, &null_assoc), cmp == ::HIR::Compare::Fuzzy);
                            return out_rv;
                        });
                }
                else
                {
                    return self.find_impl__check_crate_raw(sp, trait_path, trait_params, type, impl.m_params, impl.m_trait_args, impl.m_type,
                        [&](auto impl_params, auto placeholders, auto cmp)->bool {
                            out_rv = false;
                            return true;
                        });
                }
            }
        };

        // Positive/negative impls
        bool rv = false;
        ret = this->m_crate.find_auto_trait_impls(trait_path, type, cb_ident, [&](const auto& impl)->bool {
            return H::find_impl__auto_trait_check(*this, sp, trait_path, trait_params, type, found_cb, impl, rv);
            });
        if(ret)
            return rv;

        // Detect recursion and return true if detected
        static ::std::vector< ::std::tuple< const ::HIR::SimplePath*, const ::HIR::PathParams*, const ::HIR::TypeRef*> >    stack;
        for(const auto& ent : stack ) {
            if( *::std::get<0>(ent) != trait_path )
                continue ;
            if( ::std::get<1>(ent) && trait_params && *::std::get<1>(ent) != *trait_params )
                continue ;
            if( *::std::get<2>(ent) != type )
                continue ;

            return found_cb( ImplRef(&type, trait_params, &null_assoc), false );
        }
        stack.push_back( ::std::make_tuple( &trait_path, trait_params, &type ) );
        struct Guard {
            ~Guard() { stack.pop_back(); }
        };
        Guard   _;

        auto cmp = this->check_auto_trait_impl_destructure(sp, trait_path, trait_params, type);
        if( cmp != ::HIR::Compare::Unequal )
            return found_cb( ImplRef(&type, trait_params, &null_assoc), cmp == ::HIR::Compare::Fuzzy );
        return false;
    }
    else
    {
        // Search the crate for impls
        ret = m_crate.find_trait_impls(trait_path, type, cb_ident, [&](const auto& impl) {
            return this->find_impl__check_crate(sp, trait_path, trait_params, type, found_cb,  impl);
            });
        if(ret)
            return true;

        return false;
    }
}

bool StaticTraitResolve::find_impl__check_bound(
        const Span& sp,
        const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
        const ::HIR::TypeRef& type,
        t_cb_find_impl found_cb,
        const ::HIR::GenericBound& bound
    ) const
{
    struct H {
        static bool compare_pp(const Span& sp, const ::HIR::PathParams& left, const ::HIR::PathParams& right) {
            ASSERT_BUG( sp, left.m_types.size() == right.m_types.size(), "Parameter count mismatch between " << left << " and " << right );
            for(unsigned int i = 0; i < left.m_types.size(); i ++) {
                // TODO: Permits fuzzy comparison to handle placeholder params, should instead do a match/test/assign
                if( left.m_types[i].compare_with_placeholders(sp, right.m_types[i], [](const auto&t)->const ::HIR::TypeRef&{return t;}) == ::HIR::Compare::Unequal ) {
                //if( left.m_types[i] != right.m_types[i] ) {
                    return false;
                }
            }
            return true;
        }
    };

    // Can only get good information out of TraitBound
    if( !bound.is_TraitBound() ) {
        return false;
    }
    const auto& e = bound.as_TraitBound();

    // Obtain a pointer to UfcsKnown for magic later
    const ::HIR::Path::Data::Data_UfcsKnown* assoc_info = nullptr;
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Path, e,
        TU_IFLET(::HIR::Path::Data, e.path.m_data, UfcsKnown, pe,
            assoc_info = &pe;
        )
    )

    const auto& b_params = e.trait.m_path.m_params;
    DEBUG("(bound) - " << e.type << " : " << e.trait);
    if( e.type == type )
    {
        if( e.trait.m_path.m_path == trait_path ) {
            // Check against `params`
            if( trait_params ) {
                DEBUG("Checking " << *trait_params << " vs " << b_params);
                if( !H::compare_pp(sp, *trait_params, b_params) )
                    return false;
            }
            // Hand off to the closure, and return true if it does
            if( found_cb(ImplRef(&e.type, &e.trait.m_path.m_params, &e.trait.m_type_bounds), false) ) {
                return true;
            }
        }
        // HACK: The wrapping closure takes associated types from this bound and applies them to the returned set
        // - XXX: This is actually wrong (false-positive) in many cases. FIXME
        bool rv = this->find_named_trait_in_trait(sp,
            trait_path,*trait_params,
            *e.trait.m_trait_ptr, e.trait.m_path.m_path,e.trait.m_path.m_params,
            type,
            [&](const auto& params, auto assoc) {
                for(const auto& i : e.trait.m_type_bounds) {
                    // TODO: Only include from above when needed
                    //if( des_trait_ref.m_types.count(i.first) ) {
                        assoc.insert( ::std::make_pair(i.first, i.second.clone())  );
                    //}
                }
                return found_cb( ImplRef(type.clone(), params.clone(), mv$(assoc)), false );
            });
        if( rv ) {
            return true;
        }
    }

    // If the input type is an associated type controlled by this trait bound, check for added bounds.
    // TODO: This just checks a single layer, but it's feasable that there could be multiple layers
    if( assoc_info && e.trait.m_path.m_path == assoc_info->trait.m_path && e.type == *assoc_info->type && H::compare_pp(sp, b_params, assoc_info->trait.m_params) ) {

        const auto& trait_ref = *e.trait.m_trait_ptr;
        const auto& at = trait_ref.m_types.at(assoc_info->item);
        for(const auto& bound : at.m_trait_bounds) {
            if( bound.m_path.m_path == trait_path && (!trait_params || H::compare_pp(sp, bound.m_path.m_params, *trait_params)) ) {
                DEBUG("- Found an associated type impl");

                auto tp_mono = monomorphise_traitpath_with(sp, bound, [&assoc_info,&sp](const auto& gt)->const ::HIR::TypeRef& {
                    const auto& ge = gt.m_data.as_Generic();
                    if( ge.binding == 0xFFFF ) {
                        return *assoc_info->type;
                    }
                    else {
                        if( ge.binding >= assoc_info->trait.m_params.m_types.size() )
                            BUG(sp, "find_trait_impls_bound - Generic #" << ge.binding << " " << ge.name << " out of range");
                        return assoc_info->trait.m_params.m_types[ge.binding];
                    }
                    }, false);
                // - Expand associated types
                for(auto& ty : tp_mono.m_type_bounds) {
                    this->expand_associated_types(sp, ty.second);
                }
                DEBUG("- tp_mono = " << tp_mono);
                // TODO: Instead of using `type` here, build the real type
                if( found_cb( ImplRef(type.clone(), mv$(tp_mono.m_path.m_params), mv$(tp_mono.m_type_bounds)), false ) ) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool StaticTraitResolve::find_impl__check_crate_raw(
        const Span& sp,
        const ::HIR::SimplePath& des_trait_path, const ::HIR::PathParams* des_trait_params, const ::HIR::TypeRef& des_type,
        const ::HIR::GenericParams& impl_params_def, const ::HIR::PathParams& impl_trait_params, const ::HIR::TypeRef& impl_type,
        ::std::function<bool(::std::vector<const ::HIR::TypeRef*>, ::std::vector<::HIR::TypeRef>, ::HIR::Compare)> found_cb
    ) const
{
    auto cb_ident = [](const auto&ty)->const ::HIR::TypeRef&{return ty;};
    TRACE_FUNCTION_F("impl" << impl_params_def.fmt_args() << " " << des_trait_path << impl_trait_params << " for " << impl_type << impl_params_def.fmt_bounds());

    // TODO: What if `des_trait_params` already has impl placeholders?

    ::std::vector< const ::HIR::TypeRef*> impl_params;
    impl_params.resize( impl_params_def.m_types.size() );

    auto cb = [&impl_params,&sp,cb_ident](auto idx, const auto& /*name*/, const auto& ty) {
        assert( idx < impl_params.size() );
        if( ! impl_params[idx] ) {
            impl_params[idx] = &ty;
            DEBUG("[find_impl__check_crate_raw:cb] Set placeholder " << idx << " to " << ty);
            return ::HIR::Compare::Equal;
        }
        else {
            return impl_params[idx]->compare_with_placeholders(sp, ty, cb_ident);
        }
        };
    auto match = impl_type.match_test_generics_fuzz(sp, des_type, cb_ident, cb);
    unsigned base_impl_placeholder_idx = 0;
    if( des_trait_params )
    {
        assert( des_trait_params->m_types.size() == impl_trait_params.m_types.size() );
        unsigned max_impl_idx = 0;
        for( unsigned int i = 0; i < impl_trait_params.m_types.size(); i ++ )
        {
            const auto& l = impl_trait_params.m_types[i];
            const auto& r = des_trait_params->m_types[i];
            match &= l.match_test_generics_fuzz(sp, r, cb_ident, cb);

            visit_ty_with(r, [&](const ::HIR::TypeRef& t)->bool {
                if( t.m_data.is_Generic() && (t.m_data.as_Generic().binding >> 8) == 2 ) {
                    unsigned impl_idx = t.m_data.as_Generic().binding & 0xFF;
                    max_impl_idx = ::std::max(max_impl_idx, impl_idx+1);
                }
                return false;
                });
        }
        base_impl_placeholder_idx = max_impl_idx;

        size_t n_placeholders_needed = 0;
        for(unsigned int i = 0; i < impl_params.size(); i ++ ) {
            if( !impl_params[i] ) {
                n_placeholders_needed ++;
            }
        }
        ASSERT_BUG(sp, base_impl_placeholder_idx + n_placeholders_needed <= 256, "Out of impl placeholders");
    }
    if( match == ::HIR::Compare::Unequal ) {
        DEBUG(" > Type mismatch");
        return false;
    }

    ::std::vector< ::HIR::TypeRef>  placeholders;
    for(unsigned int i = 0; i < impl_params.size(); i ++ ) {
        if( !impl_params[i] ) {
            if( placeholders.size() == 0 )
                placeholders.resize(impl_params.size());
            placeholders[i] = ::HIR::TypeRef("impl_?", 2*256 + i + base_impl_placeholder_idx);
            DEBUG("Placeholder " << placeholders[i] << " for " << impl_params_def.m_types[i].m_name);
        }
    }
    // Callback that matches placeholders to concrete types
    auto cb_match = [&](unsigned int idx, const auto& /*name*/, const auto& ty)->::HIR::Compare {
        if( ty.m_data.is_Generic() && ty.m_data.as_Generic().binding == idx )
            return ::HIR::Compare::Equal;
        if( idx >> 8 == 2 ) {
            if( (idx % 256) >= base_impl_placeholder_idx ) {
                auto i = idx % 256 - base_impl_placeholder_idx;
                ASSERT_BUG(sp, !impl_params[i], "Placeholder to populated type returned. new " << ty << ", existing " << *impl_params[i]);
                auto& ph = placeholders[i];
                if( ph.m_data.is_Generic() && ph.m_data.as_Generic().binding == idx ) {
                    DEBUG("[find_impl__check_crate_raw:cb_match] Bind placeholder " << i << " to " << ty);
                    ph = ty.clone();
                    return ::HIR::Compare::Equal;
                }
                else if( ph == ty ) {
                    return ::HIR::Compare::Equal;
                }
                else {
                    TODO(sp, "[find_impl__check_crate_raw:cb_match] Compare placeholder " << i << " " << ph << " == " << ty);
                }
            }
            else {
                return ::HIR::Compare::Fuzzy;
            }
        }
        else {
            return ::HIR::Compare::Unequal;
        }
        };
    // Callback that returns monomorpisation results
    auto cb_monomorph = [&](const auto& gt)->const ::HIR::TypeRef& {
            const auto& ge = gt.m_data.as_Generic();
            if( ge.binding == GENERIC_Self ) {
                // TODO: `impl_type` or `des_type`
                DEBUG("[find_impl__check_crate_raw] Self - " << impl_type << " or " << des_type);
                //TODO(sp, "[find_impl__check_crate_raw] Self - " << impl_type << " or " << des_type);
                return impl_type;
            }
            ASSERT_BUG(sp, ge.binding >> 8 != 2, "[find_impl__check_crate_raw] Placeholder param seen - " << gt);
            ASSERT_BUG(sp, ge.binding < impl_params.size(), "[find_impl__check_crate_raw] Binding out of range - " << gt);
            if( !impl_params[ge.binding] ) {
                return placeholders[ge.binding];
            }
            return *impl_params[ge.binding];
            };

    // Bounds
    for(const auto& bound : impl_params_def.m_bounds) {
        if( const auto* ep = bound.opt_TraitBound() )
        {
            const auto& e = *ep;

            DEBUG("Trait bound " << e.type << " : " << e.trait);
            auto b_ty_mono = monomorphise_type_with(sp, e.type, cb_monomorph);
            this->expand_associated_types(sp, b_ty_mono);
            auto b_tp_mono = monomorphise_traitpath_with(sp, e.trait, cb_monomorph, false);
            for(auto& ty : b_tp_mono.m_path.m_params.m_types) {
                this->expand_associated_types(sp, ty);
            }
            for(auto& assoc_bound : b_tp_mono.m_type_bounds) {
                // TODO: These should be tagged with the source trait and that source trait used for expansion.
                this->expand_associated_types(sp, assoc_bound.second);
            }
            DEBUG("- b_ty_mono = " << b_ty_mono << ", b_tp_mono = " << b_tp_mono);
            // HACK: If the type is '_', assume the bound passes
            if( b_ty_mono.m_data.is_Infer() ) {
                continue ;
            }

            // TODO: This is extrememly inefficient (looks up the trait impl 1+N times)
            if( b_tp_mono.m_type_bounds.size() > 0 )
            {
                //
                for(const auto& assoc_bound : b_tp_mono.m_type_bounds) {
                    const auto& aty_name = assoc_bound.first;
                    const ::HIR::TypeRef& exp = assoc_bound.second;

                    ::HIR::GenericPath  aty_src_trait;
                    trait_contains_type(sp, b_tp_mono.m_path, *e.trait.m_trait_ptr, aty_name.c_str(), aty_src_trait);

                    bool rv = false;
                    if( b_ty_mono.m_data.is_Generic() && (b_ty_mono.m_data.as_Generic().binding >> 8) == 2 ) {
                        DEBUG("- Placeholder param " << b_ty_mono << ", magic success");
                        rv = true;
                    }
                    else {
                        rv = this->find_impl(sp, aty_src_trait.m_path, aty_src_trait.m_params, b_ty_mono, [&](const auto& impl, bool)->bool {
                            ::HIR::TypeRef have = impl.get_type(aty_name.c_str());
                            this->expand_associated_types(sp, have);

                            DEBUG("::" << aty_name << " - " << have << " ?= " << exp);
                            //auto cmp = have .match_test_generics_fuzz(sp, exp, cb_ident, cb_match);
                            auto cmp = exp .match_test_generics_fuzz(sp, have, cb_ident, cb_match);
                            if( cmp == ::HIR::Compare::Unequal )
                                DEBUG("Assoc ty " << aty_name << " mismatch, " << have << " != des " << exp);
                            return cmp != ::HIR::Compare::Unequal;
                            });
                    }
                    if( !rv ) {
                        DEBUG("> Fail (assoc " << aty_name << ") - " << b_ty_mono << " : " << aty_src_trait);
                        return false;
                    }
                }
            }

            // TODO: Detect if the associated type bound above is from directly the bounded trait, and skip this if it's the case
            //else
            {
                bool rv = false;
                if( b_ty_mono.m_data.is_Generic() && (b_ty_mono.m_data.as_Generic().binding >> 8) == 2 ) {
                    DEBUG("- Placeholder param " << b_ty_mono << ", magic success");
                    rv = true;
                }
                else {
                    rv = this->find_impl(sp, b_tp_mono.m_path.m_path, b_tp_mono.m_path.m_params, b_ty_mono, [&](const auto& impl, bool) {
                        return true;
                        });
                }
                if( !rv ) {
                    DEBUG("> Fail - " << b_ty_mono << ": " << b_tp_mono);
                    return false;
                }
            }
        }
        else  // bound.opt_TraitBound()
        {
            // Ignore
        }
    }

    return found_cb( mv$(impl_params), mv$(placeholders), match );
}

bool StaticTraitResolve::find_impl__check_crate(
        const Span& sp,
        const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
        const ::HIR::TypeRef& type,
        t_cb_find_impl found_cb,
        const ::HIR::TraitImpl& impl
    ) const
{
    DEBUG("impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type << impl.m_params.fmt_bounds());
    return this->find_impl__check_crate_raw(
        sp,
        trait_path, trait_params, type,
        impl.m_params, impl.m_trait_args, impl.m_type,
        [&](auto impl_params, auto placeholders, auto match) {
            return found_cb( ImplRef(impl_params, trait_path, impl, mv$(placeholders)), (match == ::HIR::Compare::Fuzzy) );
        });
}

::HIR::Compare StaticTraitResolve::check_auto_trait_impl_destructure(const Span& sp, const ::HIR::SimplePath& trait, const ::HIR::PathParams* params_ptr, const ::HIR::TypeRef& type) const
{
    TRACE_FUNCTION_F("trait = " << trait << ", type = " << type);
    // HELPER: Search for an impl of this trait for an inner type, and return the match type
    auto type_impls_trait = [&](const auto& inner_ty) -> ::HIR::Compare {
        auto l_res = ::HIR::Compare::Unequal;
        this->find_impl(sp, trait, *params_ptr, inner_ty, [&](auto, auto is_fuzzy){ l_res = is_fuzzy ? ::HIR::Compare::Fuzzy : ::HIR::Compare::Equal; return !is_fuzzy; });
        DEBUG("[check_auto_trait_impl_destructure] " << inner_ty << " - " << l_res);
        return l_res;
        };

    // - If the type is a path (struct/enum/...), search for impls for all contained types.
    if( const auto* ep = type.m_data.opt_Path() )
    {
        const auto& e = *ep;
        ::HIR::Compare  res = ::HIR::Compare::Equal;
        TU_MATCH( ::HIR::Path::Data, (e.path.m_data), (pe),
        (Generic,
            ::HIR::TypeRef  tmp;
            auto monomorph_cb = [&](const auto& gt)->const ::HIR::TypeRef& {
                const auto& ge = gt.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    BUG(sp, "Self type in struct/enum generics");
                }
                else if( ge.binding >> 8 == 0 ) {
                    auto idx = ge.binding & 0xFF;
                    ASSERT_BUG(sp, idx < pe.m_params.m_types.size(), "Type parameter out of range - " << gt);
                    return pe.m_params.m_types[idx];
                }
                else {
                    BUG(sp, "Unexpected type parameter - " << gt << " in content of " << type);
                }
                };
            // HELPER: Get a possibily monomorphised version of the input type (stored in `tmp` if needed)
            auto monomorph_get = [&](const auto& ty)->const ::HIR::TypeRef& {
                if( monomorphise_type_needed(ty) ) {
                    tmp = monomorphise_type_with(sp, ty,  monomorph_cb);
                    this->expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return ty;
                }
                };

            TU_MATCH( ::HIR::TypeRef::TypePathBinding, (e.binding), (tpb),
            (Opaque,
                BUG(sp, "Opaque binding on generic path - " << type);
                ),
            (Unbound,
                BUG(sp, "Unbound binding on generic path - " << type);
                ),
            (Struct,
                const auto& str = *tpb;

                // TODO: Somehow store a ruleset for auto traits on the type
                // - Map of trait->does_impl for local fields?
                // - Problems occur with type parameters
                TU_MATCH( ::HIR::Struct::Data, (str.m_data), (se),
                (Unit,
                    ),
                (Tuple,
                    for(const auto& fld : se)
                    {
                        const auto& fld_ty_mono = monomorph_get(fld.ent);
                        DEBUG("Struct::Tuple " << fld_ty_mono);
                        res &= type_impls_trait(fld_ty_mono);
                        if( res == ::HIR::Compare::Unequal )
                            return ::HIR::Compare::Unequal;
                    }
                    ),
                (Named,
                    for(const auto& fld : se)
                    {
                        const auto& fld_ty_mono = monomorph_get(fld.second.ent);
                        DEBUG("Struct::Named '" << fld.first << "' " << fld_ty_mono);

                        res &= type_impls_trait(fld_ty_mono);
                        if( res == ::HIR::Compare::Unequal )
                            return ::HIR::Compare::Unequal;
                    }
                    )
                )
                ),
            (Enum,
                if( const auto* e = tpb->m_data.opt_Data() )
                {
                    for(const auto& var : *e )
                    {
                        const auto& fld_ty_mono = monomorph_get(var.type);
                        DEBUG("Enum '" << var.name << "' " << fld_ty_mono);
                        res &= type_impls_trait(fld_ty_mono);
                        if( res == ::HIR::Compare::Unequal )
                            return ::HIR::Compare::Unequal;
                    }
                }
                ),
            (Union,
                TODO(sp, "Check auto trait destructure on union " << type);
                ),
            (ExternType,
                TODO(sp, "Check auto trait destructure on extern type " << type);
                )
            )
            DEBUG("- Nothing failed, calling callback");
            ),
        (UfcsUnknown,
            BUG(sp, "UfcsUnknown in typeck - " << type);
            ),
        (UfcsKnown,
            TODO(sp, "Check trait bounds for bound on UfcsKnown " << type);
            ),
        (UfcsInherent,
            TODO(sp, "Auto trait lookup on UFCS Inherent type");
            )
        )
        return res;
    }
    else if( const auto* ep = type.m_data.opt_Tuple() )
    {
        ::HIR::Compare  res = ::HIR::Compare::Equal;
        for(const auto& sty : *ep)
        {
            res &= type_impls_trait(sty);
            if( res == ::HIR::Compare::Unequal )
                return ::HIR::Compare::Unequal;
        }
        return res;
    }
    else if( const auto* e = type.m_data.opt_Array() )
    {
        return type_impls_trait(*e->inner);
    }
    // Otherwise, there's no negative so it must be positive
    else {
        return ::HIR::Compare::Equal;
    }
}

void StaticTraitResolve::expand_associated_types(const Span& sp, ::HIR::TypeRef& input) const
{
    TRACE_FUNCTION_F(input);
    this->expand_associated_types_inner(sp, input);
}
bool StaticTraitResolve::expand_associated_types_single(const Span& sp, ::HIR::TypeRef& input) const
{
    TRACE_FUNCTION_F(input);
    if( input.m_data.is_Path() && input.m_data.as_Path().path.m_data.is_UfcsKnown() )
    {
        return expand_associated_types__UfcsKnown(sp, input, /*recurse=*/false);
    }
    else
    {
        return false;
    }
}
void StaticTraitResolve::expand_associated_types_inner(const Span& sp, ::HIR::TypeRef& input) const
{
    TU_MATCH(::HIR::TypeRef::Data, (input.m_data), (e),
    (Infer,
        //if( m_treat_ivars_as_bugs ) {
        //    BUG(sp, "Encountered inferrence variable in static context");
        //}
        ),
    (Diverge,
        ),
    (Primitive,
        ),
    (Path,
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
        (Generic,
            for(auto& arg : e2.m_params.m_types)
                this->expand_associated_types_inner(sp, arg);
            ),
        (UfcsInherent,
            this->expand_associated_types_inner(sp, *e2.type);
            for(auto& arg : e2.params.m_types)
                this->expand_associated_types_inner(sp, arg);
            // TODO: impl params too?
            for(auto& arg : e2.impl_params.m_types)
                this->expand_associated_types_inner(sp, arg);
            ),
        (UfcsKnown,
            // - Only try resolving if the binding isn't known
            if( !e.binding.is_Unbound() )
                return ;
            this->expand_associated_types__UfcsKnown(sp, input);
            return;
            ),
        (UfcsUnknown,
            BUG(sp, "Encountered UfcsUnknown in EAT - " << e.path);
            )
        )
        ),
    (Generic,
        ),
    (TraitObject,
        // Recurse?
        ),
    (ErasedType,
        // Recurse?
        ),
    (Array,
        expand_associated_types_inner(sp, *e.inner);
        ),
    (Slice,
        expand_associated_types_inner(sp, *e.inner);
        ),
    (Tuple,
        for(auto& sub : e) {
            expand_associated_types_inner(sp, sub);
        }
        ),
    (Borrow,
        expand_associated_types_inner(sp, *e.inner);
        ),
    (Pointer,
        expand_associated_types_inner(sp, *e.inner);
        ),
    (Function,
        // Recurse?
        for(auto& ty : e.m_arg_types)
            expand_associated_types_inner(sp, ty);
        expand_associated_types_inner(sp, *e.m_rettype);
        ),
    (Closure,
        // Recurse?
        for(auto& ty : e.m_arg_types)
            expand_associated_types_inner(sp, ty);
        expand_associated_types_inner(sp, *e.m_rettype);
        )
    )
}
bool StaticTraitResolve::expand_associated_types__UfcsKnown(const Span& sp, ::HIR::TypeRef& input, bool recurse/*=true*/) const
{
    auto& e = input.m_data.as_Path();
    auto& e2 = e.path.m_data.as_UfcsKnown();

    this->expand_associated_types_inner(sp, *e2.type);
    for(auto& arg : e2.trait.m_params.m_types)
        this->expand_associated_types_inner(sp, arg);

    DEBUG("Locating associated type for " << e.path);

    // - If it's a closure, then the only trait impls are those generated by typeck
    TU_IFLET(::HIR::TypeRef::Data, e2.type->m_data, Closure, te,
        //if( te.node->m_obj_path == ::HIR::GenericPath() )
        //{
            const auto trait_fn = this->m_crate.get_lang_item_path(sp, "fn");
            const auto trait_fn_mut = this->m_crate.get_lang_item_path(sp, "fn_mut");
            const auto trait_fn_once = this->m_crate.get_lang_item_path(sp, "fn_once");
            if( e2.trait.m_path == trait_fn || e2.trait.m_path == trait_fn_mut || e2.trait.m_path == trait_fn_once  ) {
                if( e2.item == "Output" ) {
                    input = te.m_rettype->clone();
                    return true;
                }
                else {
                    ERROR(sp, E0000, "No associated type " << e2.item << " for trait " << e2.trait);
                }
            }
        //}
        //else
        //{
        //    // TODO: Locate impl _without_ binding params too hard?
        //}
    )

    // If it's a TraitObject, then maybe we're asking for a bound
    TU_IFLET(::HIR::TypeRef::Data, e2.type->m_data, TraitObject, te,
        const auto& data_trait = te.m_trait.m_path;
        if( e2.trait.m_path == data_trait.m_path ) {
            if( e2.trait.m_params == data_trait.m_params )
            {
                auto it = te.m_trait.m_type_bounds.find( e2.item );
                if( it == te.m_trait.m_type_bounds.end() ) {
                    // TODO: Mark as opaque and return.
                    // - Why opaque? It's not bounded, don't even bother
                    TODO(sp, "Handle unconstrained associate type " << e2.item << " from " << *e2.type);
                }

                input = it->second.clone();
                return true;
            }
        }
    )

    // 1. Bounds
    bool rv;
    bool assume_opaque = true;
    rv = this->iterate_bounds([&](const auto& b)->bool {
        if( const auto* bep = b.opt_TraitBound() )
        {
            const auto& be = *bep;
            DEBUG("Trait bound - " << be.type << " : " << be.trait);
            // 1. Check if the type matches
            //  - TODO: This should be a fuzzier match?
            if( be.type != *e2.type )
                return false;
            // 2. Check if the trait (or any supertrait) includes e2.trait
            if( be.trait.m_path == e2.trait ) {
                auto it = be.trait.m_type_bounds.find(e2.item);
                // 1. Check if the bounds include the desired item
                if( it == be.trait.m_type_bounds.end() ) {
                    // If not, assume it's opaque and return as such
                    // TODO: What happens if there's two bounds that overlap? 'F: FnMut<()>, F: FnOnce<(), Output=Bar>'
                    DEBUG("Found impl for " << input << " but no bound on item, assuming opaque");
                }
                else {
                    assume_opaque = false;
                    input = it->second.clone();
                }
                return true;
            }

            bool found_supertrait = this->find_named_trait_in_trait(sp,
                e2.trait.m_path, e2.trait.m_params,
                *be.trait.m_trait_ptr, be.trait.m_path.m_path, be.trait.m_path.m_params, *e2.type,
                [&e2,&input,&assume_opaque](const auto& params, auto assoc){
                    auto it = assoc.find(e2.item);
                    if( it != assoc.end() ) {
                        assume_opaque = false;
                        DEBUG("Found associated type " << input << " = " << it->second);
                        input = it->second.clone();
                    }
                    return true;
                }
                );
            if( found_supertrait ) {
                auto it = be.trait.m_type_bounds.find(e2.item);
                // 1. Check if the bounds include the desired item
                if( it == be.trait.m_type_bounds.end() ) {
                    // If not, assume it's opaque and return as such
                    // TODO: What happens if there's two bounds that overlap? 'F: FnMut<()>, F: FnOnce<(), Output=Bar>'
                    if( assume_opaque )
                        DEBUG("Found impl for " << input << " but no bound on item, assuming opaque");
                }
                else {
                    assume_opaque = false;
                    input = it->second.clone();
                }
                return true;
            }

            // - Didn't match
        }
        else if( const auto* bep = b.opt_TypeEquality() )
        {
            const auto& be = *bep;
            DEBUG("Equality - " << be.type << " = " << be.other_type);
            if( input == be.type ) {
                input = be.other_type.clone();
                return true;
            }
        }
        else
        {
            // Nothing.
        }
        return false;
        });
    if( rv ) {
        if( assume_opaque ) {
            input.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
            DEBUG("Assuming that " << input << " is an opaque name");

            bool rv = this->replace_equalities(input);
            if( recurse )
                this->expand_associated_types_inner(sp, input);
            return rv;
        }
        else {
            if( recurse )
                this->expand_associated_types_inner(sp, input);
            return true;
        }
    }

    // If the type of this UfcsKnown is ALSO a UfcsKnown - Check if it's bounded by this trait with equality
    // Use bounds on other associated types too (if `e2.type` was resolved to a fixed associated type)
    TU_IFLET(::HIR::TypeRef::Data, e2.type->m_data, Path, te_inner,
        TU_IFLET(::HIR::Path::Data, te_inner.path.m_data, UfcsKnown, pe_inner,
            // TODO: Search for equality bounds on this associated type (e3) that match the entire type (e2)
            // - Does simplification of complex associated types
            const auto& trait_ptr = this->m_crate.get_trait_by_path(sp, pe_inner.trait.m_path);
            const auto& assoc_ty = trait_ptr.m_types.at(pe_inner.item);

            // Resolve where Self=pe_inner.type (i.e. for the trait this inner UFCS is on)
            auto cb_placeholders_trait = [&](const auto& ty)->const ::HIR::TypeRef&{
                TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Generic, e,
                    if( e.binding == 0xFFFF )
                        return *pe_inner.type;
                    else if( e.binding >> 8 == 0 ) {
                        ASSERT_BUG(sp, e.binding < pe_inner.trait.m_params.m_types.size(), "");
                        return pe_inner.trait.m_params.m_types.at(e.binding);
                    }
                    else {
                        // TODO: Look in pe_inner.trait.m_params
                        TODO(sp, "Handle type params when expanding associated bound (#" << e.binding << " " << e.name);
                    }
                )
                else {
                    return ty;
                }
                };
            for(const auto& bound : assoc_ty.m_trait_bounds)
            {
                // If the bound is for Self and the outer trait
                // - TODO: Parameters?
                if( bound.m_path == e2.trait ) {
                    auto it = bound.m_type_bounds.find( e2.item );
                    if( it != bound.m_type_bounds.end() ) {
                        if( monomorphise_type_needed(it->second) ) {
                            input = monomorphise_type_with(sp, it->second, cb_placeholders_trait);
                        }
                        else {
                            input = it->second.clone();
                        }
                        if( recurse )
                            this->expand_associated_types(sp, input);
                        return true;
                    }
                }

                // Find trait in this trait.
                const auto& bound_trait = m_crate.get_trait_by_path(sp, bound.m_path.m_path);
                bool replaced = this->find_named_trait_in_trait(sp,
                        e2.trait.m_path, e2.trait.m_params,
                        bound_trait, bound.m_path.m_path,bound.m_path.m_params, *e2.type,
                        [&](const auto& params, const auto& assoc){
                            auto it = assoc.find(e2.item);
                            if( it != assoc.end() ) {
                                input = it->second.clone();
                                return true;
                            }
                            return false;
                        }
                        );
                if( replaced ) {
                    return true;
                }
            }
            DEBUG("e2 = " << *e2.type << ", input = " << input);
        )
    )

    // 2. Crate-level impls

    // - Search for the actual trait containing this associated type
    ::HIR::GenericPath  trait_path;
    if( !this->trait_contains_type(sp, e2.trait, this->m_crate.get_trait_by_path(sp, e2.trait.m_path), e2.item.c_str(), trait_path) )
        BUG(sp, "Cannot find associated type " << e2.item << " anywhere in trait " << e2.trait);
    //e2.trait = mv$(trait_path);

    bool replacement_happened = true;
    ::ImplRef  best_impl;
    rv = this->find_impl(sp, trait_path.m_path, trait_path.m_params, *e2.type, [&](auto impl, bool fuzzy) {
        DEBUG("[expand_associated_types] Found " << impl);
        // If a fuzzy match was found, monomorphise and EAT the checked types and try again
        // - A fuzzy can be caused by an opaque match.
        // - TODO: Move this logic into `find_impl`
        if( fuzzy ) {
            DEBUG("[expand_associated_types] - Fuzzy, monomorph+expand and recheck");

            auto impl_ty = impl.get_impl_type();
            this->expand_associated_types(sp, impl_ty);
            if(impl_ty != *e2.type) {
                DEBUG("[expand_associated_types] - Fuzzy - Doesn't match");
                return false;
            }
            auto pp = impl.get_trait_params();
            for(auto& ty : pp.m_types)
                this->expand_associated_types(sp, ty);
            if( pp != trait_path.m_params ) {
                DEBUG("[expand_associated_types] - Fuzzy - Doesn't match");
                return false;
            }
            DEBUG("[expand_associated_types] - Fuzzy - Actually matches");
        }

        if( impl.type_is_specialisable(e2.item.c_str()) ) {
            if( impl.more_specific_than(best_impl) ) {
                best_impl = mv$(impl);
                DEBUG("- Still specialisable");
            }
            return false;
        }
        else {
            auto nt = impl.get_type( e2.item.c_str() );
            if( nt == ::HIR::TypeRef() ) {
                DEBUG("Mark  " << e.path << " as opaque");
                e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                replacement_happened = this->replace_equalities(input);
            }
            else {
                DEBUG("Converted UfcsKnown - " << e.path << " = " << nt);
                if( input == nt ) {
                    replacement_happened = false;
                    return true;
                }
                input = mv$(nt);
                replacement_happened = true;
            }
            return true;
        }
        });
    if( rv ) {
        if( recurse )
            this->expand_associated_types(sp, input);
        return replacement_happened;
    }
    if( best_impl.is_valid() ) {
        e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
        this->replace_equalities(input);
        DEBUG("- Couldn't find a non-specialised impl of " << trait_path << " for " << *e2.type << " - treating as opaque");
        return false;
    }

    ERROR(sp, E0000, "Cannot find an implementation of " << trait_path << " for " << *e2.type);
}

bool StaticTraitResolve::replace_equalities(::HIR::TypeRef& input) const
{
    TRACE_FUNCTION_F("input="<<input);
    DEBUG("m_type_equalities = {" << m_type_equalities << "}");
    // - Check if there's an alias for this opaque name
    auto a = m_type_equalities.find(input);
    if( a != m_type_equalities.end() ) {
        input = a->second.clone();
        DEBUG("- Replace with " << input);
        return true;
    }
    else {
        return false;
    }
}

// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
bool StaticTraitResolve::iterate_bounds( ::std::function<bool(const ::HIR::GenericBound&)> cb) const
{
    const ::HIR::GenericParams* v[2] = { m_item_generics, m_impl_generics };
    for(auto p : v)
    {
        if( !p )    continue ;
        for(const auto& b : p->m_bounds)
            if(cb(b))   return true;
    }
    return false;
}


bool StaticTraitResolve::iterate_aty_bounds(const Span& sp, const ::HIR::Path::Data::Data_UfcsKnown& pe, ::std::function<bool(const ::HIR::TraitPath&)> cb) const
{
    const auto& trait_ref = m_crate.get_trait_by_path(sp, pe.trait.m_path);
    ASSERT_BUG(sp, trait_ref.m_types.count( pe.item ) != 0, "Trait " << pe.trait.m_path << " doesn't contain an associated type " << pe.item);
    const auto& aty_def = trait_ref.m_types.find(pe.item)->second;

    for(const auto& bound : aty_def.m_trait_bounds)
    {
        if( cb(bound) )
            return true;
    }
    // Search `<Self as Trait>::Name` bounds on the trait itself
    for(const auto& bound : trait_ref.m_params.m_bounds)
    {
        if( ! bound.is_TraitBound() ) continue ;
        const auto& be = bound.as_TraitBound();

        if( ! be.type.m_data.is_Path() )   continue ;
        if( ! be.type.m_data.as_Path().binding.is_Opaque() )   continue ;

        const auto& be_type_pe = be.type.m_data.as_Path().path.m_data.as_UfcsKnown();
        if( *be_type_pe.type != ::HIR::TypeRef("Self", 0xFFFF) )
            continue ;
        if( be_type_pe.trait.m_path != pe.trait.m_path )
            continue ;
        if( be_type_pe.item != pe.item )
            continue ;

        if( cb(be.trait) )
            return true;
    }

    return false;
}

// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
bool StaticTraitResolve::find_named_trait_in_trait(const Span& sp,
        const ::HIR::SimplePath& des, const ::HIR::PathParams& des_params,
        const ::HIR::Trait& trait_ptr, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& pp,
        const ::HIR::TypeRef& target_type,
        ::std::function<void(const ::HIR::PathParams&, ::std::map<RcString, ::HIR::TypeRef>)> callback
    ) const
{
    TRACE_FUNCTION_F(des << des_params << " from " << trait_path << pp);
    if( pp.m_types.size() != trait_ptr.m_params.m_types.size() ) {
        BUG(sp, "Incorrect number of parameters for trait - " << trait_path << pp);
    }

    auto monomorph_cb = [&](const auto& gt)->const ::HIR::TypeRef& {
        const auto& ge = gt.m_data.as_Generic();
        if( ge.binding == 0xFFFF ) {
            return target_type;
        }
        else {
            if( ge.binding >= pp.m_types.size() )
                BUG(sp, "find_named_trait_in_trait - Generic #" << ge.binding << " " << ge.name << " out of range");
            return pp.m_types[ge.binding];
        }
        };

    for( const auto& pt : trait_ptr.m_all_parent_traits )
    {
        auto pt_mono = monomorphise_traitpath_with(sp, pt, monomorph_cb, false);

        DEBUG(pt << " => " << pt_mono);
        // TODO: When in pre-typecheck mode, this needs to be a fuzzy match (because there might be a UfcsUnknown in the
        // monomorphed version) OR, there may be placeholders
        if( pt.m_path.m_path == des )
        {
            auto cmp = pt_mono.m_path.m_params.compare_with_placeholders(sp, des_params, [](const auto& t)->const ::HIR::TypeRef&{return t;});
            // pt_mono.m_path.m_params == des_params )
            if( cmp != ::HIR::Compare::Unequal )
            {
                callback( pt_mono.m_path.m_params, mv$(pt_mono.m_type_bounds) );
                return true;
            }
        }
    }

    return false;
}
bool StaticTraitResolve::trait_contains_type(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const char* name,  ::HIR::GenericPath& out_path) const
{
    TRACE_FUNCTION_FR("name="<<name << ", trait=" << trait_path, out_path);
    auto it = trait_ptr.m_types.find(name);
    if( it != trait_ptr.m_types.end() ) {
        out_path = trait_path.clone();
        return true;
    }

    auto monomorph = [&](const auto& gt)->const ::HIR::TypeRef& {
            const auto& ge = gt.m_data.as_Generic();
            assert(ge.binding < 256);
            assert(ge.binding < trait_path.m_params.m_types.size());
            return trait_path.m_params.m_types[ge.binding];
            };
    for(const auto& st : trait_ptr.m_all_parent_traits)
    {
        if( st.m_trait_ptr->m_types.count(name) )
        {
            out_path.m_path = st.m_path.m_path;
            out_path.m_params = monomorphise_path_params_with(sp, st.m_path.m_params, monomorph, false);
            return true;
        }
    }
    return false;
}

bool StaticTraitResolve::type_is_copy(const Span& sp, const ::HIR::TypeRef& ty) const
{
    TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
    (Generic,
        {
            auto it = m_copy_cache.find(ty);
            if( it != m_copy_cache.end() )
            {
                return it->second;
            }
        }
        bool rv = this->iterate_bounds([&](const auto& b)->bool {
            auto pp = ::HIR::PathParams();
            return this->find_impl__check_bound(sp, m_lang_Copy, &pp, ty, [&](auto , bool ){ return true; },  b);
            });
        m_copy_cache.insert(::std::make_pair( ty.clone(), rv ));
        return rv;
        ),
    (Path,
        const auto* markings = e.binding.get_trait_markings();
        if( markings )
        {
            if( !markings->is_copy )
            {
                // Doesn't impl Copy
                return false;
            }
            else if( !e.path.m_data.as_Generic().m_params.has_params() )
            {
                // No params, must be Copy
                return true;
            }
            else
            {
                // TODO: Also have a marking that indicates that the type is unconditionally Copy
            }
        }

        {
            auto it = m_copy_cache.find(ty);
            if( it != m_copy_cache.end() )
                return it->second;
        }
        auto pp = ::HIR::PathParams();
        bool rv = this->find_impl(sp, m_lang_Copy, &pp, ty, [&](auto , bool){ return true; }, true);
        m_copy_cache.insert(::std::make_pair( ty.clone(), rv ));
        return rv;
        ),
    (Diverge,
        // The ! type is kinda Copy ...
        return true;
        ),
    (Closure,
        if( TARGETVER_1_29 )
        {
            // TODO: Auto-gerated impls
            return e.node->m_is_copy;
        }
        return false;
        ),
    (Infer,
        // Shouldn't be hit
        return false;
        ),
    (Borrow,
        // Only shared &-ptrs are copy
        return (e.type == ::HIR::BorrowType::Shared);
        ),
    (Pointer,
        // All raw pointers are Copy
        return true;
        ),
    (Function,
        // All function pointers are Copy
        return true;
        ),
    (Primitive,
        // All primitives (except the unsized `str`) are Copy
        return e != ::HIR::CoreType::Str;
        ),
    (Array,
        return e.size_val == 0 || type_is_copy(sp, *e.inner);
        ),
    (Slice,
        // [T] isn't Sized, so isn't Copy ether
        return false;
        ),
    (TraitObject,
        // (Trait) isn't Sized, so isn't Copy ether
        return false;
        ),
    (ErasedType,
        for(const auto& trait : e.m_traits)
        {
            if( find_named_trait_in_trait(sp, m_lang_Copy, {},  *trait.m_trait_ptr, trait.m_path.m_path, trait.m_path.m_params,  ty, [](const auto&, auto ){ }) ) {
                return true;
            }
        }
        return false;
        ),
    (Tuple,
        for(const auto& ty : e)
            if( !type_is_copy(sp, ty) )
                return false;
        return true;
        )
    )
    throw "";
}
bool StaticTraitResolve::type_is_clone(const Span& sp, const ::HIR::TypeRef& ty) const
{
    if( !TARGETVER_1_29 )   BUG(sp, "Calling type_is_clone when not in 1.29 mode");

    TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
    (Generic,
        {
            auto it = m_clone_cache.find(ty);
            if( it != m_clone_cache.end() )
            {
                return it->second;
            }
        }
        bool rv = this->iterate_bounds([&](const auto& b)->bool {
            auto pp = ::HIR::PathParams();
            return this->find_impl__check_bound(sp, m_lang_Clone, &pp, ty, [&](auto , bool ){ return true; },  b);
            });
        m_clone_cache.insert(::std::make_pair( ty.clone(), rv ));
        return rv;
        ),
    (Path,
        if(true) {
            auto it = m_clone_cache.find(ty);
            if( it != m_clone_cache.end() )
                return it->second;
        }
        if( e.is_closure() )
        {
            bool rv = true;
            // TODO: Check all captures
            m_clone_cache.insert(::std::make_pair( ty.clone(), rv ));
            return rv;
        }
        auto pp = ::HIR::PathParams();
        bool rv = this->find_impl(sp, m_lang_Clone, &pp, ty, [&](auto , bool){ return true; }, true);
        m_clone_cache.insert(::std::make_pair( ty.clone(), rv ));
        return rv;
        ),
    (Diverge,
        // The ! type is kinda Copy/Clone ...
        return true;
        ),
    (Closure,
        if( TARGETVER_1_29 )
        {
            // TODO: Auto-gerated impls
            return e.node->m_is_copy;
        }
        return false;
        ),
    (Infer,
        // Shouldn't be hit
        return false;
        ),
    (Borrow,
        // Only shared &-ptrs are copy/clone
        return (e.type == ::HIR::BorrowType::Shared);
        ),
    (Pointer,
        // All raw pointers are Copy/Clone
        return true;
        ),
    (Function,
        // All function pointers are Copy/Clone
        return true;
        ),
    (Primitive,
        // All primitives (except the unsized `str`) are Copy/Clone
        return e != ::HIR::CoreType::Str;
        ),
    (Array,
        return e.size_val == 0 || type_is_clone(sp, *e.inner);
        ),
    (Slice,
        // [T] isn't Sized, so isn't Copy ether
        return false;
        ),
    (TraitObject,
        // (Trait) isn't Sized, so isn't Copy ether
        return false;
        ),
    (ErasedType,
        for(const auto& trait : e.m_traits)
        {
            if( find_named_trait_in_trait(sp, m_lang_Clone, {},  *trait.m_trait_ptr, trait.m_path.m_path, trait.m_path.m_params,  ty, [](const auto&, auto ){ }) ) {
                return true;
            }
        }
        return false;
        ),
    (Tuple,
        for(const auto& ty : e)
            if( !type_is_clone(sp, ty) )
                return false;
        return true;
        )
    )
    throw "";
}

bool StaticTraitResolve::type_is_sized(const Span& sp, const ::HIR::TypeRef& ty) const
{
    switch( this->metadata_type(sp, ty) )
    {
    case MetadataType::None:
        return true;
    default:
        return false;
    }
}
bool StaticTraitResolve::type_is_impossible(const Span& sp, const ::HIR::TypeRef& ty) const
{
    TU_MATCH_HDRA( (ty.m_data), {)
        break;
    default:
        return false;
    TU_ARMA(Diverge, _e)
        return true;
    TU_ARMA(Path, e) {
        TU_MATCHA( (e.binding), (pbe),
        (Unbound,
            // BUG?
            return false;
            ),
        (Opaque,
            // TODO: This can only be with UfcsKnown, so check if the trait specifies ?Sized
            return false;
            ),
        (Struct,
            const auto& params = e.path.m_data.as_Generic().m_params;
            // TODO: Check all fields, if one flags this, then it's impossible.
            const auto& str = *pbe;
            TU_MATCH_HDRA( (str.m_data), {)
            TU_ARMA(Unit, e)
                return false;
            TU_ARMA(Tuple, e) {
                for(const auto& fld : e)
                {
                    const auto& tpl = fld.ent;
                    ::HIR::TypeRef  tmp;
                    const auto& ty = (monomorphise_type_needed(tpl) ? tmp = monomorphise_type_with(sp, tpl, monomorphise_type_get_cb(sp, nullptr, &params, nullptr)) : tpl);
                    if( type_is_impossible(sp, ty) )
                        return true;
                }
                return false;
                }
            TU_ARMA(Named, e)
                for(const auto& fld : e)
                {
                    TODO(sp, "type_is_impossible for struct " << ty << " - " << fld.second.ent);
                }
            }
            ),
        (Enum,
            // TODO: Check all variants.
            TODO(sp, "type_is_impossible for enum " << ty);
            ),
        (Union,
            // TODO: Check all variants? Or just one?
            TODO(sp, "type_is_impossible for union " << ty);
            ),
        (ExternType,
            // Extern types are possible, just not usable
            return false;
            )
        )
        return true;
        }
    TU_ARMA(Borrow, e)
        return type_is_impossible(sp, *e.inner);
    TU_ARMA(Pointer, e) {
        return false;
        //return type_is_impossible(sp, *e.inner);
        }
    TU_ARMA(Function, e) {
        // TODO: Check all arguments?
        return true;
        }
    TU_ARMA(Array, e) {
        return type_is_impossible(sp, *e.inner);
        }
    TU_ARMA(Slice, e) {
        return type_is_impossible(sp, *e.inner);
        }
    TU_ARMA(Tuple, e) {
        for(const auto& ty : e)
            if( type_is_impossible(sp, ty) )
                return true;
        return false;
        }
    }
    throw "";
}

bool StaticTraitResolve::can_unsize(const Span& sp, const ::HIR::TypeRef& dst_ty, const ::HIR::TypeRef& src_ty) const
{
    TRACE_FUNCTION_F(dst_ty << " <- " << src_ty);

    ASSERT_BUG(sp, !dst_ty.m_data.is_Infer(), "_ seen after inferrence - " << dst_ty);
    ASSERT_BUG(sp, !src_ty.m_data.is_Infer(), "_ seen after inferrence - " << src_ty);

    {
        //ASSERT_BUG(sp, dst_ty != src_ty, "Equal types for can_unsize - " << dst_ty << " <-" << src_ty );
        if( dst_ty == src_ty )
            return true;
    }

    {
        bool found_bound = this->iterate_bounds([&](const auto& gb){
            if(!gb.is_TraitBound())
                return false;
            const auto& be = gb.as_TraitBound();
            if(be.trait.m_path.m_path != m_lang_Unsize)
                return false;
            const auto& be_dst = be.trait.m_path.m_params.m_types.at(0);

            if( src_ty != be.type )    return false;
            if( dst_ty != be_dst )    return false;
            return true;
            });
        if( found_bound )
        {
            return ::HIR::Compare::Equal;
        }
    }

    // Associated types, check the bounds in the trait.
    if( src_ty.m_data.is_Path() && src_ty.m_data.as_Path().path.m_data.is_UfcsKnown() )
    {
        const auto& pe = src_ty.m_data.as_Path().path.m_data.as_UfcsKnown();
        auto monomorph_cb = monomorphise_type_get_cb(sp, &*pe.type, &pe.trait.m_params, nullptr, nullptr);
        auto found_bound = this->iterate_aty_bounds(sp, pe, [&](const ::HIR::TraitPath& bound) {
            if( bound.m_path.m_path != m_lang_Unsize )
                return false;
            const auto& be_dst_tpl = bound.m_path.m_params.m_types.at(0);
            ::HIR::TypeRef  tmp_ty;
            const auto& be_dst = (monomorphise_type_needed(be_dst_tpl) ? tmp_ty = monomorphise_type_with(sp, be_dst_tpl, monomorph_cb) : be_dst_tpl);

            if( dst_ty != be_dst )  return false;
            return true;
            });
        if( found_bound )
        {
            return true;
        }
    }

    // Struct<..., T, ...>: Unsize<Struct<..., U, ...>>
    if( dst_ty.m_data.is_Path() && src_ty.m_data.is_Path() )
    {
        bool dst_is_unsizable = dst_ty.m_data.as_Path().binding.is_Struct() && dst_ty.m_data.as_Path().binding.as_Struct()->m_struct_markings.can_unsize;
        bool src_is_unsizable = src_ty.m_data.as_Path().binding.is_Struct() && src_ty.m_data.as_Path().binding.as_Struct()->m_struct_markings.can_unsize;
        if( dst_is_unsizable || src_is_unsizable )
        {
            DEBUG("Struct unsize? " << dst_ty << " <- " << src_ty);
            const auto& str = *dst_ty.m_data.as_Path().binding.as_Struct();
            const auto& dst_gp = dst_ty.m_data.as_Path().path.m_data.as_Generic();
            const auto& src_gp = src_ty.m_data.as_Path().path.m_data.as_Generic();

            if( dst_gp == src_gp )
            {
                DEBUG("Can't Unsize, destination and source are identical");
                return false;
            }
            else if( dst_gp.m_path == src_gp.m_path )
            {
                DEBUG("Checking for Unsize " << dst_gp << " <- " << src_gp);
                // Structures are equal, add the requirement that the ?Sized parameter also impl Unsize
                const auto& dst_inner = dst_gp.m_params.m_types.at(str.m_struct_markings.unsized_param);
                const auto& src_inner = src_gp.m_params.m_types.at(str.m_struct_markings.unsized_param);
                return this->can_unsize(sp, dst_inner, src_inner);
            }
            else
            {
                DEBUG("Can't Unsize, destination and source are different structs");
                return false;
            }
        }
    }

    // (Trait) <- Foo
    if( const auto* de = dst_ty.m_data.opt_TraitObject() )
    {
        // TODO: Check if src_ty is !Sized
        // - Only allowed if the source is a trait object with the same data trait and lesser bounds

        DEBUG("TraitObject unsize? " << dst_ty << " <- " << src_ty);

        // (Trait) <- (Trait+Foo)
        if( const auto* se = src_ty.m_data.opt_TraitObject() )
        {
            // 1. Data trait must be the same
            if( de->m_trait != se->m_trait )
            {
                return false;
            }

            // 2. Destination markers must be a strict subset
            for(const auto& mt : de->m_markers)
            {
                bool found = false;
                for(const auto& omt : se->m_markers) {
                    if( omt == mt ) {
                        found = true;
                        break;
                    }
                }
                if( !found ) {
                    // Return early.
                    return false;
                }
            }

            return true;
        }

        bool good;

        ::HIR::TypeRef::Data::Data_TraitObject  tmp_e;
        tmp_e.m_trait.m_path = de->m_trait.m_path.m_path;

        // Check data trait first.
        if( de->m_trait.m_path.m_path == ::HIR::SimplePath() ) {
            ASSERT_BUG(sp, de->m_markers.size() > 0, "TraitObject with no traits - " << dst_ty);
            good = true;
        }
        else {
            good = false;
            find_impl(sp, de->m_trait.m_path.m_path, de->m_trait.m_path.m_params, src_ty,
                [&](const auto impl, auto fuzz) {
                    //ASSERT_BUG(sp, !fuzz, "Fuzzy match in can_unsize - " << dst_ty << " <- " << src_ty << " - " << impl);
                    good = true;
                    for(const auto& aty : de->m_trait.m_type_bounds) {
                        auto atyv = impl.get_type(aty.first.c_str());
                        if( atyv == ::HIR::TypeRef() )
                        {
                            // Get the trait from which this associated type comes.
                            // Insert a UfcsKnown path for that
                            auto p = ::HIR::Path( src_ty.clone(), de->m_trait.m_path.clone(), aty.first );
                            // Run EAT
                            atyv = ::HIR::TypeRef::new_path( mv$(p), {} );
                        }
                        this->expand_associated_types(sp, atyv);
                        if( aty.second != atyv ) {
                            good = false;
                            DEBUG("ATY " << aty.first << " mismatch - " << aty.second << " != " << atyv);
                        }
                    }
                    return true;
                });
        }

        // Then markers
        auto cb = [&](const auto impl, auto ){
            tmp_e.m_markers.back().m_params = impl.get_trait_params();
            return true;
            };
        for(const auto& marker : de->m_markers)
        {
            if(!good)   break;
            tmp_e.m_markers.push_back( marker.m_path );
            good &= this->find_impl(sp, marker.m_path, marker.m_params, src_ty, cb);
        }

        return good;
    }

    // [T] <- [T; n]
    if( const auto* de = dst_ty.m_data.opt_Slice() )
    {
        if( const auto* se = src_ty.m_data.opt_Array() )
        {
            DEBUG("Array unsize? " << *de->inner << " <- " << *se->inner);
            return *se->inner == *de->inner;
        }
    }

    DEBUG("Can't unsize, no rules matched");
    return false;
}

// Check if the passed type contains an UnsafeCell
// Returns `Fuzzy` if generic, `Equal` if it does contain an UnsafeCell, and `Unequal` if it doesn't (shared=immutable)
HIR::Compare StaticTraitResolve::type_is_interior_mutable(const Span& sp, const ::HIR::TypeRef& ty) const
{
    TU_MATCH_HDRA( (ty.m_data), {)
    TU_ARMA(Infer, e) {
        // Is this a bug?
        return HIR::Compare::Fuzzy;
        }
    TU_ARMA(Diverge, e) {
        return HIR::Compare::Unequal;
        }
    TU_ARMA(Primitive, e) {
        return HIR::Compare::Unequal;
        }
    TU_ARMA(Path, e) {
        TU_MATCH_HDRA( (e.binding), {)
        TU_ARMA(Unbound, pbe)
            return HIR::Compare::Fuzzy;
        TU_ARMA(Opaque, pbe)
            return HIR::Compare::Fuzzy;
        TU_ARMA(ExternType, pbe)    // Extern types can't be interior mutable (but they also shouldn't be direct)
            return HIR::Compare::Unequal;
        // TODO: For struct/enum/union, look up.
        default:
            return HIR::Compare::Fuzzy;
        }
        }
    TU_ARMA(Generic, e) {
        return HIR::Compare::Fuzzy;
        }
    TU_ARMA(TraitObject, e) {
        // Can't know with a trait object
        return HIR::Compare::Fuzzy;
        }
    TU_ARMA(ErasedType, e) {
        // Can't know with an erased type (effectively a generic)
        return HIR::Compare::Fuzzy;
        }
    TU_ARMA(Array, e) {
        return this->type_is_interior_mutable(sp, *e.inner);
        }
    TU_ARMA(Slice, e) {
        return this->type_is_interior_mutable(sp, *e.inner);
        }
    TU_ARMA(Tuple, e) {
        for(const auto& t : e)
        {
            auto rv = this->type_is_interior_mutable(sp, t);
            if(rv != HIR::Compare::Unequal)
                return rv;
        }
        return HIR::Compare::Unequal;
        }
    TU_ARMA(Closure, e) {
        // TODO: Closures could be known?
        return HIR::Compare::Fuzzy;
        }
    // Borrow and pointer are not interior mutable (they might point to something, but that doesn't matter)
    TU_ARMA(Borrow, e) {
        return HIR::Compare::Unequal;
        }
    TU_ARMA(Pointer, e) {
        return HIR::Compare::Unequal;
        }
    TU_ARMA(Function, e) {
        return HIR::Compare::Unequal;
        }
    }
    return HIR::Compare::Fuzzy;
}

MetadataType StaticTraitResolve::metadata_type(const Span& sp, const ::HIR::TypeRef& ty, bool err_on_unknown/*=false*/) const
{
    TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
    (Generic,
        if( e.binding == 0xFFFF ) {
            // TODO: Self: Sized?
            return MetadataType::None;
        }
        else if( (e.binding >> 8) == 0 ) {
            auto idx = e.binding & 0xFF;
            assert( m_impl_generics );
            assert( idx < m_impl_generics->m_types.size() );
            if( m_impl_generics->m_types[idx].m_is_sized ) {
                return MetadataType::None;
            }
            else {
                return MetadataType::Unknown;
            }
        }
        else if( (e.binding >> 8) == 1 ) {
            auto idx = e.binding & 0xFF;
            assert( m_item_generics );
            assert( idx < m_item_generics->m_types.size() );
            if( m_item_generics->m_types[idx].m_is_sized ) {
                return MetadataType::None;
            }
            else {
                return MetadataType::Unknown;
            }
        }
        else {
            BUG(sp, "Unknown generic binding on " << ty);
        }
        ),
    (Path,
        TU_MATCHA( (e.binding), (pbe),
        (Unbound,
            // TODO: Should this return something else?
            return MetadataType::Unknown;
            ),
        (Opaque,
            //auto pp = ::HIR::PathParams();
            //return this->find_impl(sp, m_lang_Sized, &pp, ty, [&](auto , bool){ return true; }, true);
            // TODO: This can only be with UfcsKnown, so check if the trait specifies ?Sized
            //return MetadataType::Unknown;
            return MetadataType::None;
            ),
        (Struct,
            // TODO: Destructure?
            switch( pbe->m_struct_markings.dst_type )
            {
            case ::HIR::StructMarkings::DstType::None:
                return MetadataType::None;
            case ::HIR::StructMarkings::DstType::Possible:
                return this->metadata_type( sp, e.path.m_data.as_Generic().m_params.m_types.at(pbe->m_struct_markings.unsized_param) );
            case ::HIR::StructMarkings::DstType::Slice:
                return MetadataType::Slice;
            case ::HIR::StructMarkings::DstType::TraitObject:
                return MetadataType::TraitObject;
            }
            ),
        (ExternType,
            // Extern types aren't Sized, but have no metadata
            return MetadataType::Zero;
            ),
        (Enum,
            ),
        (Union,
            )
        )
        return MetadataType::None;
        ),
    (Diverge,
        // The ! type is kinda Sized ...
        return MetadataType::None;
        ),
    (Closure,
        return MetadataType::None;
        ),
    (Infer,
        // Shouldn't be hit
        BUG(sp, "Found ivar? " << ty);
        ),
    (Borrow,
        return MetadataType::None;
        ),
    (Pointer,
        return MetadataType::None;
        ),
    (Function,
        return MetadataType::None;
        ),
    (Primitive,
        // All primitives (except the unsized `str`) are Sized
        if( e == ::HIR::CoreType::Str )
        {
            return MetadataType::Slice;
        }
        else
        {
            return MetadataType::None;
        }
        ),
    (Array,
        return MetadataType::None;
        ),
    (Slice,
        return MetadataType::Slice;
        ),
    (TraitObject,
        return MetadataType::TraitObject;
        ),
    (ErasedType,
        // NOTE: All erased types are implicitly Sized
        return MetadataType::None;
        ),
    (Tuple,
        // TODO: Unsized tuples? are they a thing?
        //for(const auto& ty : e)
        //    if( !type_is_sized(sp, ty) )
        //        return false;
        return MetadataType::None;
        )
    )
    throw "bug";
}

bool StaticTraitResolve::type_needs_drop_glue(const Span& sp, const ::HIR::TypeRef& ty) const
{
    // If `T: Copy`, then it can't need drop glue
    if( type_is_copy(sp, ty) )
        return false;

    TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
    (Generic,
        // TODO: Is this an error?
        return true;
        ),
    (Path,
        if( e.binding.is_Opaque() )
            return true;

        // In 1.29, "manually_drop" is a struct with special behaviour (instead of being a union)
        if( TARGETVER_1_29 && e.path.m_data.as_Generic().m_path == m_crate.get_lang_item_path_opt("manually_drop") )
        {
            return false;
        }

        auto it = m_drop_cache.find(ty);
        if( it != m_drop_cache.end() )
        {
            return it->second;
        }

        auto pp = ::HIR::PathParams();
        bool has_direct_drop = this->find_impl(sp, m_lang_Drop, &pp, ty, [&](auto , bool){ return true; }, true);
        if( has_direct_drop )
        {
            m_drop_cache.insert(::std::make_pair(ty.clone(), true));
            return true;
        }

        ::HIR::TypeRef  tmp_ty;
        const auto& pe = e.path.m_data.as_Generic();
        auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr, &pe.m_params, nullptr, nullptr);
        auto monomorph = [&](const auto& tpl)->const ::HIR::TypeRef& {
            if( monomorphise_type_needed(tpl) ) {
                tmp_ty = monomorphise_type_with(sp, tpl, monomorph_cb, false);
                this->expand_associated_types(sp, tmp_ty);
                return tmp_ty;
            }
            else {
                return tpl;
            }
            };
        bool needs_drop_glue = false;
        TU_MATCHA( (e.binding), (pbe),
        (Unbound,
            BUG(sp, "Unbound path");
            ),
        (Opaque,
            // Technically a bug, checked above
            return true;
            ),
        (Struct,
            TU_MATCHA( (pbe->m_data), (se),
            (Unit,
                ),
            (Tuple,
                for(const auto& e : se)
                {
                    if( type_needs_drop_glue(sp, monomorph(e.ent)) )
                    {
                        needs_drop_glue = true;
                        break;
                    }
                }
                ),
            (Named,
                for(const auto& e : se)
                {
                    if( type_needs_drop_glue(sp, monomorph(e.second.ent)) )
                    {
                        needs_drop_glue = true;
                        break;
                    }
                }
                )
            )
            ),
        (Enum,
            if(const auto* e = pbe->m_data.opt_Data())
            {
                for(const auto& var : *e)
                {
                    if( type_needs_drop_glue(sp, monomorph(var.type)) )
                    {
                        needs_drop_glue = true;
                        break;
                    }
                }
            }
            ),
        (Union,
            // Unions don't have drop glue unless they impl Drop
            needs_drop_glue = false;
            ),
        (ExternType,
            // Extern types don't have drop glue
            needs_drop_glue = false;
            )
        )
        m_drop_cache.insert(::std::make_pair(ty.clone(), needs_drop_glue));
        return needs_drop_glue;
        ),
    (Diverge,
        return false;
        ),
    (Closure,
        // TODO: Destructure?
        return true;
        ),
    (Infer,
        BUG(sp, "type_needs_drop_glue on _");
        return false;
        ),
    (Borrow,
        // &-ptrs don't have drop glue
        if( e.type != ::HIR::BorrowType::Owned )
            return false;
        return type_needs_drop_glue(sp, *e.inner);
        ),
    (Pointer,
        return false;
        ),
    (Function,
        return false;
        ),
    (Primitive,
        return false;
        ),
    (Array,
        return type_needs_drop_glue(sp, *e.inner);
        ),
    (Slice,
        return type_needs_drop_glue(sp, *e.inner);
        ),
    (TraitObject,
        return true;
        ),
    (ErasedType,
        // Is this an error?
        return true;
        ),
    (Tuple,
        for(const auto& ty : e)
        {
            if( type_needs_drop_glue(sp, ty) )
                return true;
        }
        return false;
        )
    )
    assert(!"Fell off the end of type_needs_drop_glue");
    throw "";
}

const ::HIR::TypeRef* StaticTraitResolve::is_type_owned_box(const ::HIR::TypeRef& ty) const
{
    if( ! ty.m_data.is_Path() ) {
        return nullptr;
    }
    const auto& te = ty.m_data.as_Path();

    if( ! te.path.m_data.is_Generic() ) {
        return nullptr;
    }
    const auto& pe = te.path.m_data.as_Generic();

    if( pe.m_path != m_lang_Box ) {
        return nullptr;
    }
    // TODO: Properly assert?
    return &pe.m_params.m_types.at(0);
}
const ::HIR::TypeRef* StaticTraitResolve::is_type_phantom_data(const ::HIR::TypeRef& ty) const
{
    if( ! ty.m_data.is_Path() ) {
        return nullptr;
    }
    const auto& te = ty.m_data.as_Path();

    if( ! te.path.m_data.is_Generic() ) {
        return nullptr;
    }
    const auto& pe = te.path.m_data.as_Generic();

    if( pe.m_path != m_lang_PhantomData ) {
        return nullptr;
    }
    // TODO: Properly assert?
    return &pe.m_params.m_types.at(0);
}

StaticTraitResolve::ValuePtr StaticTraitResolve::get_value(const Span& sp, const ::HIR::Path& p, MonomorphState& out_params, bool signature_only/*=false*/) const
{
    TRACE_FUNCTION_F(p << ", signature_only=" << signature_only);
    out_params = MonomorphState {};
    TU_MATCH_HDR( (p.m_data), {)
    TU_ARM(p.m_data, Generic, pe) {
        if( pe.m_path.m_components.size() > 1 )
        {
            const auto& ti = m_crate.get_typeitem_by_path(sp, pe.m_path, /*ignore_crate_name=*/false, /*ignore_last_node=*/true);
            if( const auto* e = ti.opt_Enum() )
            {
                out_params.pp_impl = &pe.m_params;
                auto idx = e->find_variant(pe.m_path.m_components.back());
                if( e->m_data.is_Data() )
                {
                    if( e->m_data.as_Data()[idx].type != ::HIR::TypeRef::new_unit() )
                    {
                        return ValuePtr::Data_EnumConstructor { e, idx };
                    }
                }
                return ValuePtr::Data_EnumValue { e, idx };
            }
        }
        const ::HIR::Module& mod = m_crate.get_mod_by_path(sp, pe.m_path, /*ignore_last_node=*/true);
        const auto& v = mod.m_value_items.at(pe.m_path.m_components.back());
        TU_MATCHA( (v->ent), (ve),
        (Import, BUG(sp, "Module Import");),
        (Constant,
            out_params.pp_method = &pe.m_params;
            return &ve;
            ),
        (Static,
            out_params.pp_method = &pe.m_params;
            return &ve;
            ),
        (Function,
            out_params.pp_method = &pe.m_params;
            return &ve;
            ),
        (StructConstant,
            out_params.pp_impl = &pe.m_params;
            TODO(sp, "StructConstant - " << p);
            ),
        (StructConstructor,
            out_params.pp_impl = &pe.m_params;
            return ValuePtr::Data_StructConstructor { &ve.ty, &m_crate.get_struct_by_path(sp, ve.ty) };
            )
        )
        throw "";
        }
    TU_ARM(p.m_data, UfcsKnown, pe) {
        out_params.self_ty = &*pe.type;
        out_params.pp_impl = &pe.trait.m_params;
        out_params.pp_method = &pe.params;
        const ::HIR::Trait& tr = m_crate.get_trait_by_path(sp, pe.trait.m_path);
        if( signature_only )
        {
            const ::HIR::TraitValueItem& v = tr.m_values.at(pe.item);
            TU_MATCHA( (v), (ve),
            (Constant, return &ve; ),
            (Static,   return &ve; ),
            (Function, return &ve; )
            )
        }
        else
        {
            ImplRef best_impl;
            this->find_impl(sp, pe.trait.m_path, &pe.trait.m_params, *pe.type, [&](auto impl, bool is_fuzz)->bool{
                if( ! impl.m_data.is_TraitImpl() )
                    return false;
                const auto& ti = *impl.m_data.as_TraitImpl().impl;
                auto it = ti.m_constants.find(pe.item);
                if(it == ti.m_constants.end()) {
                    // An impl was found, but it did't have the value
                    return false;
                }

                if( impl.more_specific_than(best_impl) )
                {
                    best_impl = mv$(impl);
                    // If this value is specialisable, keep searching (return false)
                    return !it->second.is_specialisable;
                }
                // Keep searching
                return false;
                });
            if( !best_impl.is_valid() )
            {
                TODO(sp, "What should be done when an impl can't be found? " << p);
            }

            if( ! best_impl.m_data.is_TraitImpl() )
                TODO(sp, "Use bounded constant values for " << p);
            auto& ie = best_impl.m_data.as_TraitImpl();
            out_params.pp_impl = &out_params.pp_impl_data;
            for(auto ptr : ie.params)
            {
                // TODO: Avoid cloning when the params are in the placeholder array
                out_params.pp_impl_data.m_types.push_back( ptr->clone() );
            }

            const auto& ti = *ie.impl;
            const auto& c = ti.m_constants.at(pe.item);

            // TODO: What if the type requires monomorphisation? Leave it up to the caller
            return &c.data;
        }
        throw "";
        }
    TU_ARM(p.m_data, UfcsInherent, pe) {
        out_params.self_ty = &*pe.type;
        //out_params.pp_impl = &out_params.pp_impl_data;
        out_params.pp_impl = &pe.impl_params;
        out_params.pp_method = &pe.params;
        ValuePtr    rv;
        m_crate.find_type_impls(*pe.type, [](const auto&x)->const ::HIR::TypeRef& { return x; }, [&](const auto& impl) {
            DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
            // TODO: Populate pp_impl
            // TODO: Specialisation
            {
                auto fit = impl.m_methods.find(pe.item);
                if( fit != impl.m_methods.end() )
                {
                    ASSERT_BUG(sp, impl.m_params.m_types.size() == pe.impl_params.m_types.size(), "Mismatch in param counts " << p << ", params are " << impl.m_params.fmt_args());
                    DEBUG("- Contains method, good");
                    rv = ValuePtr { &fit->second.data };
                    return true;
                }
            }
            {
                auto it = impl.m_constants.find(pe.item);
                if( it != impl.m_constants.end() )
                {
                    ASSERT_BUG(sp, impl.m_params.m_types.size() == pe.impl_params.m_types.size(), "Mismatch in param counts " << p << ", params are " << impl.m_params.fmt_args());
                    rv = ValuePtr { &it->second.data };
                    return true;
                }
            }
            return false;
            });
        return rv;
        }
    TU_ARM(p.m_data, UfcsUnknown, pe) {
        BUG(sp, "UfcsUnknown - " << p);
        }
    }
    throw "";
}
