/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/static.cpp
 * - Non-inferred type checking
 */
#include "static.hpp"

void StaticTraitResolve::prep_indexes()
{
    static Span sp_AAA;
    const Span& sp = sp_AAA;
    
    TRACE_FUNCTION_F("");
    
    auto add_equality = [&](::HIR::TypeRef long_ty, ::HIR::TypeRef short_ty){
        DEBUG("[prep_indexes] ADD " << long_ty << " => " << short_ty);
        // TODO: Sort the two types by "complexity" (most of the time long >= short)
        this->m_type_equalities.insert(::std::make_pair( mv$(long_ty), mv$(short_ty) ));
        };
    
    this->iterate_bounds([&](const auto& b) {
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
            auto cb_mono = [&](const auto& ty)->const auto& {
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

bool StaticTraitResolve::find_impl(
    const Span& sp,
    const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
    const ::HIR::TypeRef& type,
    t_cb_find_impl found_cb
    ) const
{
    TRACE_FUNCTION_F(trait_path << FMT_CB(os, if(trait_params) { os << *trait_params; } else { os << "<?>"; }) << " for " << type);
    auto cb_ident = [](const auto&ty)->const auto&{return ty;};
    
    struct H {
        static bool compare_pp(const Span& sp, const ::HIR::PathParams& left, const ::HIR::PathParams& right) {
            ASSERT_BUG( sp, left.m_types.size() == right.m_types.size(), "Parameter count mismatch" );
            for(unsigned int i = 0; i < left.m_types.size(); i ++) {
                if( left.m_types[i] != right.m_types[i] ) {
                    return false;
                }
            }
            return true;
        } 
    };
    const ::HIR::Path::Data::Data_UfcsKnown* assoc_info = nullptr;
    TU_IFLET(::HIR::TypeRef::Data, type.m_data, Path, e,
        TU_IFLET(::HIR::Path::Data, e.path.m_data, UfcsKnown, pe,
            assoc_info = &pe;
        )
    )
    
    bool ret;
    
    // TODO: A bound can imply something via its associated types. How deep can this go?
    // E.g. `T: IntoIterator<Item=&u8>` implies `<T as IntoIterator>::IntoIter : Iterator<Item=&u8>`
    ret = this->iterate_bounds([&](const auto& b) {
        TU_IFLET(::HIR::GenericBound, b, TraitBound, e,
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
                    if( found_cb(ImplRef(&e.type, &e.trait.m_path.m_params, &e.trait.m_type_bounds)) ) {
                        return true;
                    }
                }
                #if 1
                // HACK: The wrapping closure takes associated types from this bound and applies them to the returned set
                // - XXX: This is actually wrong (false-positive) in many cases. FIXME
                bool rv = this->find_named_trait_in_trait(sp,
                    trait_path,b_params,
                    *e.trait.m_trait_ptr, e.trait.m_path.m_path,e.trait.m_path.m_params,
                    type,
                    [&](const auto& params, auto assoc) {
                        for(const auto& i : e.trait.m_type_bounds) {
                            // TODO: Only include from above when needed
                            //if( des_trait_ref.m_types.count(i.first) ) {
                                assoc.insert( ::std::make_pair(i.first, i.second.clone())  );
                            //}
                        }
                        return found_cb( ImplRef(type.clone(), params.clone(), mv$(assoc)) );
                    });
                if( rv ) {
                    return true;
                }
                #endif
            }
            
            // If the input type is an associated type controlled by this trait bound, check for added bounds.
            // TODO: This just checks a single layer, but it's feasable that there could be multiple layers
            if( assoc_info && e.trait.m_path.m_path == assoc_info->trait.m_path && e.type == *assoc_info->type && H::compare_pp(sp, b_params, assoc_info->trait.m_params) ) {
                
                const auto& trait_ref = *e.trait.m_trait_ptr;
                const auto& at = trait_ref.m_types.at(assoc_info->item);
                for(const auto& bound : at.m_trait_bounds) {
                    if( bound.m_path.m_path == trait_path && (!trait_params || H::compare_pp(sp, bound.m_path.m_params, *trait_params)) ) {
                        DEBUG("- Found an associated type impl");
                        
                        auto tp_mono = monomorphise_traitpath_with(sp, bound, [&assoc_info,&sp](const auto& gt)->const auto& {
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
                        if( found_cb( ImplRef(type.clone(), mv$(tp_mono.m_path.m_params), mv$(tp_mono.m_type_bounds)) ) ) {
                            return true;
                        }
                    }
                }
            }
            
            return false;
        )
        return false;
        });
    if(ret)
        return true;
    
    // Search the crate for impls
    ret = m_crate.find_trait_impls(trait_path, type, cb_ident, [&](const auto& impl) {
        return this->find_impl__check_crate(sp, trait_path, trait_params, type, found_cb,  impl);
        });
    if(ret)
        return true;
    
    return false;
}

bool StaticTraitResolve::find_impl__check_crate(
        const Span& sp,
        const ::HIR::SimplePath& trait_path, const ::HIR::PathParams* trait_params,
        const ::HIR::TypeRef& type,
        t_cb_find_impl found_cb,
        const ::HIR::TraitImpl& impl
    ) const
{
    auto cb_ident = [](const auto&ty)->const auto&{return ty;};
    DEBUG("impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type << impl.m_params.fmt_bounds());
    
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
    if( trait_params )
    {
        assert( trait_params->m_types.size() == impl.m_trait_args.m_types.size() );
        for( unsigned int i = 0; i < impl.m_trait_args.m_types.size(); i ++ )
        {
            const auto& l = impl.m_trait_args.m_types[i];
            const auto& r = trait_params->m_types[i];
            match &= l.match_test_generics_fuzz(sp, r, cb_ident, cb);
        }
    }
    if( match != ::HIR::Compare::Equal ) {
        DEBUG(" > Type mismatch");
        // TODO: Support fuzzy matches for some edge cases
        return false;
    }
    
    ::std::vector< ::HIR::TypeRef>  placeholders;
    for(unsigned int i = 0; i < impl_params.size(); i ++ ) {
        if( !impl_params[i] ) {
            if( placeholders.size() == 0 )
                placeholders.resize(impl_params.size());
            placeholders[i] = ::HIR::TypeRef("impl_?", 2*256 + i);
        }
    }
    // Callback that matches placeholders to concrete types
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
    // Callback that returns monomorpisation results
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
            DEBUG("[find_impl] Trait bound " << e.type << " : " << e.trait);
            auto b_ty_mono = monomorphise_type_with(sp, e.type, cb_monomorph);
            auto b_tp_mono = monomorphise_traitpath_with(sp, e.trait, cb_monomorph, false);
            for(auto& ty : b_tp_mono.m_path.m_params.m_types) {
                this->expand_associated_types(sp, ty);
            }
            for(auto& assoc_bound : b_tp_mono.m_type_bounds) {
                // TODO: These should be tagged with the source trait and that source trait used for expansion.
                this->expand_associated_types(sp, assoc_bound.second);
            }
            DEBUG("[find_impl] - b_ty_mono = " << b_ty_mono << ", b_tp_mono = " << b_tp_mono);
            // HACK: If the type is '_', assume the bound passes
            if( b_ty_mono.m_data.is_Infer() ) {
                continue ;
            }
            
            // TODO: This is extrememly inefficient (looks up the trait impl 1+N times)
            if( b_tp_mono.m_type_bounds.size() > 0 ) {
                //
                for(const auto& assoc_bound : b_tp_mono.m_type_bounds) {
                    const auto& aty_name = assoc_bound.first;
                    const ::HIR::TypeRef& exp = assoc_bound.second;
                    
                    ::HIR::GenericPath  aty_src_trait;
                    trait_contains_type(sp, b_tp_mono.m_path, *e.trait.m_trait_ptr, aty_name, aty_src_trait);
                    
                    auto rv = this->find_impl(sp, aty_src_trait.m_path, aty_src_trait.m_params, b_ty_mono, [&](const auto& impl) {
                        ::HIR::TypeRef have = impl.get_type(aty_name.c_str());
                        
                        //auto cmp = have .match_test_generics_fuzz(sp, exp, cb_ident, cb_match);
                        auto cmp = exp .match_test_generics_fuzz(sp, have, cb_ident, cb_match);
                        ASSERT_BUG(sp, cmp == ::HIR::Compare::Equal, "Assoc ty " << aty_name << " mismatch, " << have << " != des " << exp);
                        return true;
                        });
                    if( !rv ) {
                        DEBUG("> Fail - " << b_ty_mono << ": " << aty_src_trait);
                        return false;
                    }
                }
            }
            
            auto rv = this->find_impl(sp, b_tp_mono.m_path.m_path, b_tp_mono.m_path.m_params, b_ty_mono, [&](const auto& impl) {
                
                #if 0
                for(const auto& assoc_bound : b_tp_mono.m_type_bounds) {
                    const char* name = assoc_bound.first.c_str();
                    const ::HIR::TypeRef& exp = assoc_bound.second;
                    ::HIR::TypeRef have = impl.get_type(name);
                    // TODO: Returning `_` means unset associated type, should that still be compared?
                    if( have != ::HIR::TypeRef() )
                    {
                        auto cmp = have .match_test_generics_fuzz(sp, exp, cb_ident, cb_match);
                        ASSERT_BUG(sp, cmp == ::HIR::Compare::Equal, "Assoc ty " << name << " mismatch, " << have << " != des " << exp);
                    }
                    else
                    {
                        DEBUG("Assoc `" << name << "` unbound, can't compare with " << exp);
                    }
                }
                #endif
                return true;
                });
            if( !rv ) {
                DEBUG("> Fail - " << b_ty_mono << ": " << b_tp_mono);
                return false;
            }
            )
        )
    }
    
    return found_cb( ImplRef(impl_params, trait_path, impl, mv$(placeholders)) );
}

void StaticTraitResolve::expand_associated_types(const Span& sp, ::HIR::TypeRef& input) const
{
    TRACE_FUNCTION_F(input);
    TU_MATCH(::HIR::TypeRef::Data, (input.m_data), (e),
    (Infer,
        BUG(sp, "Encountered inferrence variable in static context");
        ),
    (Diverge,
        ),
    (Primitive,
        ),
    (Path,
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
        (Generic,
            for(auto& arg : e2.m_params.m_types)
                this->expand_associated_types(sp, arg);
            ),
        (UfcsInherent,
            TODO(sp, "Path - UfcsInherent - " << e.path);
            ),
        (UfcsKnown,
            // - Only try resolving if the binding isn't known
            if( !e.binding.is_Unbound() )
                return ;
            
            this->expand_associated_types(sp, *e2.type);
            for(auto& arg : e2.trait.m_params.m_types)
                this->expand_associated_types(sp, arg);
            
            DEBUG("Locating associated type for " << e.path);
            
            // - If it's a closure, then the only trait impls are those generated by typeck
            TU_IFLET(::HIR::TypeRef::Data, e2.type->m_data, Closure, te,
                const auto trait_fn = this->m_crate.get_lang_item_path(sp, "fn");
                const auto trait_fn_mut = this->m_crate.get_lang_item_path(sp, "fn_mut");
                const auto trait_fn_once = this->m_crate.get_lang_item_path(sp, "fn_once");
                if( e2.trait.m_path == trait_fn || e2.trait.m_path == trait_fn_mut || e2.trait.m_path == trait_fn_once  ) {
                    if( e2.item == "Output" ) {
                        input = te.m_rettype->clone();
                        return ;
                    }
                    else {
                        ERROR(sp, E0000, "No associated type " << e2.item << " for trait " << e2.trait);
                    }
                }
                else {
                    ERROR(sp, E0000, "No implementation of " << e2.trait << " for " << *e2.type);
                }
            )
            
            // 1. Bounds
            bool rv;
            bool assume_opaque = true;
            rv = this->iterate_bounds([&](const auto& b) {
                TU_MATCH_DEF(::HIR::GenericBound, (b), (be),
                (
                    ),
                (TraitBound,
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
                    ),
                (TypeEquality,
                    DEBUG("Equality - " << be.type << " = " << be.other_type);
                    if( input == be.type ) {
                        input = be.other_type.clone();
                        return true;
                    }
                    )
                )
                return false;
                });
            if( rv ) {
                if( assume_opaque ) {
                    input.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                    DEBUG("Assuming that " << input << " is an opaque name");
                    
                    this->replace_equalities(input);
                }
                this->expand_associated_types(sp, input);
                return;
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
                    auto cb_placeholders_trait = [&](const auto& ty)->const auto&{
                        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Generic, e,
                            if( e.binding == 0xFFFF )
                                return *pe_inner.type;
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
                                this->expand_associated_types(sp, input);
                                return ;
                            }
                        }
                    }
                    DEBUG("e2 = " << *e2.type << ", input = " << input);
                )
            )

            // 2. Crate-level impls
            // TODO: Search for the actual trait containing this associated type
            ::HIR::GenericPath  trait_path;
            if( !this->trait_contains_type(sp, e2.trait, this->m_crate.get_trait_by_path(sp, e2.trait.m_path), e2.item, trait_path) )
                BUG(sp, "Cannot find associated type " << e2.item << " anywhere in trait " << e2.trait);
            //e2.trait = mv$(trait_path);
            
            rv = this->find_impl(sp, trait_path.m_path, trait_path.m_params, *e2.type, [&](const auto& impl) {
                DEBUG("[expand_associated_types] Found " << impl);
                
                auto nt = impl.get_type( e2.item.c_str() );
                DEBUG("Converted UfcsKnown - " << e.path << " = " << nt);
                input = mv$(nt);
                return true;
                });
            if( rv ) {
                this->expand_associated_types(sp, input);
                return;
            }
            
            // TODO: If the type is a generic or an opaque associated, we can't know.
            // - If the trait contains any of the above, it's unknowable
            // - Otherwise, it's an error
            e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
            this->replace_equalities(input);
            DEBUG("Couldn't resolve associated type for " << input << " (and won't ever be able to, assuming opaque)");
            ),
        (UfcsUnknown,
            BUG(sp, "Encountered UfcsUnknown");
            )
        )
        ),
    (Generic,
        ),
    (TraitObject,
        // Recurse?
        ),
    (Array,
        expand_associated_types(sp, *e.inner);
        ),
    (Slice,
        expand_associated_types(sp, *e.inner);
        ),
    (Tuple,
        for(auto& sub : e) {
            expand_associated_types(sp, sub);
        }
        ),
    (Borrow,
        expand_associated_types(sp, *e.inner);
        ),
    (Pointer,
        expand_associated_types(sp, *e.inner);
        ),
    (Function,
        // Recurse?
        ),
    (Closure,
        // Recurse?
        )
    )
}

void StaticTraitResolve::replace_equalities(::HIR::TypeRef& input) const
{
    TRACE_FUNCTION_F("input="<<input);
    DEBUG("m_type_equalities = {" << m_type_equalities << "}");
    // - Check if there's an alias for this opaque name
    auto a = m_type_equalities.find(input);
    if( a != m_type_equalities.end() ) {
        input = a->second.clone();
        DEBUG("- Replace with " << input);
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
// -------------------------------------------------------------------------------------------------------------------
//
// -------------------------------------------------------------------------------------------------------------------
bool StaticTraitResolve::find_named_trait_in_trait(const Span& sp,
        const ::HIR::SimplePath& des, const ::HIR::PathParams& des_params,
        const ::HIR::Trait& trait_ptr, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& pp,
        const ::HIR::TypeRef& target_type,
        ::std::function<void(const ::HIR::PathParams&, ::std::map< ::std::string, ::HIR::TypeRef>)> callback
    ) const
{
    TRACE_FUNCTION_F(des << " from " << trait_path << pp);
    if( pp.m_types.size() != trait_ptr.m_params.m_types.size() ) {
        BUG(sp, "Incorrect number of parameters for trait");
    }
    for( const auto& pt : trait_ptr.m_parent_traits )
    {
        auto pt_mono = monomorphise_traitpath_with(sp, pt, [&](const auto& gt)->const auto& {
            const auto& ge = gt.m_data.as_Generic();
            if( ge.binding == 0xFFFF ) {
                return target_type;
            }
            else {
                if( ge.binding >= pp.m_types.size() )
                    BUG(sp, "find_named_trait_in_trait - Generic #" << ge.binding << " " << ge.name << " out of range");
                return pp.m_types[ge.binding];
            }
            }, false);

        DEBUG(pt << " => " << pt_mono);
        if( pt.m_path.m_path == des ) {
            callback( pt_mono.m_path.m_params, mv$(pt_mono.m_type_bounds) );
            return true;
        }
        
        const auto& tr = m_crate.get_trait_by_path(sp, pt.m_path.m_path);
        if( find_named_trait_in_trait(sp, des, des_params,  tr, pt.m_path.m_path, pt_mono.m_path.m_params,  target_type, callback) ) {
            return true;
        }
    }
    return false;
}
bool StaticTraitResolve::trait_contains_type(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait_ptr, const ::std::string& name,  ::HIR::GenericPath& out_path) const
{
    auto it = trait_ptr.m_types.find(name);
    if( it != trait_ptr.m_types.end() ) {
        out_path = trait_path.clone();
        return true;
    }
    
    auto monomorph = [&](const auto& gt)->const auto& {
            const auto& ge = gt.m_data.as_Generic();
            assert(ge.binding < 256);
            assert(ge.binding < trait_path.m_params.m_types.size());
            return trait_path.m_params.m_types[ge.binding];
            };
    // TODO: Prevent infinite recursion
    for(const auto& st : trait_ptr.m_parent_traits)
    {
        auto& st_ptr = this->m_crate.get_trait_by_path(sp, st.m_path.m_path);
        if( trait_contains_type(sp, st.m_path, st_ptr, name, out_path) ) {
            out_path.m_params = monomorphise_path_params_with(sp, mv$(out_path.m_params), monomorph, false);
            return true;
        }
    }
    return false;
}

bool StaticTraitResolve::type_is_copy(const ::HIR::TypeRef& ty) const
{
    TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (e),
    (
        // Search for a Copy bound or impl.
        return false;
        ),
    (Borrow,
        return (e.type == ::HIR::BorrowType::Shared);
        ),
    (Pointer,
        return true;
        ),
    (Primitive,
        return e != ::HIR::CoreType::Str;
        ),
    (Array,
        return type_is_copy(*e.inner);
        )
    )
}
