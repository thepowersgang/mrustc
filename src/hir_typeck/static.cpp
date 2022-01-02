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
    static ::HIR::TraitPath::assoc_list_t   null_assoc;

    if( !dont_handoff_to_specialised ) {
        if( trait_path == m_lang_Copy ) {
            if( this->type_is_copy(sp, type) ) {
                return found_cb( ImplRef(&type, &null_params, &null_assoc), false );
            }
        }
        else if( TARGETVER_LEAST_1_29 && trait_path == m_lang_Clone ) {
            // NOTE: Duplicated check for enumerate
            if( type.data().is_Tuple() || type.data().is_Array() || type.data().is_Function() || type.data().is_Closure()
                    || TU_TEST1(type.data(), Path, .is_closure()) )
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
            ASSERT_BUG(sp, trait_params, "TODO: Support no params for Unsize");
            const auto& dst_ty = trait_params->m_types.at(0);
            if( this->can_unsize(sp, dst_ty, type) ) {
                return found_cb( ImplRef(&type, trait_params, &null_assoc), false );
            }
        }
        else if( TARGETVER_LEAST_1_54 && trait_path == m_lang_DiscriminantKind ) {
            // If the type is generic, then don't populate the ATY
            // Otherwise, populate the ATY with the correct type
            // - Unit for non-enums
            // - Enum type (usize probably) for enums
            if( type.data().is_Generic() || (type.data().is_Path() && type.data().as_Path().binding.is_Opaque()) ) {
                return found_cb( ImplRef(&type, trait_params, &null_assoc), false );
            }
            else if( type.data().is_Path() ) {
                if( const auto* enmpp = type.data().as_Path().binding.opt_Enum() ) {
                    const auto& enm = **enmpp;
                    HIR::TypeRef    tag_ty = enm.get_repr_type(enm.m_tag_repr);
                    ::HIR::TraitPath::assoc_list_t   assoc_list;
                    assoc_list.insert(std::make_pair( RcString::new_interned("Discriminant"), HIR::TraitPath::AtyEqual {
                        m_lang_DiscriminantKind,
                        std::move(tag_ty)
                        } ));
                    return found_cb(ImplRef(type.clone(), {}, std::move(assoc_list)), false);
                }
                else {
                }
            }
            else {
            }
            static ::HIR::TraitPath::assoc_list_t   assoc_unit;
            if(assoc_unit.empty()) {
                assoc_unit.insert(std::make_pair( RcString::new_interned("Discriminant"), HIR::TraitPath::AtyEqual {
                    m_lang_DiscriminantKind,
                    HIR::TypeRef::new_unit()
                    } ));
            }
            return found_cb( ImplRef(&type, trait_params, &assoc_unit), false );
        }
        else if( TARGETVER_LEAST_1_54 && trait_path == m_lang_Pointee ) {
            static ::HIR::TraitPath::assoc_list_t   assoc_unit;
            static ::HIR::TraitPath::assoc_list_t   assoc_slice;
            static RcString name_Metadata;
            if(assoc_unit.empty()) {
                name_Metadata = RcString::new_interned("Metadata");
                assoc_unit.insert(std::make_pair( name_Metadata, HIR::TraitPath::AtyEqual {
                    m_lang_Pointee,
                    HIR::TypeRef::new_unit()
                    } ));
                assoc_slice.insert(std::make_pair( name_Metadata, HIR::TraitPath::AtyEqual {
                    m_lang_Pointee,
                    HIR::CoreType::Usize
                    } ));
            }
            // Generics (or opaque ATYs)
            if( type.data().is_Generic() || (type.data().is_Path() && type.data().as_Path().binding.is_Opaque()) ) {
                // If the type is `Sized` return `()` as the type
                if( type_is_sized(sp, type) ) {
                    return found_cb( ImplRef(&type, trait_params, &assoc_unit), false );
                }
                else {
                    // Return unbounded
                    return found_cb( ImplRef(&type, trait_params, &null_assoc), false );
                }
            }
            // Trait object: `Metadata=DynMetadata<T>`
            if( type.data().is_TraitObject() ) {
                ::HIR::TraitPath::assoc_list_t   assoc_list;
                assoc_list.insert(std::make_pair( name_Metadata, HIR::TraitPath::AtyEqual {
                    m_lang_Pointee,
                    ::HIR::TypeRef::new_path(::HIR::GenericPath(m_lang_DynMetadata, HIR::PathParams(type.clone())), &m_crate.get_struct_by_path(sp, m_lang_DynMetadata))
                    } ));
                return found_cb(ImplRef(type.clone(), {}, std::move(assoc_list)), false);
            }
            // Slice and str
            if( type.data().is_Slice() || TU_TEST1(type.data(), Primitive, == HIR::CoreType::Str) ) {
                return found_cb( ImplRef(&type, trait_params, &assoc_slice), false );
            }
            // Structs: Can delegate their metadata
            if( type.data().is_Path() && type.data().as_Path().binding.is_Struct() )
            {
                const auto& str = *type.data().as_Path().binding.as_Struct();
                switch(str.m_struct_markings.dst_type)
                {
                case HIR::StructMarkings::DstType::None:
                    return found_cb( ImplRef(&type, trait_params, &assoc_unit), false );
                case HIR::StructMarkings::DstType::Possible:
                    TODO(sp, "m_lang_Pointee - " << type);
                    //return found_cb( ImplRef(&type, trait_params, &null_assoc), false );
                case HIR::StructMarkings::DstType::Slice:
                    return found_cb( ImplRef(&type, trait_params, &assoc_slice), false );
                case HIR::StructMarkings::DstType::TraitObject:
                    TODO(sp, "m_lang_Pointee - " << type);
                }
            }
            return found_cb( ImplRef(&type, trait_params, &assoc_unit), false );
        }
    }

    // Special case: Generic placeholder
    if(const auto* e = type.data().opt_Generic() )
    {
        if( (e->binding >> 8) == 2 )
        {
            // TODO: If the type is a magic placeholder, assume it impls the specified trait.
            // TODO: Restructure so this knows that the placehlder impls the impl-provided bounds.
            return found_cb( ImplRef(&type, trait_params, &null_assoc), false );
        }
    }
    struct H
    {
        static bool check_params(const Span& sp, const HIR::PathParams& target_params, const HIR::PathParams* trait_params)
        {
            if(!trait_params)
                return true;

            return target_params.compare_with_placeholders(sp, *trait_params, [](const auto&t)->const ::HIR::TypeRef&{return t;}) != HIR::Compare::Unequal;
        }
    };

    // --- MAGIC IMPLS ---
    // TODO: There should be quite a few more here, but laziness
    TU_MATCH_HDRA( (type.data()), {)
    default:
        // Nothing magic
    TU_ARMA(Function, e) {
        if( trait_path == m_lang_Fn || trait_path == m_lang_FnMut || trait_path == m_lang_FnOnce ) {
            if( trait_params )
            {
                const auto& des_arg_tys = trait_params->m_types.at(0).data().as_Tuple();
                if( des_arg_tys.size() != e.m_arg_types.size() ) {
                    return false;
                }
                for(unsigned int i = 0; i < des_arg_tys.size(); i ++)
                {
                    if( des_arg_tys[i].compare_with_placeholders(sp, e.m_arg_types[i], [](const auto&t)->const ::HIR::TypeRef&{return t;}) == ::HIR::Compare::Unequal ) {
                        return false;
                    }
                }
            }
            else
            {
                trait_params = &null_params;
            }
            std::vector<HIR::TypeRef>   arg_types;
            for(unsigned int i = 0; i < e.m_arg_types.size(); i ++)
            {
                arg_types.push_back(e.m_arg_types[i].clone());
            }
            HIR::PathParams params;
            params.m_types.push_back(HIR::TypeRef::new_tuple(std::move(arg_types)));
            ::HIR::TraitPath::assoc_list_t  assoc;
            assoc.insert( ::std::make_pair("Output", ::HIR::TraitPath::AtyEqual { ::HIR::GenericPath(m_lang_FnOnce, params.clone()), e.m_rettype.clone() }) );
            return found_cb( ImplRef(type.clone(), mv$(params), mv$(assoc)), false );
        }
        }
    TU_ARMA(Closure, e) {
        if( trait_path == m_lang_Fn || trait_path == m_lang_FnMut || trait_path == m_lang_FnOnce )
        {
            if( trait_params )
            {
                const auto& des_arg_tys = trait_params->m_types.at(0).data().as_Tuple();
                if( des_arg_tys.size() != e.m_arg_types.size() ) {
                    return false;
                }
                for(unsigned int i = 0; i < des_arg_tys.size(); i ++)
                {
                    if( des_arg_tys[i].compare_with_placeholders(sp, e.m_arg_types[i], [](const auto&t)->const ::HIR::TypeRef&{return t;}) == ::HIR::Compare::Unequal ) {
                        return false;
                    }
                }
            }
            else
            {
                trait_params = &null_params;
            }
            switch( e.node->m_class )
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
            ::HIR::TraitPath::assoc_list_t  assoc;
            assoc.insert( ::std::make_pair("Output", ::HIR::TraitPath::AtyEqual { ::HIR::GenericPath(m_lang_FnOnce, trait_params->clone()), e.m_rettype.clone() }) );
            return found_cb( ImplRef(type.clone(), trait_params->clone(), mv$(assoc)), false );
        }
        }
    TU_ARMA(Generator, e) {
        if( TARGETVER_LEAST_1_39 && trait_path == m_lang_Generator )
        {
            ::HIR::TraitPath::assoc_list_t   assoc;
            assoc.insert(::std::make_pair("Yield" , ::HIR::TraitPath::AtyEqual { trait_path.clone(), e.node->m_yield_ty.clone() }));
            assoc.insert(::std::make_pair("Return", ::HIR::TraitPath::AtyEqual { trait_path.clone(), e.node->m_return.clone() }));
            return found_cb( ImplRef(type.clone(), HIR::PathParams(), mv$(assoc)), ::HIR::Compare::Equal );
        }
        }
    // ----
    // TraitObject traits and supertraits
    // ----
    TU_ARMA(TraitObject, e) {
        if( trait_path == e.m_trait.m_path.m_path )
        {
            if( H::check_params(sp, e.m_trait.m_path.m_params, trait_params) )
            {
                return found_cb( ImplRef(&type, &e.m_trait.m_path.m_params, &e.m_trait.m_type_bounds), false );
            }
        }
        // Markers too
        for( const auto& mt : e.m_markers )
        {
            if( trait_path == mt.m_path ) {
                if( H::check_params(sp, mt.m_params, trait_params) )
                {
                    return found_cb( ImplRef(&type, &mt.m_params, &null_assoc), false );
                }
            }
        }

        // - Check if the desired trait is a supertrait of this.
        // TODO: What if `trait_params` is nullptr?
        bool rv = false;
        bool is_supertrait = trait_params && this->find_named_trait_in_trait(sp, trait_path,*trait_params, *e.m_trait.m_trait_ptr, e.m_trait.m_path.m_path,e.m_trait.m_path.m_params, type,
            [&](const auto& i_params, const auto& i_assoc) {
                // Invoke callback with a proper ImplRef
                ::HIR::TraitPath::assoc_list_t  assoc_clone;
                for(const auto& e : i_assoc)
                    assoc_clone.insert( ::std::make_pair(e.first, e.second.clone()) );
                // HACK! Just add all the associated type bounds (only inserted if not already present)
                for(const auto& e2 : e.m_trait.m_type_bounds)
                    assoc_clone.insert( ::std::make_pair(e2.first, e2.second.clone()) );
                auto ir = ImplRef(type.clone(), i_params.clone(), mv$(assoc_clone));
                DEBUG("- ir = " << ir);
                rv = found_cb( mv$(ir), false );
                return true;
            });
        if( is_supertrait )
        {
            return rv;
        }
        }
    // Same for ErasedType
    TU_ARMA(ErasedType, e) {
        for(const auto& trait : e.m_traits)
        {
            if( trait_path == trait.m_path.m_path && H::check_params(sp, trait.m_path.m_params, trait_params) )
            {
                return found_cb( ImplRef(&type, &trait.m_path.m_params, &trait.m_type_bounds), false );
            }

            bool rv = false;
            // TODO: If `trait_params` is nullptr, this doesn't run (is that sane?)
            bool is_supertrait = trait_params && this->find_named_trait_in_trait(sp, trait_path,*trait_params, *trait.m_trait_ptr, trait.m_path.m_path,trait.m_path.m_params, type,
                [&](const auto& i_params, const auto& i_assoc) {
                    // Invoke callback with a proper ImplRef
                    ::HIR::TraitPath::assoc_list_t  assoc_clone;
                    for(const auto& assoc_e : i_assoc)
                        assoc_clone.insert( ::std::make_pair(assoc_e.first, assoc_e.second.clone()) );
                    // HACK! Just add all the associated type bounds (only inserted if not already present)
                    for(const auto& e2 : trait.m_type_bounds)
                        assoc_clone.insert( ::std::make_pair(e2.first, e2.second.clone()) );
                    auto ir = ImplRef(type.clone(), i_params.clone(), mv$(assoc_clone));
                    DEBUG("[find_impl] - ir = " << ir);
                    rv = found_cb( mv$(ir), false );
                    return true;
                });
            if( is_supertrait )
            {
                return rv;
            }
        }
        }

    // ---
    // If this type is an opaque UfcsKnown - check bounds
    // ---
    TU_ARMA(Path, e) {
        if( e.binding.is_Opaque() )
        {
            ASSERT_BUG(sp, e.path.m_data.is_UfcsKnown(), "Opaque bound type wasn't UfcsKnown - " << type);
            const auto& pe = e.path.m_data.as_UfcsKnown();
            DEBUG("Checking bounds on definition of " << pe.item << " in " << pe.trait);

            // If this associated type has a bound of the desired trait, return it.
            const auto& trait_ref = m_crate.get_trait_by_path(sp, pe.trait.m_path);
            ASSERT_BUG(sp, trait_ref.m_types.count( pe.item ) != 0, "Trait " << pe.trait.m_path << " doesn't contain an associated type " << pe.item);
            const auto& aty_def = trait_ref.m_types.find(pe.item)->second;

            auto monomorph_cb = MonomorphStatePtr(&pe.type, &pe.trait.m_params, nullptr);

            auto check_bound = [&](const ::HIR::TraitPath& bound) {
                const auto& b_params = bound.m_path.m_params;
                ::HIR::PathParams   params_mono_o;
                const auto& b_params_mono = monomorphise_pathparams_with_opt(sp, params_mono_o, b_params, monomorph_cb, false);
                DEBUG("[find_impl] : " << bound.m_path.m_path << b_params_mono);

                if( bound.m_path.m_path == trait_path )
                {
                    if( H::check_params(sp, b_params_mono, trait_params) )
                    {
                        if( &b_params_mono == &params_mono_o || ::std::any_of(bound.m_type_bounds.begin(), bound.m_type_bounds.end(), [&](const auto& x){ return monomorphise_type_needed(x.second.type); }) )
                        {
                            ::HIR::TraitPath::assoc_list_t  atys;
                            if( ! bound.m_type_bounds.empty() )
                            {
                                for(const auto& tb : bound.m_type_bounds)
                                {
                                    auto src = monomorph_cb.monomorph_genericpath(sp, tb.second.source_trait, false);
                                    auto aty = monomorph_cb.monomorph_type(sp, tb.second.type, false);
                                    expand_associated_types(sp, aty);
                                    expand_associated_types_params(sp, src.m_params);
                                    atys.insert(::std::make_pair( tb.first, ::HIR::TraitPath::AtyEqual { mv$(src), mv$(aty) } ));
                                }
                            }
                            if( found_cb( ImplRef(type.clone(), mv$(params_mono_o), mv$(atys)), false ) )
                                return true;
                            params_mono_o = monomorph_cb.monomorph_path_params(sp, b_params, false);
                        }
                        else
                        {
                            if( found_cb( ImplRef(&type, &bound.m_path.m_params, &bound.m_type_bounds), false ) )
                                return true;
                        }
                    }
                }

                if(trait_params)
                {
                    return this->find_named_trait_in_trait(sp,  trait_path, *trait_params,  *bound.m_trait_ptr,  bound.m_path.m_path, b_params_mono, type,
                        [&](const auto& i_params, const auto& i_assoc) {
                            if( i_params != *trait_params )
                                return false;
                            DEBUG("impl " << trait_path << i_params << " for " << type << " -- desired " << trait_path << *trait_params);
                            return found_cb( ImplRef(type.clone(), i_params.clone(), {}), false );
                        });
                }
                else
                {
                    auto monomorph = MonomorphStatePtr(&type, &b_params_mono, nullptr);

                    for( const auto& pt : bound.m_trait_ptr->m_all_parent_traits )
                    {
                        auto pt_mono = monomorph.monomorph_traitpath(sp, pt, false);

                        //DEBUG(pt << " => " << pt_mono);
                        // TODO: When in pre-typecheck mode, this needs to be a fuzzy match (because there might be a UfcsUnknown in the
                        // monomorphed version) OR, there may be placeholders
                        if( pt.m_path.m_path == trait_path )
                        {
                            // TODO: Monomorphse trait params
                            //DEBUG("impl " << trait_path << i_params << " for " << type << " -- desired " << trait_path << *trait_params);
                            return found_cb( ImplRef(type.clone(), mv$(pt_mono.m_path.m_params), {}), false );
                        }
                    }
                    return false;
                }
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
                if( !be.type.data().is_Path() )
                    continue;
                if( !be.type.data().as_Path().path.m_data.is_UfcsKnown() )
                    continue ;
                {
                    const auto& pe2 = be.type.data().as_Path().path.m_data.as_UfcsKnown();
                    if( pe2.type != ::HIR::TypeRef("Self",GENERIC_Self) )
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
        }
        // --- /UfcsKnown ---
    }

    bool ret;

    // TODO: A bound can imply something via its associated types. How deep can this go?
    // E.g. `T: IntoIterator<Item=&u8>` implies `<T as IntoIterator>::IntoIter : Iterator<Item=&u8>`
    if( this->find_impl__bounds(sp, trait_path, trait_params, type, found_cb) )
    {
        DEBUG("Success");
        return true;
    }

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
                        [&](auto impl_params, auto cmp)->bool {
                            //rv = found_cb( ImplRef(impl_params, trait_path, impl, mv$(placeholders)), (cmp == ::HIR::Compare::Fuzzy) );
                            out_rv = found_cb(ImplRef(&type, trait_params, &null_assoc), cmp == ::HIR::Compare::Fuzzy);
                            return out_rv;
                        });
                }
                else
                {
                    return self.find_impl__check_crate_raw(sp, trait_path, trait_params, type, impl.m_params, impl.m_trait_args, impl.m_type,
                        [&](auto impl_params, auto cmp)->bool {
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
        DEBUG("Search for " << trait_path << " for " << type);
        ret = m_crate.find_trait_impls(trait_path, type, cb_ident, [&](const auto& impl) {
            return this->find_impl__check_crate(sp, trait_path, trait_params, type, found_cb,  impl);
            });
        if(ret)
            return true;

        return false;
    }
}

bool StaticTraitResolve::find_impl__bounds(
        const Span& sp,
        const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
        const ::HIR::TypeRef& type,
        t_cb_find_impl found_cb
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

    {
        auto ir = m_trait_bounds.equal_range(std::make_pair(std::ref(type), std::ref(trait_path)));
        for(auto it = ir.first; it != ir.second; ++ it)
        {
            const auto& b_type = it->first.first;
            const auto& b_params = it->first.second.m_params;
            DEBUG(b_type << ": " << trait_path << b_params);
            // Check against `params`
            if( trait_params ) {
                if( !H::compare_pp(sp, *trait_params, b_params) )
                    continue;
            }
            // Hand off to the closure, and return true if it does
            if( found_cb(ImplRef(&b_type, &b_params, &it->second.assoc), false) ) {
                return true;
            }
        }
    }

    // Obtain a pointer to UfcsKnown for magic later
    const ::HIR::Path::Data::Data_UfcsKnown* assoc_info = nullptr;
    if(const auto* e = type.data().opt_Path())
    {
        assoc_info = e->path.m_data.opt_UfcsKnown();
    }
    if(assoc_info)
    {
        auto ir = m_trait_bounds.equal_range(std::make_pair(std::ref(assoc_info->type), std::ref(assoc_info->trait.m_path)));
        for(auto it = ir.first; it != ir.second; ++ it)
        {
            const auto& bound = *it;
            const auto& b_params = it->first.second.m_params;

            if( H::compare_pp(sp, b_params, assoc_info->trait.m_params) )
            {
                const auto& trait_ref = *bound.second.trait_ptr;
                const auto& at = trait_ref.m_types.at(assoc_info->item);
                for(const auto& bound : at.m_trait_bounds)
                {
                    if( bound.m_path.m_path == trait_path && (!trait_params || H::compare_pp(sp, bound.m_path.m_params, *trait_params)) ) {
                        DEBUG("- Found an associated type impl");

                        auto tp_mono = MonomorphStatePtr(&assoc_info->type, &assoc_info->trait.m_params, nullptr).monomorph_traitpath(sp, bound, false);
                        // - Expand associated types
                        for(auto& ty : tp_mono.m_type_bounds) {
                            this->expand_associated_types(sp, ty.second.type);
                        }
                        DEBUG("- tp_mono = " << tp_mono);
                        // TODO: Instead of using `type` here, build the real type
                        if( found_cb( ImplRef(type.clone(), mv$(tp_mono.m_path.m_params), mv$(tp_mono.m_type_bounds)), false ) ) {
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

namespace {

    class GetParams:
    public ::HIR::MatchGenerics
    {
        Span    sp;
        HIR::PathParams& impl_params;
    public:
        GetParams(Span sp, const HIR::GenericParams& impl_params_def, HIR::PathParams& impl_params):
            sp(sp),
            impl_params(impl_params)
        {
            impl_params.m_types.resize( impl_params_def.m_types.size() );
            impl_params.m_values.resize( impl_params_def.m_values.size() );
        }

        ::HIR::Compare match_ty(const ::HIR::GenericRef& g, const ::HIR::TypeRef& ty, ::HIR::t_cb_resolve_type resolve_cb) override {
            ASSERT_BUG(sp, g.binding < impl_params.m_types.size(), "[GetParams] Type generic " << g << " out of bounds (" << impl_params.m_types.size() << ")");
            if( impl_params.m_types[g.binding] == HIR::TypeRef() )
            {
                impl_params.m_types[g.binding] = ty.clone();
                DEBUG("[GetParams] Set impl ty param " << g << " to " << ty);
                return ::HIR::Compare::Equal;
            }
            else {
                return impl_params.m_types[g.binding].compare_with_placeholders(sp, ty, resolve_cb);
            }
        }
        ::HIR::Compare match_val(const ::HIR::GenericRef& g, const ::HIR::ConstGeneric& sz) override {
            ASSERT_BUG(sp, g.binding < impl_params.m_values.size(), "[GetParams] Value generic " << g << " out of range (" << impl_params.m_values.size() << ")");
            if( impl_params.m_values[g.binding].is_Infer() )
            {
                impl_params.m_values[g.binding] = sz.clone();
                DEBUG("[GetParams] Set impl val param " << g << " to " << sz);
                return ::HIR::Compare::Equal;
            }
            else
            {
                if( impl_params.m_values[g.binding] != sz ) {
                    return HIR::Compare::Unequal;
                }
                else {
                    return HIR::Compare::Equal;
                }
            }
        }
    };
}

bool StaticTraitResolve::find_impl__check_crate_raw(
        const Span& sp,
        const ::HIR::SimplePath& des_trait_path, const ::HIR::PathParams* des_trait_params, const ::HIR::TypeRef& des_type,
        const ::HIR::GenericParams& impl_params_def, const ::HIR::PathParams& impl_trait_params, const ::HIR::TypeRef& impl_type,
        ::std::function<bool(HIR::PathParams, ::HIR::Compare)> found_cb
    ) const
{
    auto cb_ident = [](const auto&ty)->const ::HIR::TypeRef&{return ty;};
    TRACE_FUNCTION_F("impl" << impl_params_def.fmt_args() << " " << des_trait_path << impl_trait_params << " for " << impl_type << impl_params_def.fmt_bounds());

    // TODO: What if `des_trait_params` already has impl placeholders?

    HIR::PathParams impl_params;
    GetParams get_params { sp, impl_params_def, impl_params };

    auto match = impl_type.match_test_generics_fuzz(sp, des_type, cb_ident, get_params);
    unsigned base_impl_placeholder_idx = 0;
    if( des_trait_params )
    {
        ASSERT_BUG( sp, des_trait_params->m_types.size() == impl_trait_params.m_types.size(), "Size mismatch in arguments for " << des_trait_path << " - " << *des_trait_params << " and " << impl_trait_params );
        unsigned max_impl_idx = 0;
        for( unsigned int i = 0; i < impl_trait_params.m_types.size(); i ++ )
        {
            const auto& l = impl_trait_params.m_types[i];
            const auto& r = des_trait_params->m_types[i];
            match &= l.match_test_generics_fuzz(sp, r, cb_ident, get_params);

            visit_ty_with(r, [&](const ::HIR::TypeRef& t)->bool {
                if( t.data().is_Generic() && (t.data().as_Generic().binding >> 8) == 2 ) {
                    unsigned impl_idx = t.data().as_Generic().binding & 0xFF;
                    max_impl_idx = ::std::max(max_impl_idx, impl_idx+1);
                }
                return false;
                });
        }
        base_impl_placeholder_idx = max_impl_idx;

        size_t n_placeholders_needed = 0;
        for(unsigned int i = 0; i < impl_params.m_types.size(); i ++ ) {
            if( impl_params.m_types[i] == HIR::TypeRef() ) {
                n_placeholders_needed ++;
            }
        }
        ASSERT_BUG(sp, base_impl_placeholder_idx + n_placeholders_needed <= 256, "Out of impl placeholders");
    }
    if( match == ::HIR::Compare::Unequal ) {
        DEBUG(" > Type mismatch");
        return false;
    }

    auto placeholder_name = RcString::new_interned(FMT("impl_?_" << &impl_params_def));
    std::vector<HIR::TypeRef>   placeholders;
    for(unsigned int i = 0; i < impl_params.m_types.size(); i ++ ) {
        if( impl_params.m_types[i] == HIR::TypeRef() )
        {
            if( placeholders.size() == 0 )
                placeholders.resize(impl_params.m_types.size());
            placeholders[i] = ::HIR::TypeRef(placeholder_name, 2*256 + i + base_impl_placeholder_idx);
            DEBUG("Placeholder " << placeholders[i] << " for I:" << i << " " << impl_params_def.m_types[i].m_name);
        }
    }
    for(size_t i = 0; i < impl_params.m_values.size(); i ++ ) {
        if( impl_params.m_values[i] == HIR::ConstGeneric() )
        {
            // TODO: Is there an equivalent of a placeholder for const generics?
            // - Yes, it's a placeholder generic :D
            // TODO: use placeholder generics for values
        }
    }

    struct Matcher:
        public ::HIR::MatchGenerics,
        public Monomorphiser
    {
        Span    sp;
        const HIR::PathParams& impl_params;
        unsigned    base_impl_placeholder_idx;
        RcString    placeholder_name;
        std::vector<HIR::TypeRef>&  placeholders;
        Matcher(Span sp, const HIR::PathParams& impl_params, RcString placeholder_name, unsigned base_impl_placeholder_idx, std::vector<HIR::TypeRef>& placeholders):
            sp(sp),
            impl_params(impl_params),
            base_impl_placeholder_idx(base_impl_placeholder_idx),
            placeholder_name(placeholder_name),
            placeholders(placeholders)
        {
        }

        ::HIR::Compare match_ty(const ::HIR::GenericRef& g, const ::HIR::TypeRef& ty, ::HIR::t_cb_resolve_type resolve_cb) override {
            if( ty.data().is_Generic() && ty.data().as_Generic().binding == g.binding)
                return ::HIR::Compare::Equal;
            if( g.is_placeholder() )
            {
                if( g.idx() >= base_impl_placeholder_idx )
                {
                    auto i = g.idx() - base_impl_placeholder_idx;
                    ASSERT_BUG(sp, impl_params.m_types[i] == HIR::TypeRef(), "Placeholder to populated type returned. new " << ty << ", existing " << impl_params.m_types[i]);
                    auto& ph = placeholders[i];
                    if( ph.data().is_Generic() && ph.data().as_Generic() == g ) {
                        DEBUG("[find_impl__check_crate_raw] Bind placeholder " << i << " to " << ty);
                        ph = ty.clone();
                        return ::HIR::Compare::Equal;
                    }
                    else if( ph == ty ) {
                        return ::HIR::Compare::Equal;
                    }
                    else {
                        TODO(sp, "[find_impl__check_crate_raw] Compare placeholder " << i << " " << ph << " == " << ty);
                    }
                }
                else {
                    return ::HIR::Compare::Fuzzy;
                }
            }
            else {
                return ::HIR::Compare::Unequal;
            }
        }
        ::HIR::Compare match_val(const ::HIR::GenericRef& g, const ::HIR::ConstGeneric& sz) override {
            TODO(Span(), "Matcher::match_val " << g << " with " << sz);
        }

        ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& ge) const override {
            if( ge.is_self() ) {
                // TODO: `impl_type` or `des_type`
            //    DEBUG("[find_impl__check_crate_raw] Self - " << impl_type << " or " << des_type);
                //TODO(sp, "[find_impl__check_crate_raw] Self - " << impl_type << " or " << des_type);
            //    return impl_type;
                TODO(sp, "get_type Self");
            }
            ASSERT_BUG(sp, !ge.is_placeholder(), "[find_impl__check_crate_raw] Placeholder param seen - " << ge);
            if( impl_params.m_types.at(ge.binding) != HIR::TypeRef() ) {
                return impl_params.m_types.at(ge.binding).clone();
            }
            return placeholders.at(ge.binding).clone();
        }
        ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& val) const override {
            ASSERT_BUG(sp, val.binding < 256, "Generic value binding in " << val << " out of range (>=256)");
            ASSERT_BUG(sp, val.binding < impl_params.m_values.size(), "Generic value binding in " << val << " out of range (>= " << impl_params.m_values.size() << ")");
            if( !impl_params.m_values.at(val.binding).is_Infer() ) {
                return impl_params.m_values.at(val.binding).clone();
            }
            //ASSERT_BUG(sp, placeholders.m_values.size() == impl_params.m_values.size(), "Placeholder size mismatch: " << placeholders.m_values.size() << " != " << impl_params.m_values.size());
            //return placeholders.m_values.at(val.binding).clone();
            TODO(sp, "Value placeholders");
        }
    };
    Matcher matcher { sp, impl_params, placeholder_name, base_impl_placeholder_idx, placeholders };

    // Bounds
    for(const auto& bound : impl_params_def.m_bounds)
    {
        if( const auto* ep = bound.opt_TraitBound() )
        {
            const auto& e = *ep;

            DEBUG("Trait bound " << e.type << " : " << e.trait);
            auto b_ty_mono = matcher.monomorph_type(sp, e.type);
            this->expand_associated_types(sp, b_ty_mono);
            auto b_tp_mono = matcher.monomorph_traitpath(sp, e.trait, false);
            expand_associated_types_tp(sp, b_tp_mono);
            DEBUG("- b_ty_mono = " << b_ty_mono << ", b_tp_mono = " << b_tp_mono);
            // HACK: If the type is '_', assume the bound passes
            if( b_ty_mono.data().is_Infer() ) {
                continue ;
            }

            // TODO: This is extrememly inefficient (looks up the trait impl 1+N times)
            if( b_tp_mono.m_type_bounds.size() > 0 )
            {
                //
                for(const auto& assoc_bound : b_tp_mono.m_type_bounds) {
                    const auto& aty_name = assoc_bound.first;
                    const ::HIR::TypeRef& exp = assoc_bound.second.type;

                    // TODO: use `assoc_bound.second.source_trait`
                    ::HIR::GenericPath  aty_src_trait;
                    trait_contains_type(sp, b_tp_mono.m_path, *e.trait.m_trait_ptr, aty_name.c_str(), aty_src_trait);

                    bool rv = false;
                    if( b_ty_mono.data().is_Generic() && b_ty_mono.data().as_Generic().is_placeholder() ) {
                        DEBUG("- Placeholder param " << b_ty_mono << ", magic success");
                        rv = true;
                    }
                    else {
                        rv = this->find_impl(sp, aty_src_trait.m_path, aty_src_trait.m_params, b_ty_mono, [&](const ImplRef& impl, bool)->bool {
                            ::HIR::TypeRef have = impl.get_type(aty_name.c_str());
                            if( have == HIR::TypeRef() ) {
                                have = HIR::TypeRef::new_path(HIR::Path(impl.get_impl_type(), HIR::GenericPath(aty_src_trait.m_path, impl.get_trait_params()), aty_name), HIR::TypePathBinding::make_Unbound({}));
                            }
                            this->expand_associated_types(sp, have);

                            DEBUG("[find_impl__check_crate_raw] ATY ::" << aty_name << " - " << have << " ?= " << exp);
                            //auto cmp = have .match_test_generics_fuzz(sp, exp, cb_ident, matcher);
                            auto cmp = exp .match_test_generics_fuzz(sp, have, cb_ident, matcher);
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
                if( b_ty_mono.data().is_Generic() && b_ty_mono.data().as_Generic().is_placeholder() ) {
                    DEBUG("- Placeholder param " << b_ty_mono << ", magic success");
                    rv = true;
                }
                else {
                    rv = this->find_impl(sp, b_tp_mono.m_path.m_path, b_tp_mono.m_path.m_params, b_ty_mono, [&](const auto& impl, bool) {
                        return true;
                        });
                }
                if(!rv && visit_ty_with(b_ty_mono, [](const HIR::TypeRef& ty){ return ty.data().is_Generic() && ty.data().as_Generic().is_placeholder(); }) ) {
                    DEBUG("- Placeholder param within " << b_ty_mono << ", magic success");
                    rv = true;
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

    for(size_t i = 0; i < impl_params.m_types.size(); i ++)
    {
        if( impl_params.m_types[i] == HIR::TypeRef() )
        {
            impl_params.m_types[i] = std::move(placeholders[i]);
        }
        ASSERT_BUG(sp, impl_params.m_types[i] != HIR::TypeRef(), "Impl parameter #" << i << " wasn't set (or even a placeholder)");
    }

    assert(impl_params_def.m_types.size() == impl_params.m_types.size());
    for(size_t i = 0; i < impl_params_def.m_types.size(); i ++)
    {
        if( impl_params_def.m_types.at(i).m_is_sized )
        {
            if( !type_is_sized(sp, impl_params.m_types[i]) )
            {
                DEBUG("- Sized bound failed for " << impl_params.m_types[i]);
                return false;
            }
        }
    }

    return found_cb( mv$(impl_params), match );
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
        [&](auto impl_params, auto match) {
            return found_cb( ImplRef(mv$(impl_params), m_crate.get_trait_by_path(sp, trait_path), trait_path, impl), (match == ::HIR::Compare::Fuzzy) );
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
    if( const auto* ep = type.data().opt_Path() )
    {
        const auto& e = *ep;
        ::HIR::Compare  res = ::HIR::Compare::Equal;
        TU_MATCH_HDRA( (e.path.m_data), {)
        TU_ARMA(Generic, pe) {
            ::HIR::TypeRef  tmp;
            auto monomorph = MonomorphStatePtr(nullptr, &pe.m_params, nullptr);
            // HELPER: Get a possibily monomorphised version of the input type (stored in `tmp` if needed)
            auto monomorph_get = [&](const auto& ty)->const ::HIR::TypeRef& {
                return this->monomorph_expand_opt(sp, tmp, ty, monomorph);
                };
            
            TU_MATCH_HDRA( (e.binding), {)
            TU_ARMA(Opaque, tpb) {
                BUG(sp, "Opaque binding on generic path - " << type);
                }
            TU_ARMA(Unbound, tpb) {
                BUG(sp, "Unbound binding on generic path - " << type);
                }
            TU_ARMA(Struct, tpb) {
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
                }
            TU_ARMA(Enum, tpb) {
                if( const auto* e = tpb->m_data.opt_Data() )
                {
                    for(const auto& var : *e)
                    {
                        const auto& fld_ty_mono = monomorph_get(var.type);
                        DEBUG("Enum '" << var.name << "'" << fld_ty_mono);
                        res &= type_impls_trait(fld_ty_mono);
                        if( res == ::HIR::Compare::Unequal )
                            return ::HIR::Compare::Unequal;
                    }
                }
                }
            TU_ARMA(Union, tpb) {
                for(const auto& fld : tpb->m_variants)
                {
                    const auto& fld_ty_mono = monomorph_get(fld.second.ent);
                    DEBUG("Union '" << fld.first << "' " << fld_ty_mono);
                    res &= type_impls_trait(fld_ty_mono);
                    if( res == ::HIR::Compare::Unequal )
                        return ::HIR::Compare::Unequal;
                }
                }
            TU_ARMA(ExternType, tpb) {
                TODO(sp, "Check auto trait destructure on extern type " << type);
                }
            }
            DEBUG("- Nothing failed, calling callback");
            }
        TU_ARMA(UfcsUnknown, pe) {
            BUG(sp, "UfcsUnknown in typeck - " << type);
            }
        TU_ARMA(UfcsKnown, pe) {
            TODO(sp, "Check trait bounds for bound on UfcsKnown " << type);
            }
        TU_ARMA(UfcsInherent, pe) {
            TODO(sp, "Auto trait lookup on UFCS Inherent type");
            }
        }
        return res;
    }
    else if( const auto* ep = type.data().opt_Tuple() )
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
    else if( const auto* e = type.data().opt_Array() )
    {
        return type_impls_trait(e->inner);
    }
    // Otherwise, there's no negative so it must be positive
    else {
        return ::HIR::Compare::Equal;
    }
}

void StaticTraitResolve::expand_associated_types(const Span& sp, ::HIR::TypeRef& input) const
{
    TRACE_FUNCTION_FR(input, input);
    this->expand_associated_types_inner(sp, input);
}
void StaticTraitResolve::expand_associated_types_path(const Span& sp, ::HIR::Path& input) const
{
    TRACE_FUNCTION_FR(input, input);
    TU_MATCH_HDRA( (input.m_data), { )
    TU_ARMA(Generic, e2) {
        this->expand_associated_types_params(sp, e2.m_params);
        }
    TU_ARMA(UfcsInherent, e2) {
        this->expand_associated_types_inner(sp, e2.type);
        this->expand_associated_types_params(sp, e2.params);
        // TODO: impl params too?
        for(auto& arg : e2.impl_params.m_types)
            this->expand_associated_types_inner(sp, arg);
        }
    TU_ARMA(UfcsKnown, e2) {
        this->expand_associated_types_inner(sp, e2.type);
        this->expand_associated_types_params(sp, e2.trait.m_params);
        this->expand_associated_types_params(sp, e2.params);
        }
    TU_ARMA(UfcsUnknown, e2) {
        BUG(sp, "Encountered UfcsUnknown in EAT - " << input);
        }
    }
}
bool StaticTraitResolve::expand_associated_types_single(const Span& sp, ::HIR::TypeRef& input) const
{
    TRACE_FUNCTION_F(input);
    if( input.data().is_Path() && input.data().as_Path().path.m_data.is_UfcsKnown() )
    {
        return expand_associated_types__UfcsKnown(sp, input, /*recurse=*/false);
    }
    else
    {
        return false;
    }
}
void StaticTraitResolve::expand_associated_types_params(const Span& sp, ::HIR::PathParams& params) const
{
    for(auto& arg : params.m_types)
        this->expand_associated_types_inner(sp, arg);
}
void StaticTraitResolve::expand_associated_types_tp(const Span& sp, ::HIR::TraitPath& input) const
{
    expand_associated_types_params(sp, input.m_path.m_params);
    for(auto& arg : input.m_type_bounds)
    {
        this->expand_associated_types_params(sp, arg.second.source_trait.m_params);
        this->expand_associated_types_inner(sp, arg.second.type);
    }
    for(auto& arg : input.m_trait_bounds)
    {
        this->expand_associated_types_params(sp, arg.second.source_trait.m_params);
        for(auto& t : arg.second.traits)
            this->expand_associated_types_tp(sp, t);
    }
}
void StaticTraitResolve::expand_associated_types_inner(const Span& sp, ::HIR::TypeRef& input) const
{
    // TODO: use visit_ty_with instead?
    TU_MATCH_HDRA( (input.data_mut()), {)
    TU_ARMA(Infer, e) {
        //if( m_treat_ivars_as_bugs ) {
        //    BUG(sp, "Encountered inferrence variable in static context");
        //}
        }
    TU_ARMA(Diverge, e) {
        }
    TU_ARMA(Primitive, e) {
        }
    TU_ARMA(Path, e) {
        TU_MATCH_HDRA( (e.path.m_data), { )
        TU_ARMA(Generic, e2) {
            expand_associated_types_params(sp, e2.m_params);
            }
        TU_ARMA(UfcsInherent, e2) {
            this->expand_associated_types_inner(sp, e2.type);
            expand_associated_types_params(sp, e2.params);
            // TODO: impl params too?
            for(auto& arg : e2.impl_params.m_types)
                this->expand_associated_types_inner(sp, arg);
            }
        TU_ARMA(UfcsKnown, e2) {
            // - Only try resolving if the binding isn't known
            if( !e.binding.is_Unbound() )
                return ;
            auto it = m_aty_cache.find(e.path);
            if( it != m_aty_cache.end() )
            {
                DEBUG("Cached " << it->second);
                input = it->second.clone();
            }
            else
            {
                auto p = e.path.clone();
                this->expand_associated_types__UfcsKnown(sp, input);
                m_aty_cache.insert(std::make_pair( std::move(p), input.clone() ));
            }
            return;
            }
        TU_ARMA(UfcsUnknown, e2) {
            BUG(sp, "Encountered UfcsUnknown in EAT - " << e.path);
            }
        }
        }
    TU_ARMA(Generic, e) {
        }
    TU_ARMA(TraitObject, e) {
        expand_associated_types_tp(sp, e.m_trait);
        for(auto& m : e.m_markers)
            expand_associated_types_params(sp, m.m_params);
        }
    TU_ARMA(ErasedType, e) {
        // Recurse?
        }
    TU_ARMA(Array, e) {
        expand_associated_types_inner(sp, e.inner);
        }
    TU_ARMA(Slice, e) {
        expand_associated_types_inner(sp, e.inner);
        }
    TU_ARMA(Tuple, e) {
        for(auto& sub : e) {
            expand_associated_types_inner(sp, sub);
        }
        }
    TU_ARMA(Borrow, e) {
        expand_associated_types_inner(sp, e.inner);
        }
    TU_ARMA(Pointer, e) {
        expand_associated_types_inner(sp, e.inner);
        }
    TU_ARMA(Function, e) {
        // Recurse?
        for(auto& ty : e.m_arg_types)
            expand_associated_types_inner(sp, ty);
        expand_associated_types_inner(sp, e.m_rettype);
        }
    TU_ARMA(Closure, e) {
        // Recurse?
        for(auto& ty : e.m_arg_types)
            expand_associated_types_inner(sp, ty);
        expand_associated_types_inner(sp, e.m_rettype);
        }
    TU_ARMA(Generator, e) {
        // TODO: Call into the node?
        // - This should never be monomorphed, so useless?
        }
    }
}
bool StaticTraitResolve::expand_associated_types__UfcsKnown(const Span& sp, ::HIR::TypeRef& input, bool recurse/*=true*/) const
{
    TRACE_FUNCTION_FR(input, input);
    auto& e = input.data_mut().as_Path();
    auto& e2 = e.path.m_data.as_UfcsKnown();

    this->expand_associated_types_inner(sp, e2.type);
    for(auto& arg : e2.trait.m_params.m_types)
        this->expand_associated_types_inner(sp, arg);

    DEBUG("Locating associated type for " << e.path);

    TU_MATCH_HDRA( (e2.type.data()), {)
    default:
        // Nothing special
        break;
    TU_ARMA(Infer, te) {
        DEBUG("Infer seen in static EAT, leaving as-is");
        return false;
        }
    // - If it's a closure, then the only trait impls are those generated by typeck
    TU_ARMA(Closure, te) {
        //if( te.node->m_obj_path == ::HIR::GenericPath() )
        //{
            if( e2.trait.m_path == m_lang_Fn || e2.trait.m_path == m_lang_FnMut || e2.trait.m_path == m_lang_FnOnce  ) {
                if( e2.item == "Output" ) {
                    input = te.m_rettype.clone();
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
        }
    // If it's a TraitObject, then maybe we're asking for a bound
    TU_ARMA(TraitObject, te) {
        const auto& data_trait = te.m_trait.m_path;
        if( e2.trait.m_path == data_trait.m_path ) {
            if( e2.trait.m_params == data_trait.m_params )
            {
                auto it = te.m_trait.m_type_bounds.find( e2.item );
                if( it == te.m_trait.m_type_bounds.end() ) {
                    // TODO: Mark as opaque and return.
                    // - Why opaque? It's not bounded, don't even bother
                    TODO(sp, "Handle unconstrained associate type " << e2.item << " from " << e2.type);
                }

                input = it->second.type.clone();
                return true;
            }
        }
        }
    // TODO: ErasedType? Does that need a bounds check?
    }

    // 1. Bounds
    bool rv = false;
    bool assume_opaque = true;
    if(!rv)
    {
        auto it = m_type_equalities.find(input);
        if( it != m_type_equalities.end() )
        {
            DEBUG("Equality " << input << " -> " << it->second);
            input = it->second.clone();
            rv = true;
            assume_opaque = false;
        }
    }
    if(!rv)
    {
        for(const auto& bound : m_trait_bounds)
        {
            const auto& be_type = bound.first.first;
            const auto& be_trait = bound.first.second;

            DEBUG("Trait bound - " << be_type << " : " << be_trait);
            // 1. Check if the type matches
            //  - TODO: This should be a fuzzier match?
            if( be_type != e2.type )
                continue;
            // 2. Check if the trait (or any supertrait) includes e2.trait
            if( be_trait == e2.trait ) {
                auto it = bound.second.assoc.find(e2.item);
                // 1. Check if the bounds include the desired item
                if( it == bound.second.assoc.end() ) {
                    // If not, assume it's opaque and return as such
                    // TODO: What happens if there's two bounds that overlap? 'F: FnMut<()>, F: FnOnce<(), Output=Bar>'
                    DEBUG("Found impl for " << input << " but no bound on item, assuming opaque");
                }
                else {
                    assume_opaque = false;
                    input = it->second.type.clone();
                }
                rv = true;
                break;
            }
        }
    }
    if( rv ) {
        if( assume_opaque ) {
            input.data_mut().as_Path().binding = ::HIR::TypePathBinding::make_Opaque({});
            ASSERT_BUG(sp, monomorphise_type_needed(input), "Set opaque on a non-generic type: " << input);
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
    if(const auto* te_inner = e2.type.data().opt_Path())
    {
        if(const auto* pe_inner_p = te_inner->path.m_data.opt_UfcsKnown())
        {
            const auto& pe_inner = *pe_inner_p;
            // TODO: Search for equality bounds on this associated type (e3) that match the entire type (e2)
            // - Does simplification of complex associated types
            const auto& trait_ptr = this->m_crate.get_trait_by_path(sp, pe_inner.trait.m_path);
            const auto& assoc_ty = trait_ptr.m_types.at(pe_inner.item);

            DEBUG("Inner UfcsKnown");

            // Resolve where Self=pe_inner.type (i.e. for the trait this inner UFCS is on)
            auto cb_placeholders_trait = MonomorphStatePtr(&pe_inner.type, &pe_inner.trait.m_params, nullptr);
            for(const auto& bound : assoc_ty.m_trait_bounds)
            {
                // If the bound is for Self and the outer trait
                // - TODO: Parameters?
                if( bound.m_path == e2.trait ) {
                    auto it = bound.m_type_bounds.find( e2.item );
                    if( it != bound.m_type_bounds.end() ) {
                        DEBUG("Found inner bound: " << it->second.type);
                        if( monomorphise_type_needed(it->second.type) ) {
                            input = cb_placeholders_trait.monomorph_type(sp, it->second.type);
                        }
                        else {
                            input = it->second.type.clone();
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
                        bound_trait, bound.m_path.m_path,bound.m_path.m_params, e2.type,
                        [&](const auto& params, const auto& assoc){
                            auto it = assoc.find(e2.item);
                            if( it != assoc.end() ) {
                                input = it->second.type.clone();
                                return true;
                            }
                            return false;
                        }
                        );
                if( replaced ) {
                    if( recurse )
                        this->expand_associated_types(sp, input);
                    return true;
                }
            }
            DEBUG("e2 = " << e2.type << ", input = " << input);
        }
    }

    // 2. Crate-level impls

    // - Search for the actual trait containing this associated type
    ::HIR::GenericPath  trait_path;
    if( !this->trait_contains_type(sp, e2.trait, this->m_crate.get_trait_by_path(sp, e2.trait.m_path), e2.item.c_str(), trait_path) )
        BUG(sp, "Cannot find associated type " << e2.item << " anywhere in trait " << e2.trait);
    //e2.trait = mv$(trait_path);

    bool replacement_happened = true;
    ::ImplRef  best_impl;
    rv = this->find_impl(sp, trait_path.m_path, trait_path.m_params, e2.type, [&](ImplRef impl, bool fuzzy) {
        DEBUG("[expand_associated_types] Found " << impl);
        // If a fuzzy match was found, monomorphise and EAT the checked types and try again
        // - A fuzzy can be caused by an opaque match.
        // - TODO: Move this logic into `find_impl`
        if( fuzzy ) {
            auto cb_ident = [](const ::HIR::TypeRef& x)->const ::HIR::TypeRef& { return x; };
            DEBUG("[expand_associated_types] - Fuzzy, monomorph+expand and recheck");

            auto impl_ty = impl.get_impl_type();
            this->expand_associated_types(sp, impl_ty);
            if(impl_ty != e2.type) {
                DEBUG("[expand_associated_types] - Fuzzy - impl type doesn't match: " << impl_ty << " != " << e2.type);
                return false;
            }
            auto pp = impl.get_trait_params();
            for(auto& ty : pp.m_types)
                this->expand_associated_types(sp, ty);
            if( pp.compare_with_placeholders(sp, trait_path.m_params, cb_ident) == HIR::Compare::Unequal ) {
                DEBUG("[expand_associated_types] - Fuzzy - params don't match: " << pp << " != " << trait_path.m_params);
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
            if( nt != ::HIR::TypeRef() ) {
                DEBUG("Converted UfcsKnown - " << e.path << " = " << nt);
                if( input == nt ) {
                    replacement_happened = false;
                    return true;
                }
                input = mv$(nt);
                replacement_happened = true;
            }
            else {
                DEBUG("Mark  " << e.path << " as opaque");
                e.binding = ::HIR::TypePathBinding::make_Opaque({});
                ASSERT_BUG(sp, monomorphise_type_needed(input), "Set opaque on a non-generic type: " << input);
                replacement_happened = this->replace_equalities(input);
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
        e.binding = ::HIR::TypePathBinding::make_Opaque({});
        ASSERT_BUG(sp, monomorphise_type_needed(input), "Set opaque on a non-generic type: " << input);
        this->replace_equalities(input);
        DEBUG("- Couldn't find a non-specialised impl of " << trait_path << " for " << e2.type << " - treating as opaque");
        return false;
    }

    ERROR(sp, E0000, "Cannot find an implementation of " << trait_path << " for " << e2.type);
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

        if( ! be.type.data().is_Path() )   continue ;
        if( ! be.type.data().as_Path().binding.is_Opaque() )   continue ;

        const auto& be_type_pe = be.type.data().as_Path().path.m_data.as_UfcsKnown();
        if( be_type_pe.type != ::HIR::TypeRef("Self", 0xFFFF) )
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
        ::std::function<bool(const ::HIR::PathParams&, ::HIR::TraitPath::assoc_list_t)> callback
    ) const
{
    TRACE_FUNCTION_F(des << des_params << " from " << trait_path << pp);
    if( pp.m_types.size() != trait_ptr.m_params.m_types.size() ) {
        BUG(sp, "Incorrect number of parameters for trait - " << trait_path << pp);
    }
    
    auto monomorph = MonomorphStatePtr(&target_type, &pp, nullptr);

    for( const auto& pt : trait_ptr.m_all_parent_traits )
    {
        auto pt_mono = monomorph.monomorph_traitpath(sp, pt, false);

        DEBUG(pt << " => " << pt_mono);
        // TODO: When in pre-typecheck mode, this needs to be a fuzzy match (because there might be a UfcsUnknown in the
        // monomorphed version) OR, there may be placeholders
        if( pt.m_path.m_path == des )
        {
            auto cmp = pt_mono.m_path.m_params.compare_with_placeholders(sp, des_params, [](const auto& t)->const ::HIR::TypeRef&{return t;});
            // pt_mono.m_path.m_params == des_params )
            if( cmp != ::HIR::Compare::Unequal )
            {
                return callback( pt_mono.m_path.m_params, mv$(pt_mono.m_type_bounds) );
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

    auto monomorph = MonomorphStatePtr(nullptr, &trait_path.m_params, nullptr);
    for(const auto& st : trait_ptr.m_all_parent_traits)
    {
        if( st.m_trait_ptr->m_types.count(name) )
        {
            out_path.m_path = st.m_path.m_path;
            out_path.m_params = monomorph.monomorph_path_params(sp, st.m_path.m_params, false);
            return true;
        }
    }
    return false;
}

bool StaticTraitResolve::type_is_copy(const Span& sp, const ::HIR::TypeRef& ty) const
{
    TU_MATCH_HDRA( (ty.data()), {)
    TU_ARMA(Generic, e) {
        {
            auto it = m_copy_cache.find(ty);
            if( it != m_copy_cache.end() )
            {
                return it->second;
            }
        }
        auto pp = ::HIR::PathParams();
        bool rv = this->find_impl__bounds(sp, m_lang_Copy, &pp, ty, [&](auto , bool ){ return true; });
        m_copy_cache.insert(::std::make_pair( ty.clone(), rv ));
        return rv;
        }
    TU_ARMA(Path, e) {
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
        }
    TU_ARMA(Diverge, e) {
        // The ! type is kinda Copy ...
        return true;
        }
    TU_ARMA(Closure, e) {
        if( TARGETVER_LEAST_1_29 )
        {
            // TODO: Auto-gerated impls
            return e.node->m_is_copy;
        }
        return false;
        }
    TU_ARMA(Generator, e) {
        // NOTE: Generators aren't Copy
        return false;
        }
    TU_ARMA(Infer, e) {
        // Shouldn't be hit
        return false;
        }
    TU_ARMA(Borrow, e) {
        // Only shared &-ptrs are copy
        return (e.type == ::HIR::BorrowType::Shared);
        }
    TU_ARMA(Pointer, e) {
        // All raw pointers are Copy
        return true;
        }
    TU_ARMA(Function, e) {
        // All function pointers are Copy
        return true;
        }
    TU_ARMA(Primitive, e) {
        // All primitives (except the unsized `str`) are Copy
        return e != ::HIR::CoreType::Str;
        }
    TU_ARMA(Array, e) {
        // TODO: Why is `[T; 0]` treated as `Copy`?
        if( TU_TEST1(e.size, Known, == 0) )
            return true;
        return type_is_copy(sp, e.inner);
        }
    TU_ARMA(Slice, e) {
        // [T] isn't Sized, so isn't Copy ether
        return false;
        }
    TU_ARMA(TraitObject, e) {
        // (Trait) isn't Sized, so isn't Copy ether
        return false;
        }
    TU_ARMA(ErasedType, e) {
        for(const auto& trait : e.m_traits)
        {
            if( find_named_trait_in_trait(sp, m_lang_Copy, {},  *trait.m_trait_ptr, trait.m_path.m_path, trait.m_path.m_params,  ty, [](const auto&, auto ){ return true; }) ) {
                return true;
            }
        }
        return false;
        }
    TU_ARMA(Tuple, e) {
        for(const auto& ty : e)
            if( !type_is_copy(sp, ty) )
                return false;
        return true;
        }
    }
    throw "";
}
bool StaticTraitResolve::type_is_clone(const Span& sp, const ::HIR::TypeRef& ty) const
{
    if( !TARGETVER_LEAST_1_29 )   BUG(sp, "Calling type_is_clone when not in >=1.29 mode");
    
    TU_MATCH_HDRA( (ty.data()), {)
    TU_ARMA(Generic, e) {
        {
            auto it = m_clone_cache.find(ty);
            if( it != m_clone_cache.end() )
            {
                return it->second;
            }
        }
        auto pp = ::HIR::PathParams();
        bool rv = this->find_impl__bounds(sp, m_lang_Clone, &pp, ty, [&](auto , bool ){ return true; });
        m_clone_cache.insert(::std::make_pair( ty.clone(), rv ));
        return rv;
        }
    TU_ARMA(Path, e) {
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
        }
    TU_ARMA(Diverge, e) {
        // The ! type is kinda Copy/Clone ...
        return true;
        }
    TU_ARMA(Closure, e) {
        if( TARGETVER_LEAST_1_29 )
        {
            return e.node->m_is_copy;
        }
        return false;
        }
    TU_ARMA(Generator, e) {
        TODO(sp, "type_is_clone - Generator");
        }
    TU_ARMA(Infer, e) {
        // Shouldn't be hit
        return false;
        }
    TU_ARMA(Borrow, e) {
        // Only shared &-ptrs are copy/clone
        return (e.type == ::HIR::BorrowType::Shared);
        }
    TU_ARMA(Pointer, e) {
        // All raw pointers are Copy/Clone
        return true;
        }
    TU_ARMA(Function, e) {
        // All function pointers are Copy/Clone
        return true;
        }
    TU_ARMA(Primitive, e) {
        // All primitives (except the unsized `str`) are Copy/Clone
        return e != ::HIR::CoreType::Str;
        }
    TU_ARMA(Array, e) {
        return e.size.as_Known() == 0 || type_is_clone(sp, e.inner);
        }
    TU_ARMA(Slice, e) {
        // [T] isn't Sized, so isn't Copy ether
        return false;
        }
    TU_ARMA(TraitObject, e) {
        // (Trait) isn't Sized, so isn't Copy ether
        return false;
        }
    TU_ARMA(ErasedType, e) {
        for(const auto& trait : e.m_traits)
        {
            if( find_named_trait_in_trait(sp, m_lang_Clone, {},  *trait.m_trait_ptr, trait.m_path.m_path, trait.m_path.m_params,  ty, [](const auto&, auto ){ return true; }) ) {
                return true;
            }
        }
        return false;
        }
    TU_ARMA(Tuple, e) {
        for(const auto& ty : e)
            if( !type_is_clone(sp, ty) )
                return false;
        return true;
        }
    }
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
    TU_MATCH_HDRA( (ty.data()), {)
        break;
    default:
        return false;
    TU_ARMA(Diverge, _e)
        return true;
    TU_ARMA(Path, e) {
        TU_MATCH_HDRA( (e.binding), {)
        TU_ARMA(Unbound, pbe) {
            // BUG?
            return false;
            }
        TU_ARMA(Opaque, pbe) {
            // TODO: This can only be with UfcsKnown, so check if the trait specifies ?Sized
            return false;
            }
        TU_ARMA(Struct, pbe) {
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
                    const auto& ty = this->monomorph_expand_opt(sp, tmp, tpl, MonomorphStatePtr(nullptr, &params, nullptr));
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
            }
        TU_ARMA(Enum, pbe) {
            // TODO: Check all variants.
            TODO(sp, "type_is_impossible for enum " << ty);
            }
        TU_ARMA(Union, pbe) {
            // TODO: Check all variants? Or just one?
            TODO(sp, "type_is_impossible for union " << ty);
            }
        TU_ARMA(ExternType, pbe) {
            // Extern types are possible, just not usable
            return false;
            }
        }
        return true;
        }
    TU_ARMA(Borrow, e)
        return type_is_impossible(sp, e.inner);
    TU_ARMA(Pointer, e) {
        return false;
        //return type_is_impossible(sp, e.inner);
        }
    TU_ARMA(Function, e) {
        // TODO: Check all arguments?
        return true;
        }
    TU_ARMA(Array, e) {
        return type_is_impossible(sp, e.inner);
        }
    TU_ARMA(Slice, e) {
        return type_is_impossible(sp, e.inner);
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

    ASSERT_BUG(sp, !dst_ty.data().is_Infer(), "_ seen after inferrence - " << dst_ty);
    ASSERT_BUG(sp, !src_ty.data().is_Infer(), "_ seen after inferrence - " << src_ty);

    {
        //ASSERT_BUG(sp, dst_ty != src_ty, "Equal types for can_unsize - " << dst_ty << " <-" << src_ty );
        if( dst_ty == src_ty )
            return true;
    }

    auto ir = m_trait_bounds.equal_range(std::make_pair(std::ref(src_ty), std::ref(m_lang_Unsize)));
    for(auto it = ir.first; it != ir.second; ++ it)
    {
        const auto& be_dst = it->first.second.m_params.m_types.at(0);

        if( dst_ty == be_dst ) {
            DEBUG("Found bounded");
            return ::HIR::Compare::Equal;
        }
    }

    // Associated types, check the bounds in the trait.
    if( src_ty.data().is_Path() && src_ty.data().as_Path().path.m_data.is_UfcsKnown() )
    {
        const auto& pe = src_ty.data().as_Path().path.m_data.as_UfcsKnown();
        auto ms = MonomorphStatePtr(&pe.type, &pe.trait.m_params, nullptr);
        auto found_bound = this->iterate_aty_bounds(sp, pe, [&](const ::HIR::TraitPath& bound) {
            if( bound.m_path.m_path != m_lang_Unsize )
                return false;
            const auto& be_dst_tpl = bound.m_path.m_params.m_types.at(0);
            ::HIR::TypeRef  tmp_ty;
            const auto& be_dst = ms.maybe_monomorph_type(sp, tmp_ty, be_dst_tpl);

            if( dst_ty != be_dst )  return false;
            return true;
            });
        if( found_bound )
        {
            return true;
        }
    }

    // Struct<..., T, ...>: Unsize<Struct<..., U, ...>>
    if( dst_ty.data().is_Path() && src_ty.data().is_Path() )
    {
        bool dst_is_unsizable = dst_ty.data().as_Path().binding.is_Struct() && dst_ty.data().as_Path().binding.as_Struct()->m_struct_markings.can_unsize;
        bool src_is_unsizable = src_ty.data().as_Path().binding.is_Struct() && src_ty.data().as_Path().binding.as_Struct()->m_struct_markings.can_unsize;
        if( dst_is_unsizable || src_is_unsizable )
        {
            DEBUG("Struct unsize? " << dst_ty << " <- " << src_ty);
            const auto& str = *dst_ty.data().as_Path().binding.as_Struct();
            const auto& dst_gp = dst_ty.data().as_Path().path.m_data.as_Generic();
            const auto& src_gp = src_ty.data().as_Path().path.m_data.as_Generic();

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
    if( const auto* de = dst_ty.data().opt_TraitObject() )
    {
        // TODO: Check if src_ty is !Sized
        // - Only allowed if the source is a trait object with the same data trait and lesser bounds

        DEBUG("TraitObject unsize? " << dst_ty << " <- " << src_ty);

        // (Trait) <- (Trait+Foo)
        if( const auto* se = src_ty.data().opt_TraitObject() )
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

        ::HIR::TypeData::Data_TraitObject  tmp_e;
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
                            atyv = ::HIR::TypeRef::new_path( ::HIR::Path( src_ty.clone(), aty.second.source_trait.clone(), aty.first ), {} );
                        }
                        // Run EAT
                        this->expand_associated_types(sp, atyv);
                        if( aty.second.type != atyv ) {
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
    if( const auto* de = dst_ty.data().opt_Slice() )
    {
        if( const auto* se = src_ty.data().opt_Array() )
        {
            DEBUG("Array unsize? " << de->inner << " <- " << se->inner);
            return se->inner == de->inner;
        }
    }

    DEBUG("Can't unsize, no rules matched");
    return false;
}

// Check if the passed type contains an UnsafeCell
// Returns `Fuzzy` if generic, `Equal` if it does contain an UnsafeCell, and `Unequal` if it doesn't (shared=immutable)
HIR::Compare StaticTraitResolve::type_is_interior_mutable(const Span& sp, const ::HIR::TypeRef& ty) const
{
    TU_MATCH_HDRA( (ty.data()), {)
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
        auto monomorph_cb = MonomorphStatePtr(nullptr, e.path.m_data.is_Generic() ? &e.path.m_data.as_Generic().m_params : nullptr, nullptr);
        HIR::TypeRef    tmp_ty;
        auto monomorph = [&](const auto& tpl)->const ::HIR::TypeRef& {
            return this->monomorph_expand_opt(sp, tmp_ty, tpl, monomorph_cb);
            };
        TU_MATCH_HDRA( (e.binding), {)
        TU_ARMA(Unbound, pbe)
            return HIR::Compare::Fuzzy;
        TU_ARMA(Opaque, pbe)
            return HIR::Compare::Fuzzy;
        TU_ARMA(ExternType, pbe)    // Extern types can't be interior mutable (but they also shouldn't be direct)
            return HIR::Compare::Unequal;

        TU_ARMA(Struct, pbe) {
            const HIR::GenericPath& p = e.path.m_data.as_Generic();
            if( p.m_path == m_crate.get_lang_item_path(sp, "unsafe_cell") ) {
                return HIR::Compare::Equal;
            }
            // TODO: Cache this result?
            TU_MATCH_HDRA( (pbe->m_data), { )
            TU_ARMA(Unit, _)    return HIR::Compare::Unequal;
            TU_ARMA(Tuple, e) {
                for(const auto& v : e) {
                    switch( this->type_is_interior_mutable(sp, monomorph(v.ent)) )
                    {
                    case HIR::Compare::Equal:
                        return HIR::Compare::Equal;
                    case HIR::Compare::Fuzzy:
                        return HIR::Compare::Fuzzy;
                    default:
                        continue;
                    }
                }
                return HIR::Compare::Unequal;
                }
            TU_ARMA(Named, e) {
                for(const auto& v : e) {
                    switch( this->type_is_interior_mutable(sp, monomorph(v.second.ent)) )
                    {
                    case HIR::Compare::Equal:
                        return HIR::Compare::Equal;
                    case HIR::Compare::Fuzzy:
                        return HIR::Compare::Fuzzy;
                    default:
                        continue;
                    }
                }
                return HIR::Compare::Unequal;
                }
            }
            }
        TU_ARMA(Enum, pbe) {
            TU_MATCH_HDRA( (pbe->m_data), { )
            TU_ARMA(Value, _)   return HIR::Compare::Unequal;
            TU_ARMA(Data, ee) {
                for(const auto& var : ee) {
                    switch( this->type_is_interior_mutable(sp, monomorph(var.type)) )
                    {
                    case HIR::Compare::Equal:
                        return HIR::Compare::Equal;
                    case HIR::Compare::Fuzzy:
                        return HIR::Compare::Fuzzy;
                    default:
                        continue;
                    }
                }
                return HIR::Compare::Unequal;
                }
            }
            }
        TU_ARMA(Union, pbe) {
            DEBUG("TODO: Check if union is interior mutable - " << e.path);
            return HIR::Compare::Fuzzy;
            }
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
        return this->type_is_interior_mutable(sp, e.inner);
        }
    TU_ARMA(Slice, e) {
        return this->type_is_interior_mutable(sp, e.inner);
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
    TU_ARMA(Generator, e) {
        return HIR::Compare::Unequal;
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
    TU_MATCH_HDRA( (ty.data()), {)
    default:
        return MetadataType::None;
    TU_ARMA(Generic, e) {
        // Check for an explicit `Sized` bound
        auto pp = ::HIR::PathParams();
        bool rv = this->find_impl__bounds(sp, m_lang_Sized, &pp, ty, [&](auto , bool ){ return true; });
        if(rv) {
            return MetadataType::None;
        }
        if( e.binding == 0xFFFF ) {
            // TODO: `Self: ?Sized` (e.g. traits)
            return MetadataType::None;
        }
        else if( (e.binding >> 8) == 0 ) {
            auto idx = e.binding & 0xFF;
            ASSERT_BUG(sp, m_impl_generics, "Encountered generic " << ty << " without impl generics available");
            ASSERT_BUG(sp, idx < m_impl_generics->m_types.size(), "Encountered generic " << ty << " out of range of impl generic spec");
            if( m_impl_generics->m_types[idx].m_is_sized ) {
                return MetadataType::None;
            }
            else {
                return MetadataType::Unknown;
            }
        }
        else if( (e.binding >> 8) == 1 ) {
            auto idx = e.binding & 0xFF;
            ASSERT_BUG(sp, m_item_generics, "Encountered generic " << ty << " without item generics available");
            ASSERT_BUG(sp, idx < m_item_generics->m_types.size(), "Encountered generic " << ty << " out of range of item generic spec");
            if( m_item_generics->m_types[idx].m_is_sized ) {
                return MetadataType::None;
            }
            else {
                return MetadataType::Unknown;
            }
        }
        else if( e.is_placeholder() ) {
            return MetadataType::None;
        }
        else {
            BUG(sp, "Unknown generic binding on " << ty);
        }
        }
    TU_ARMA(ErasedType, e) {
        if(e.m_is_sized)
            return MetadataType::None;
        else
            return MetadataType::Unknown;
        }
    TU_ARMA(Path, e) {
        TU_MATCH_HDRA( (e.binding), { )
        TU_ARMA(Unbound, pbe) {
            // TODO: Should this return something else?
            return MetadataType::Unknown;
            }
        TU_ARMA(Opaque, pbe) {
            //auto pp = ::HIR::PathParams();
            //return this->find_impl(sp, m_lang_Sized, &pp, ty, [&](auto , bool){ return true; }, true);
            // TODO: This can only be with UfcsKnown, so check if the trait specifies ?Sized
            //return MetadataType::Unknown;
            return MetadataType::None;
            }
        TU_ARMA(Struct, pbe) {
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
            }
        TU_ARMA(ExternType, pbe) {
            // Extern types aren't Sized, but have no metadata
            return MetadataType::Zero;
            }
        TU_ARMA(Enum, pbe) {
            }
        TU_ARMA(Union, pbe) {
            }
        }
        return MetadataType::None;
        }
    TU_ARMA(Infer, e) {
        // Shouldn't be hit
        BUG(sp, "Found ivar? " << ty);
        }
    TU_ARMA(Diverge, e) {
        // The ! type is kinda Sized ...
        return MetadataType::None;
        }
    TU_ARMA(Primitive, e) {
        // All primitives (except the unsized `str`) are Sized
        if( e == ::HIR::CoreType::Str )
        {
            return MetadataType::Slice;
        }
        else
        {
            return MetadataType::None;
        }
        }
    TU_ARMA(Slice, e) {
        return MetadataType::Slice;
        }
    TU_ARMA(TraitObject, e) {
        return MetadataType::TraitObject;
        }
    TU_ARMA(Tuple, e) {
        // TODO: Unsized tuples? are they a thing?
        //for(const auto& ty : e)
        //    if( !type_is_sized(sp, ty) )
        //        return false;
        return MetadataType::None;
        }
    }
    throw "bug";
}

bool StaticTraitResolve::type_needs_drop_glue(const Span& sp, const ::HIR::TypeRef& ty) const
{
    // If `T: Copy`, then it can't need drop glue
    if( type_is_copy(sp, ty) )
        return false;

    TU_MATCH_HDRA( (ty.data()), {)
    TU_ARMA(Generic, e) {
        // TODO: Is this an error?
        return true;
        }
    TU_ARMA(Path, e) {
        if( e.binding.is_Opaque() )
            return true;

        // In 1.29, "manually_drop" is a struct with special behaviour (instead of being a union)
        if( TARGETVER_LEAST_1_29 && e.path.m_data.as_Generic().m_path == m_crate.get_lang_item_path_opt("manually_drop") )
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
        auto monomorph_cb = MonomorphStatePtr(nullptr, &pe.m_params, nullptr);
        auto monomorph = [&](const auto& tpl)->const ::HIR::TypeRef& {
            return this->monomorph_expand_opt(sp, tmp_ty, tpl, monomorph_cb);
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
        }
    TU_ARMA(Diverge, e) {
        return false;
        }
    TU_ARMA(Closure, e) {
        // Note: Copy already covered above
        return true;
        }
    TU_ARMA(Generator, e) {
        return true;
        }
    TU_ARMA(Infer, e) {
        BUG(sp, "type_needs_drop_glue on _");
        return false;
        }
    TU_ARMA(Borrow, e) {
        // &-ptrs don't have drop glue
        if( e.type != ::HIR::BorrowType::Owned )
            return false;
        return type_needs_drop_glue(sp, e.inner);
        }
    TU_ARMA(Pointer, e) {
        return false;
        }
    TU_ARMA(Function, e) {
        return false;
        }
    TU_ARMA(Primitive, e) {
        return false;
        }
    TU_ARMA(Array, e) {
        return type_needs_drop_glue(sp, e.inner);
        }
    TU_ARMA(Slice, e) {
        return type_needs_drop_glue(sp, e.inner);
        }
    TU_ARMA(TraitObject, e) {
        return true;
        }
    TU_ARMA(ErasedType, e) {
        // Is this an error?
        return true;
        }
    TU_ARMA(Tuple, e) {
        for(const auto& ty : e)
        {
            if( type_needs_drop_glue(sp, ty) )
                return true;
        }
        return false;
        }
    }
    assert(!"Fell off the end of type_needs_drop_glue");
    throw "";
}

const ::HIR::TypeRef* StaticTraitResolve::is_type_owned_box(const ::HIR::TypeRef& ty) const
{
    if( ! ty.data().is_Path() ) {
        return nullptr;
    }
    const auto& te = ty.data().as_Path();

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
    if( ! ty.data().is_Path() ) {
        return nullptr;
    }
    const auto& te = ty.data().as_Path();

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

StaticTraitResolve::ValuePtr StaticTraitResolve::get_value(const Span& sp, const ::HIR::Path& p, MonomorphState& out_params, bool signature_only/*=false*/, const HIR::GenericParams** out_impl_params_def/*=nullptr*/) const
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
                if(out_impl_params_def) {
                    *out_impl_params_def = &e->m_params;
                }
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
        ASSERT_BUG(sp, mod.m_value_items.count(pe.m_path.m_components.back()), "Missing item " << pe.m_path);
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
            const auto& str = m_crate.get_struct_by_path(sp, ve.ty);
            if(out_impl_params_def) {
                *out_impl_params_def = &str.m_params;
            }
            return ValuePtr::Data_StructConstructor { &ve.ty, &str };
            )
        )
        throw "";
        }
    TU_ARM(p.m_data, UfcsKnown, pe) {
        out_params.self_ty = pe.type.clone();
        out_params.pp_impl = &pe.trait.m_params;
        out_params.pp_method = &pe.params;
        const ::HIR::Trait& tr = m_crate.get_trait_by_path(sp, pe.trait.m_path);
        if( !tr.m_values.count(pe.item) ) {
            DEBUG("Value " << pe.item << " not found in trait " << pe.trait.m_path);
            return ValuePtr();
        }

        if(out_impl_params_def) {
            *out_impl_params_def = &tr.m_params;
            // Updated if an impl is found+used
        }

        const ::HIR::TraitValueItem& v = tr.m_values.at(pe.item);
        if( signature_only )
        {
            TU_MATCHA( (v), (ve),
            (Constant, return &ve; ),
            (Static,   return &ve; ),
            (Function, return &ve; )
            )
        }
        else
        {
            bool best_is_spec = false;
            ImplRef best_impl;
            ValuePtr    rv;
            this->find_impl(sp, pe.trait.m_path, &pe.trait.m_params, pe.type, [&](auto impl, bool is_fuzz)->bool{
                DEBUG(impl);
                if( ! impl.m_data.is_TraitImpl() )
                    return false;
                const ::HIR::TraitImpl& ti = *impl.m_data.as_TraitImpl().impl;
                bool is_spec = false;

                ValuePtr    this_rv;
                // - Constants
                if(this_rv.is_NotFound())
                {
                    auto it = ti.m_constants.find(pe.item);
                    if(it != ti.m_constants.end())
                    {
                        is_spec = it->second.is_specialisable;
                        this_rv = &it->second.data;
                    }
                }
                // - Statics
                if(this_rv.is_NotFound())
                {
                    auto it = ti.m_statics.find(pe.item);
                    if(it != ti.m_statics.end())
                    {
                        is_spec = it->second.is_specialisable;
                        this_rv = &it->second.data;
                    }
                }
                // - Functions
                if(this_rv.is_NotFound())
                {
                    auto it = ti.m_methods.find(pe.item);
                    if(it != ti.m_methods.end())
                    {
                        is_spec = it->second.is_specialisable;
                        this_rv = &it->second.data;
                    }
                }

                if(this_rv.is_NotFound()) {
                    DEBUG("- Missing the target item");
                    return false;
                }
                else if( !impl.more_specific_than(best_impl) ) {
                    // Keep searching
                    DEBUG("- Less specific");
                    return false;
                }
                else {
                    DEBUG("- More specific (is_spec=" << is_spec << ")");
                    best_is_spec = is_spec;
                    best_impl = mv$(impl);
                    rv = std::move(this_rv);
                    // NOTE: There could be an overlapping and more-specific impl without `default` being involved
                    //return !is_spec;
                    return false;
                }
                });
            if( !best_impl.is_valid() )
            {
                // If the type and impl are fully known, then look for trait provided values/bodies
                if( !monomorphise_type_needed(pe.type) && !monomorphise_pathparams_needed(pe.trait.m_params) )
                {
                    // Look for provided bodies
                    TU_MATCH_HDRA( (v), {)
                    TU_ARMA(Constant, ve) {
                        // Constants?
                        if( ve.m_value_state != HIR::Constant::ValueState::Unknown ) {
                            DEBUG("Trait provided value");
                            // NOTE: The parameters have already been set
                            return &ve;
                        }
                        }
                    TU_ARMA(Static, ve) {
                        // Statics?
                        }
                    TU_ARMA(Function, ve) {
                        if( ve.m_code || ve.m_code.m_mir ) {
                            DEBUG("Trait provided body");
                            // NOTE: The parameters have already been set
                            return &ve;
                        }
                        // Fall through if there's no provided body
                        }
                    }
                }
                return ValuePtr::make_NotYetKnown({});
            }
            if(best_is_spec)
            {
                // If there's generics present in the path, return NotYetKnown
                if( monomorphise_type_needed(pe.type) || monomorphise_pathparams_needed(pe.trait.m_params) )
                {
                    DEBUG("Specialisable and still generic, return NotYetKnown");
                    return ValuePtr::make_NotYetKnown({});
                }
            }

            if( ! best_impl.m_data.is_TraitImpl() )
                TODO(sp, "Use bounded constant values for " << p);
            auto& ie = best_impl.m_data.as_TraitImpl();
            if(out_impl_params_def) {
                *out_impl_params_def = &ie.impl->m_params;
            }
            out_params.pp_impl = &out_params.pp_impl_data;
            out_params.pp_impl_data = ie.impl_params.clone();
            ASSERT_BUG(sp, !rv.is_NotFound(), "");
            return rv;
        }
        throw "";
        }
    TU_ARM(p.m_data, UfcsInherent, pe) {
        out_params.self_ty = pe.type.clone();
        //out_params.pp_impl = &out_params.pp_impl_data;
        out_params.pp_impl = &pe.impl_params;
        out_params.pp_method = &pe.params;
        ValuePtr    rv;
        m_crate.find_type_impls(pe.type, [](const auto&x)->const ::HIR::TypeRef& { return x; }, [&](const auto& impl) {
            DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
            // Populate pp_impl if not populated
            if( !pe.impl_params.has_params() ) {
                GetParams get_params { sp, impl.m_params, out_params.pp_impl_data };

                auto cb_ident = [](const ::HIR::TypeRef&ty)->const ::HIR::TypeRef& { return ty; };
                impl.m_type.match_test_generics_fuzz(sp, pe.type, cb_ident, get_params);

                const auto& impl_params = out_params.pp_impl_data;
                for(size_t i = 0; i < impl_params.m_types.size(); i ++ ) {
                    if( impl_params.m_types[i] == HIR::TypeRef() ) {
                        // TODO: Error when there's a type param that can't be determined?
                    }
                }
                for(size_t i = 0; i < impl_params.m_values.size(); i ++ ) {
                    if( impl_params.m_values[i] == HIR::ConstGeneric() ) {
                        // TODO: Error when there's a value param that can't be determined?
                    }
                }

                out_params.pp_impl = &out_params.pp_impl_data;
            }

            if(out_impl_params_def) {
                *out_impl_params_def = &impl.m_params;
            }

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
