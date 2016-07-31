/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/static.cpp
 * - Non-inferred type checking
 */
#include "static.hpp"

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
        DEBUG("[find_impl] - impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type << impl.m_params.fmt_bounds());
        
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
            DEBUG("[find_impl]  > Type mismatch");
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
        //auto cb_match = [&](unsigned int idx, const auto& ty) {
        //    if( ty.m_data.is_Generic() && ty.m_data.as_Generic().binding == idx )
        //        return ::HIR::Compare::Equal;
        //    if( idx >> 8 == 2 ) {
        //        auto i = idx % 256;
        //        ASSERT_BUG(sp, !impl_params[i], "Placeholder to populated type returned");
        //        auto& ph = placeholders[i];
        //        if( ph.m_data.is_Generic() && ph.m_data.as_Generic().binding == idx ) {
        //            DEBUG("Bind placeholder " << i << " to " << ty);
        //            ph = ty.clone();
        //            return ::HIR::Compare::Equal;
        //        }
        //        else {
        //            TODO(sp, "Compare placeholder " << i << " " << ph << " == " << ty);
        //        }
        //    }
        //    else {
        //        return ::HIR::Compare::Unequal;
        //    }
        //    };
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
                DEBUG("Trait bound " << e.type << " : " << e.trait);
                auto b_ty_mono = monomorphise_type_with(sp, e.type, cb_monomorph);
                auto b_tp_mono = monomorphise_traitpath_with(sp, e.trait, cb_monomorph, false);
                DEBUG("- b_ty_mono = " << b_ty_mono << ", b_tp_mono = " << b_tp_mono);
                // HACK: If the type is '_', assume the bound passes
                if( b_ty_mono.m_data.is_Infer() ) {
                    continue ;
                }
                if( !this->find_impl(sp, b_tp_mono.m_path.m_path, b_tp_mono.m_path.m_params, b_ty_mono, [](const auto&){return true;}) ) {
                    DEBUG("> Fail - " << b_ty_mono << ": " << b_tp_mono);
                    return false;
                }
                )
            )
        }
        
        return found_cb( ImplRef(impl_params, impl) );
        });
    if(ret)
        return true;
    
    return false;
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
                    DEBUG("Assuming that " << input << " is an opaque name");
                    input.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
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
                DEBUG("Found impl" << impl);
                //auto it = assoc.find( e2.item );
                //if( it == assoc.end() )
                //    ERROR(sp, E0000, "Couldn't find assocated type " << e2.item << " in " << e2.trait);
                //
                //DEBUG("Converted UfcsKnown - " << e.path << " = " << it->second);
                //input = it->second.clone();
                return true;
                });
            if( rv ) {
                this->expand_associated_types(sp, input);
                return;
            }
            
            // If there are no ivars in this path, set its binding to Opaque
            //if( !this->m_ivars.type_contains_ivars(input) ) {
                // TODO: If the type is a generic or an opaque associated, we can't know.
                // - If the trait contains any of the above, it's unknowable
                // - Otherwise, it's an error
                e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                DEBUG("Couldn't resolve associated type for " << input << " (and won't ever be able to)");
            //}
            //else {
            //    DEBUG("Couldn't resolve associated type for " << input << " (will try again later)");
            //}
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

bool ImplRef::more_specific_than(const ImplRef& other) const
{
    TU_MATCH(Data, (this->m_data), (e),
    (TraitImpl,
        if( e.impl == nullptr ) {
            return false;
        }
        TU_MATCH(Data, (other.m_data), (oe),
        (TraitImpl,
            if( oe.impl == nullptr ) {
                return true;
            }
            TODO(Span(), "more_specific_than - TraitImpl ( `" << *this << "` '>' `" << other << "`)");
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
bool ImplRef::type_is_specializable(const char* name) const
{
    TU_MATCH(Data, (this->m_data), (e),
    (TraitImpl,
        if( e.impl == nullptr ) {
            // No impl yet? This type is specialisable.
            return true;
        }
        //TODO(Span(), "type_is_specializable - Impl = " << *this << ", Type = " << name);
        return true;
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
        DEBUG(*this);
        auto it = e.impl->m_types.find(name);
        if( it == e.impl->m_types.end() )
            return ::HIR::TypeRef();
        if( monomorphise_type_needed(it->second) ) {
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
            return monomorphise_type_with(sp, it->second, cb_monomorph);
        }
        else {
            return it->second.clone();
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
            if( e.impl->m_params.m_types.size() )
            {
                os << "<";
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
                os << ">";
            }
            os << " ?" << e.impl->m_trait_args << " for " << e.impl->m_type << e.impl->m_params.fmt_bounds();
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
