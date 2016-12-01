/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/enumerate.cpp
 * - Translation item enumeration
 *
 * Enumerates items required for translation.
 */
#include "main_bindings.hpp"
#include "trans_list.hpp"
#include <hir/hir.hpp>
#include <mir/mir.hpp>
#include <hir_typeck/common.hpp>

struct Params {
    Span    sp;
    ::HIR::PathParams   pp_method;
    ::HIR::PathParams   pp_impl;
    ::HIR::TypeRef  self_type;
    
    t_cb_generic get_cb() const;
    ::HIR::TypeRef monomorph(const ::HIR::TypeRef& p) const;
    ::HIR::Path monomorph(const ::HIR::Path& p) const;
};

void Trans_Enumerate_FillFrom(TransList& out, const ::HIR::Crate& crate, const ::HIR::Function& function, Params pp={});
void Trans_Enumerate_FillFrom(TransList& out, const ::HIR::Crate& crate, const ::HIR::Static& stat, Params pp={});

/// Enumerate trans items starting from `::main` (binary crate)
TransList Trans_Enumerate_Main(const ::HIR::Crate& crate)
{
    TransList   rv;
    auto main_path = ::HIR::SimplePath("", {"main"});
    const auto& fcn = crate.get_function_by_path(Span(), main_path);
    rv.add_function(main_path, fcn);
    Trans_Enumerate_FillFrom(rv, crate,  fcn);
    return rv;
}

/// Enumerate trans items for all public non-generic items (library crate)
TransList Trans_Enumerate_Public(const ::HIR::Crate& crate)
{
    TransList   rv;
    
    for(const auto& vi : crate.m_root_module.m_value_items)
    {
        if( vi.second->is_public ) {
            TU_MATCHA( (vi.second->ent), (e),
            (Import,
                ),
            (StructConstant,
                ),
            (StructConstructor,
                ),
            (Constant,
                ),
            (Static,
                Trans_Enumerate_FillFrom(rv,crate, e);
                ),
            (Function,
                // - Only add if there are no generics.
                Trans_Enumerate_FillFrom(rv,crate, e);
                )
            )
        }
    }
    
    return rv;
}

namespace {
    TAGGED_UNION(EntPtr, NotFound,
        (NotFound, struct{}),
        (Function, const ::HIR::Function*),
        (Static, const ::HIR::Static*)
        );
    EntPtr get_ent_simplepath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path)
    {
        const ::HIR::Module* mod;
        if( path.m_crate_name != "" ) {
            ASSERT_BUG(sp, crate.m_ext_crates.count(path.m_crate_name) > 0, "Crate '" << path.m_crate_name << "' not loaded");
            mod = &crate.m_ext_crates.at(path.m_crate_name)->m_root_module;
        }
        else {
            mod = &crate.m_root_module;
        }
        
        for( unsigned int i = 0; i < path.m_components.size() - 1; i ++ )
        {
            const auto& pc = path.m_components[i];
            auto it = mod->m_mod_items.find( pc );
            if( it == mod->m_mod_items.end() ) {
                BUG(sp, "Couldn't find component " << i << " of " << path);
            }
            TU_MATCH_DEF( ::HIR::TypeItem, (it->second->ent), (e2),
            (
                BUG(sp, "Node " << i << " of path " << path << " wasn't a module");
                ),
            (Module,
                mod = &e2;
                )
            )
        }
        
        auto it = mod->m_value_items.find( path.m_components.back() );
        if( it == mod->m_value_items.end() ) {
            return EntPtr {};
        }
        
        TU_MATCH( ::HIR::ValueItem, (it->second->ent), (e),
        (Import,
            ),
        (StructConstant,
            ),
        (StructConstructor,
            ),
        (Function,
            return EntPtr { &e };
            ),
        (Constant,
            //return EntPtr { &e };
            ),
        (Static,
            return EntPtr { &e };
            )
        )
        BUG(sp, "Path " << path << " pointed to a invalid item - " << it->second->ent.tag_str());
    }
    EntPtr get_ent_fullpath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::Path& path, ::HIR::PathParams& impl_pp)
    {
        TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
        (Generic,
            return get_ent_simplepath(sp, crate, e.m_path);
            ),
        (UfcsInherent,
            // Easy (ish)
            EntPtr rv;
            crate.find_type_impls(*e.type, [](const auto&x)->const auto& { return x; }, [&](const auto& impl) {
                {
                    auto fit = impl.m_methods.find(e.item);
                    if( fit != impl.m_methods.end() )
                    {
                        DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                        rv = EntPtr { &fit->second.data };
                        return true;
                    }
                }
                //{
                //    auto it = impl.m_constants.find(e.item);
                //    if( it != impl.m_constants.end() )
                //    {
                //        rv = EntPtr { &it->second.data };
                //        return true;
                //    }
                //}
                return false;
                });
            return rv;
            ),
        (UfcsKnown,
            EntPtr rv;
            
            // Obtain trait pointer (for default impl and to know what the item type is)
            const auto& trait_ref = crate.get_trait_by_path(sp, e.trait.m_path);
            auto trait_vi_it = trait_ref.m_values.find(e.item);
            ASSERT_BUG(sp, trait_vi_it != trait_ref.m_values.end(), "Couldn't find item " << e.item << " in trait " << e.trait.m_path);
            const auto& trait_vi = trait_vi_it->second;
            

            if( e.item == "#vtable" ) {
                return EntPtr{};
            }
            
            ::std::vector<const ::HIR::TypeRef*>    best_impl_params;
            const ::HIR::TraitImpl* best_impl = nullptr;
            crate.find_trait_impls(e.trait.m_path, *e.type, [](const auto&x)->const auto& { return x; }, [&](const auto& impl) {
                
                ::std::vector<const ::HIR::TypeRef*>    impl_params;

                auto cb_ident = [](const auto& t)->const auto& { return t; };
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
                impl_params.resize(impl.m_params.m_types.size());
                auto match = impl.m_type.match_test_generics_fuzz(sp, *e.type, cb_ident, cb);
                match &= impl.m_trait_args.match_test_generics_fuzz(sp, e.trait.m_params, cb_ident, cb);
                if( match != ::HIR::Compare::Equal )
                    return false;
                
                if( best_impl == nullptr || impl.more_specific_than(*best_impl) ) {
                    best_impl = &impl;
                    bool is_spec = false;
                    TU_MATCHA( (trait_vi), (ve),
                    (Constant, ),
                    (Static,
                        //auto it = impl.m_statics.find(e.item);
                        //if( it == impl.m_statics.end() ) {
                        //    DEBUG("Static " << e.item << " missing in trait " << e.trait << " for " << *e.type);
                        //    return false;
                        //}
                        //is_spec = it->second.is_specialisable;
                        ),
                    (Function,
                        auto fit = impl.m_methods.find(e.item);
                        if( fit == impl.m_methods.end() ) {
                            DEBUG("Method " << e.item << " missing in trait " << e.trait << " for " << *e.type);
                            return false;
                        }
                        is_spec = fit->second.is_specialisable;
                        )
                    )
                    best_impl_params = mv$( impl_params );
                    return !is_spec;
                }
                return false;
                });
            if( !best_impl )
                return EntPtr {};
            const auto& impl = *best_impl;

            for(const auto* ty_ptr : best_impl_params) {
                assert( ty_ptr );
                impl_pp.m_types.push_back( ty_ptr->clone() );
            }
            
            TU_MATCHA( (trait_vi), (ve),
            (Constant, TODO(sp, "Associated constant"); ),
            (Static,
                TODO(sp, "Associated static - " << path);
                ),
            (Function,
                auto fit = impl.m_methods.find(e.item);
                if( fit != impl.m_methods.end() )
                {
                    DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                    return EntPtr { &fit->second.data };
                }
                impl_pp = e.trait.m_params.clone();
                return EntPtr { &ve };
                )
            )
            BUG(sp, "");
            ),
        (UfcsUnknown,
            // TODO: Are these valid at this point in compilation?
            TODO(sp, "get_ent_fullpath(path = " << path << ")");
            )
        )
        throw "";
    }
}

void Trans_Enumerate_FillFrom_Path(TransList& out, const ::HIR::Crate& crate, const ::HIR::Path& path, const Params& pp)
{
    Span    sp;
    auto path_mono = pp.monomorph(path);
    Params  sub_pp;
    TU_MATCHA( (path_mono.m_data), (pe),
    (Generic,
        sub_pp = Params { sp, pe.m_params.clone() };
        ),
    (UfcsKnown,
        sub_pp = Params { sp, pe.params.clone(), pe.impl_params.clone(), pe.type->clone() };
        ),
    (UfcsInherent,
        sub_pp = Params { sp, pe.params.clone(), pe.impl_params.clone(), pe.type->clone() };
        ),
    (UfcsUnknown,
        BUG(sp, "UfcsUnknown - " << path);
        )
    )
    // Get the item type
    // - Valid types are Function and Static
    auto item_ref = get_ent_fullpath(sp, crate, path_mono, sub_pp.pp_impl);
    TU_MATCHA( (item_ref), (e),
    (NotFound,
        // TODO: This should be an error... but vtable generation?
        ),
    (Function,
        // Add this path (monomorphised) to the queue
        if( out.add_function( mv$(path_mono), *e ) )
        {
            Trans_Enumerate_FillFrom(out,crate, *e, mv$(sub_pp));
        }
        ),
    (Static,
        if( out.add_static( mv$(path_mono), *e ) )
        {
            Trans_Enumerate_FillFrom(out,crate, *e, mv$(sub_pp));
        }
        )
    )
}
void Trans_Enumerate_FillFrom_MIR_LValue(TransList& out, const ::HIR::Crate& crate, const ::MIR::LValue& lv, const Params& pp)
{
    TU_MATCHA( (lv), (e),
    (Variable,
        ),
    (Temporary,
        ),
    (Argument,
        ),
    (Static,
        Trans_Enumerate_FillFrom_Path(out,crate, e, pp);
        ),
    (Return,
        ),
    (Field,
        Trans_Enumerate_FillFrom_MIR_LValue(out,crate, *e.val, pp);
        ),
    (Deref,
        Trans_Enumerate_FillFrom_MIR_LValue(out,crate, *e.val, pp);
        ),
    (Index,
        Trans_Enumerate_FillFrom_MIR_LValue(out,crate, *e.val, pp);
        Trans_Enumerate_FillFrom_MIR_LValue(out,crate, *e.idx, pp);
        ),
    (Downcast,
        Trans_Enumerate_FillFrom_MIR_LValue(out,crate, *e.val, pp);
        )
    )
}
void Trans_Enumerate_FillFrom_MIR(TransList& out, const ::HIR::Crate& crate, const ::MIR::Function& code, const Params& pp)
{
    for(const auto& bb : code.blocks)
    {
        for(const auto& stmt : bb.statements)
        {
            TU_MATCHA((stmt), (se),
            (Assign,
                Trans_Enumerate_FillFrom_MIR_LValue(out,crate, se.dst, pp);
                TU_MATCHA( (se.src), (e),
                (Use,
                    Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e, pp);
                    ),
                (Constant,
                    TU_MATCHA( (e), (ce),
                    (Int, ),
                    (Uint,),
                    (Float, ),
                    (Bool, ),
                    (Bytes, ),
                    (StaticString, ),  // String
                    (Const,
                        // TODO: Should this trigger anything?
                        ),
                    (ItemAddr,
                        Trans_Enumerate_FillFrom_Path(out,crate, ce, pp);
                        )
                    )
                    ),
                (SizedArray,
                    Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.val, pp);
                    ),
                (Borrow,
                    Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.val, pp);
                    ),
                (Cast,
                    Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.val, pp);
                    ),
                (BinOp,
                    Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.val_l, pp);
                    Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.val_r, pp);
                    ),
                (UniOp,
                    Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.val, pp);
                    ),
                (DstMeta,
                    Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.val, pp);
                    ),
                (MakeDst,
                    Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.ptr_val, pp);
                    Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.meta_val, pp);
                    ),
                (Tuple,
                    for(const auto& val : e.vals)
                        Trans_Enumerate_FillFrom_MIR_LValue(out,crate, val, pp);
                    ),
                (Array,
                    for(const auto& val : e.vals)
                        Trans_Enumerate_FillFrom_MIR_LValue(out,crate, val, pp);
                    ),
                (Variant,
                    Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.val, pp);
                    ),
                (Struct,
                    for(const auto& val : e.vals)
                        Trans_Enumerate_FillFrom_MIR_LValue(out,crate, val, pp);
                    )
                )
                ),
            (Drop,
                Trans_Enumerate_FillFrom_MIR_LValue(out,crate, se.slot, pp);
                // TODO: Ensure that the drop glue for this type is generated
                )
            )
        }
        TU_MATCHA( (bb.terminator), (e),
        (Incomplete, ),
        (Return, ),
        (Diverge, ),
        (Goto, ),
        (Panic, ),
        (If,
            Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.cond, pp);
            ),
        (Switch,
            Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.val, pp);
            ),
        (Call,
            Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.ret_val, pp);
            Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e.fcn_val, pp);
            for(const auto& arg : e.args)
                Trans_Enumerate_FillFrom_MIR_LValue(out,crate, arg, pp);
            )
        )
    }
}

void Trans_Enumerate_FillFrom(TransList& out, const ::HIR::Crate& crate, const ::HIR::Function& function, Params pp)
{
    TRACE_FUNCTION_F("Function pp=" << pp.pp_method<<"+"<<pp.pp_impl);
    if( function.m_code.m_mir )
        Trans_Enumerate_FillFrom_MIR(out, crate, *function.m_code.m_mir, pp);
}
void Trans_Enumerate_FillFrom(TransList& out, const ::HIR::Crate& crate, const ::HIR::Static& item, Params pp)
{
    TRACE_FUNCTION;
    if( item.m_value.m_mir )
        Trans_Enumerate_FillFrom_MIR(out, crate, *item.m_value.m_mir, pp);
}

t_cb_generic Params::get_cb() const
{
    return monomorphise_type_get_cb(sp, &self_type, &pp_impl, &pp_method);
}
::HIR::Path Params::monomorph(const ::HIR::Path& p) const
{
    return monomorphise_path_with(sp, p, this->get_cb(), false);
}

