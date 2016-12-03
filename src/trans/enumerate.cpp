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
#include <hir_typeck/common.hpp>    // monomorph
#include <hir_typeck/static.hpp>    // StaticTraitResolve


void Trans_Enumerate_FillFrom(TransList& out, const ::HIR::Crate& crate, const ::HIR::Function& function, TransList_Function& fcn_out, Trans_Params pp={});
void Trans_Enumerate_FillFrom(TransList& out, const ::HIR::Crate& crate, const ::HIR::Static& stat, TransList_Static& stat_out, Trans_Params pp={});

/// Enumerate trans items starting from `::main` (binary crate)
TransList Trans_Enumerate_Main(const ::HIR::Crate& crate)
{
    static Span sp;
    
    TransList   rv;
    auto main_path = ::HIR::SimplePath("", {"main"});
    const auto& fcn = crate.get_function_by_path(sp, main_path);
    
    auto* ptr = rv.add_function(main_path);
    assert(ptr);
    Trans_Enumerate_FillFrom(rv, crate,  fcn, *ptr);
    
    return rv;
}

namespace {
    void Trans_Enumerate_Public_Mod(TransList& out, const ::HIR::Crate& crate, const ::HIR::Module& mod, ::HIR::SimplePath mod_path)
    {
        for(const auto& vi : mod.m_value_items)
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
                    auto* ptr = out.add_static(mod_path + vi.first);
                    if(ptr)
                        Trans_Enumerate_FillFrom(out,crate, e, *ptr);
                    ),
                (Function,
                    if( e.m_params.m_types.size() == 0 )
                    {
                        auto* ptr = out.add_function(mod_path + vi.first);
                        if(ptr)
                            Trans_Enumerate_FillFrom(out,crate, e, *ptr);
                    }
                    )
                )
            }
        }
    }
}

/// Enumerate trans items for all public non-generic items (library crate)
TransList Trans_Enumerate_Public(const ::HIR::Crate& crate)
{
    TransList   rv;
    
    Trans_Enumerate_Public_Mod(rv, crate, crate.m_root_module,  ::HIR::SimplePath("",{}));
    
    return rv;
}

namespace {
    TAGGED_UNION(EntPtr, NotFound,
        (NotFound, struct{}),
        (VTable, struct{}),
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
        TRACE_FUNCTION_F(path);
        StaticTraitResolve  resolve { crate };
        
        TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
        (Generic,
            return get_ent_simplepath(sp, crate, e.m_path);
            ),
        (UfcsInherent,
            // Easy (ish)
            EntPtr rv;
            crate.find_type_impls(*e.type, [](const auto&x)->const auto& { return x; }, [&](const auto& impl) {
                DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                {
                    auto fit = impl.m_methods.find(e.item);
                    if( fit != impl.m_methods.end() )
                    {
                        DEBUG("- Contains method, good");
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
                return EntPtr::make_VTable({});
            }
            
            bool is_dynamic = false;
            ::std::vector<::HIR::TypeRef>    best_impl_params;
            const ::HIR::TraitImpl* best_impl = nullptr;
            resolve.find_impl(sp, e.trait.m_path, e.trait.m_params, *e.type, [&](auto impl_ref, auto is_fuzz) {
                DEBUG("Found " << impl_ref);
                //ASSERT_BUG(sp, !is_fuzz, "Fuzzy match not allowed here");
                if( ! impl_ref.m_data.is_TraitImpl() ) {
                    DEBUG("Trans impl search found an invalid impl type");
                    is_dynamic = true;
                    // TODO: This can only really happen if it's a trait object magic impl, which should become a vtable lookup.
                    return true;
                }
                const auto& impl_ref_e = impl_ref.m_data.as_TraitImpl();
                const auto& impl = *impl_ref_e.impl;
                ASSERT_BUG(sp, impl.m_trait_args.m_types.size() == e.trait.m_params.m_types.size(), "Trait parameter count mismatch " << impl.m_trait_args << " vs " << e.trait.m_params);
                
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
                    best_impl_params.clear();
                    for(unsigned int i = 0; i < impl_ref_e.params.size(); i ++)
                    {
                        if( impl_ref_e.params[i] )
                            best_impl_params.push_back( impl_ref_e.params[i]->clone() );
                        else if( ! impl_ref_e.params_ph[i].m_data.is_Generic() )
                            best_impl_params.push_back( impl_ref_e.params_ph[i].clone() );
                        else
                            BUG(sp, "Parameter " << i << " unset");
                    }
                    return !is_spec;
                }
                return false;
                });
            if( is_dynamic )
                return EntPtr::make_VTable( {} );
            if( !best_impl )
                return EntPtr {};
            const auto& impl = *best_impl;

            impl_pp.m_types = mv$(best_impl_params);
            
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

void Trans_Enumerate_FillFrom_Path(TransList& out, const ::HIR::Crate& crate, const ::HIR::Path& path, const Trans_Params& pp)
{
    TRACE_FUNCTION_F(path);
    Span    sp;
    auto path_mono = pp.monomorph(crate, path);
    Trans_Params  sub_pp(sp);
    TU_MATCHA( (path_mono.m_data), (pe),
    (Generic,
        sub_pp.pp_method = pe.m_params.clone();
        ),
    (UfcsKnown,
        sub_pp.pp_method = pe.params.clone();
        sub_pp.self_type = pe.type->clone();
        ),
    (UfcsInherent,
        sub_pp.pp_method = pe.params.clone();
        sub_pp.pp_impl = pe.impl_params.clone();
        sub_pp.self_type = pe.type->clone();
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
        BUG(sp, "Item not found for " << path_mono);
        ),
    (VTable,
        // This is returned either if the item is <T as U>::#vtable or if it's <(Trait) as Trait>::method
        if( path_mono.m_data.as_UfcsKnown().item == "#vtable" )
        {
            // TODO: Vtable generation
            //if( auto* ptr = out.add_vtable(mv$(path_mono)) )
            //{
            //}
        }
        else if( path_mono.m_data.as_UfcsKnown().type->m_data.is_TraitObject() )
        {
            // Must have been a dynamic dispatch request, just leave as-is
        }
        else
        {
            BUG(sp, "");
        }
        ),
    (Function,
        // Add this path (monomorphised) to the queue
        if( auto* ptr = out.add_function(mv$(path_mono)) )
        {
            Trans_Enumerate_FillFrom(out,crate, *e, *ptr, mv$(sub_pp));
        }
        ),
    (Static,
        if( auto* ptr = out.add_static(mv$(path_mono)) )
        {
            Trans_Enumerate_FillFrom(out,crate, *e, *ptr, mv$(sub_pp));
        }
        )
    )
}
void Trans_Enumerate_FillFrom_MIR_LValue(TransList& out, const ::HIR::Crate& crate, const ::MIR::LValue& lv, const Trans_Params& pp)
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
void Trans_Enumerate_FillFrom_MIR(TransList& out, const ::HIR::Crate& crate, const ::MIR::Function& code, const Trans_Params& pp)
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
                (DstPtr,
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

void Trans_Enumerate_FillFrom(TransList& out, const ::HIR::Crate& crate, const ::HIR::Function& function, TransList_Function& out_fcn, Trans_Params pp)
{
    TRACE_FUNCTION_F("Function pp=" << pp.pp_method<<"+"<<pp.pp_impl);
    if( function.m_code.m_mir )
    {
        Trans_Enumerate_FillFrom_MIR(out, crate, *function.m_code.m_mir, pp);
        out_fcn.ptr = &function;
        out_fcn.pp = mv$(pp);
    }
    else
    {
        out_fcn.ptr = nullptr;
    }
}
void Trans_Enumerate_FillFrom(TransList& out, const ::HIR::Crate& crate, const ::HIR::Static& item, TransList_Static& out_stat, Trans_Params pp)
{
    TRACE_FUNCTION;
    if( item.m_value.m_mir )
    {
        Trans_Enumerate_FillFrom_MIR(out, crate, *item.m_value.m_mir, pp);
        out_stat.ptr = &item;
        out_stat.pp = mv$(pp);
    }
    else
    {
        out_stat.ptr = nullptr;
    }
}

