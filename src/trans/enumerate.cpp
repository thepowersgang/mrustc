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
#include <hir/item_path.hpp>


void Trans_Enumerate_Types(TransList& out, const ::HIR::Crate& crate);
void Trans_Enumerate_FillFrom_Path(TransList& out, const ::HIR::Crate& crate, const ::HIR::Path& path, const Trans_Params& pp);
void Trans_Enumerate_FillFrom(TransList& out, const ::HIR::Crate& crate, const ::HIR::Function& function, TransList_Function& fcn_out, Trans_Params pp={});
void Trans_Enumerate_FillFrom(TransList& out, const ::HIR::Crate& crate, const ::HIR::Static& stat, TransList_Static& stat_out, Trans_Params pp={});
void Trans_Enumerate_FillFrom_VTable(TransList& out, const ::HIR::Crate& crate, ::HIR::Path vtable_path, const Trans_Params& pp);
void Trans_Enumerate_FillFrom_Literal(TransList& out, const ::HIR::Crate& crate, const ::HIR::Literal& lit, const Trans_Params& pp);

/// Enumerate trans items starting from `::main` (binary crate)
TransList Trans_Enumerate_Main(const ::HIR::Crate& crate)
{
    static Span sp;
    
    TransList   rv;
    
    // "start" language item
    // - Takes main, and argc/argv as arguments
    {
        auto start_path = crate.get_lang_item_path(sp, "start");
        const auto& fcn = crate.get_function_by_path(sp, start_path);
        
        auto* ptr = rv.add_function(start_path);
        assert(ptr);
        Trans_Enumerate_FillFrom(rv, crate,  fcn, *ptr);
    }
    
    // user entrypoint
    {
        auto main_path = ::HIR::SimplePath("", {"main"});
        const auto& fcn = crate.get_function_by_path(sp, main_path);
        
        auto* ptr = rv.add_function(main_path);
        assert(ptr);
        Trans_Enumerate_FillFrom(rv, crate,  fcn, *ptr);
    }
    
    // TODO: Search the trans list for external functions that refer to a named symbol, search for that defined elsewhere.
    // - Not needed yet, there's a slow hack elsewhere

    Trans_Enumerate_Types(rv, crate);
    
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
    
    Trans_Enumerate_Types(rv, crate);
    
    return rv;
}

namespace {
    struct PtrComp
    {
        template<typename T>
        bool operator()(const T* lhs, const T* rhs) const { return *lhs < *rhs; }
    };
    
    struct TypeVisitor
    {
        const ::HIR::Crate& m_crate;
        ::std::vector< ::HIR::TypeRef>& out_list;
        
        ::std::set< ::HIR::TypeRef> visited;
        ::std::set< const ::HIR::TypeRef*, PtrComp> active_set;
        
        TypeVisitor(const ::HIR::Crate& crate, ::std::vector< ::HIR::TypeRef>& out_list):
            m_crate(crate),
            out_list(out_list)
        {}
        
        void visit_struct(const ::HIR::GenericPath& path, const ::HIR::Struct& item) {
            static Span sp;
            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& {
                if( monomorphise_type_needed(x) ) {
                    tmp = monomorphise_type(sp, item.m_params, path.m_params, x);
                    //m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return x;
                }
                };
            TU_MATCHA( (item.m_data), (e),
            (Unit,
                ),
            (Tuple,
                for(const auto& fld : e) {
                    visit_type( monomorph(fld.ent) );
                }
                ),
            (Named,
                for(const auto& fld : e)
                    visit_type( monomorph(fld.second.ent) );
                )
            )
        }
        void visit_union(const ::HIR::GenericPath& path, const ::HIR::Union& item) {
            // TODO: .
        }
        void visit_enum(const ::HIR::GenericPath& path, const ::HIR::Enum& item) {
            static Span sp;
            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& {
                if( monomorphise_type_needed(x) ) {
                    tmp = monomorphise_type(sp, item.m_params, path.m_params, x);
                    //m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return x;
                }
                };
            for(const auto& variant : item.m_variants)
            {
                TU_MATCHA( (variant.second), (e),
                (Unit,
                    ),
                (Value,
                    ),
                (Tuple,
                    for(const auto& ty : e)
                        visit_type( monomorph(ty.ent) );
                    ),
                (Struct,
                    for(const auto& fld : e)
                        visit_type( monomorph(fld.second.ent) );
                    )
                )
            }
        }
        
        void visit_type(const ::HIR::TypeRef& ty)
        {
            // Already done
            if( visited.find(ty) != visited.end() )
                return ;
            
            if( active_set.find(&ty) != active_set.end() ) {
                // TODO: Handle recursion
                return ;
            }
            active_set.insert( &ty );
            
            TU_MATCHA( (ty.m_data), (te),
            // Impossible
            (Infer,
                ),
            (Generic,
                ),
            (ErasedType,
                ),
            (Closure,
                ),
            // Nothing to do
            (Diverge,
                ),
            (Primitive,
                ),
            // Recursion!
            (Path,
                TU_MATCHA( (te.binding), (tpb),
                (Unbound,   ),
                (Opaque,   ),
                (Struct,
                    visit_struct(te.path.m_data.as_Generic(), *tpb);
                    ),
                (Union,
                    visit_union(te.path.m_data.as_Generic(), *tpb);
                    ),
                (Enum,
                    visit_enum(te.path.m_data.as_Generic(), *tpb);
                    )
                )
                ),
            (TraitObject,
                static Span sp;
                // Ensure that the data trait's vtable is present
                const auto& trait = *te.m_trait.m_trait_ptr;
                
                auto vtable_ty_spath = te.m_trait.m_path.m_path;
                vtable_ty_spath.m_components.back() += "#vtable";
                const auto& vtable_ref = m_crate.get_struct_by_path(sp, vtable_ty_spath);
                // Copy the param set from the trait in the trait object
                ::HIR::PathParams   vtable_params = te.m_trait.m_path.m_params.clone();
                // - Include associated types on bound
                for(const auto& ty_b : te.m_trait.m_type_bounds) {
                    auto idx = trait.m_type_indexes.at(ty_b.first);
                    if(vtable_params.m_types.size() <= idx)
                        vtable_params.m_types.resize(idx+1);
                    vtable_params.m_types[idx] = ty_b.second.clone();
                }
                
                
                visit_type( ::HIR::TypeRef( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref ) );
                ),
            (Array,
                visit_type(*te.inner);
                ),
            (Slice,
                visit_type(*te.inner);
                ),
            (Borrow,
                visit_type(*te.inner);
                ),
            (Pointer,
                visit_type(*te.inner);
                ),
            (Tuple,
                for(const auto& sty : te)
                    visit_type(sty);
                ),
            (Function,
                visit_type(*te.m_rettype);
                for(const auto& sty : te.m_arg_types)
                    visit_type(sty);
                )
            )
            active_set.erase( active_set.find(&ty) );
            
            visited.insert( ty.clone() );
            out_list.push_back( ty.clone() );
        }
    };
}

// Enumerate types required for the enumerated items
void Trans_Enumerate_Types(TransList& out, const ::HIR::Crate& crate)
{
    TypeVisitor tv { crate, out.m_types };

    unsigned int types_count = 0;
    bool constructors_added;
    do
    {
        for(const auto& ent : out.m_functions)
        {
            TRACE_FUNCTION_F("Enumerate fn " << ent.first);
            assert(ent.second->ptr);
            const auto& fcn = *ent.second->ptr;
            const auto& pp = ent.second->pp;
            
            tv.visit_type( pp.monomorph(crate, fcn.m_return) );
            for(const auto& arg : fcn.m_args)
                tv.visit_type( pp.monomorph(crate, arg.second) );
            
            if( fcn.m_code.m_mir )
            {
                const auto& mir = *fcn.m_code.m_mir;
                for(const auto& ty : mir.named_variables)
                    tv.visit_type(pp.monomorph(crate, ty));
                for(const auto& ty : mir.temporaries)
                    tv.visit_type(pp.monomorph(crate, ty));
            }
        }
        for(const auto& ent : out.m_statics)
        {
            TRACE_FUNCTION_F("Enumerate static " << ent.first);
            assert(ent.second->ptr);
            const auto& stat = *ent.second->ptr;
            const auto& pp = ent.second->pp;
            
            tv.visit_type( pp.monomorph(crate, stat.m_type) );
        }
        
        constructors_added = false;
        for(unsigned int i = types_count; i < out.m_types.size(); i ++ )
        {
            const auto& ty = out.m_types[i];
            if( ty.m_data.is_Path() )
            {
                const auto& te = ty.m_data.as_Path();
                const ::HIR::TraitMarkings* markings_ptr = nullptr;
                TU_MATCHA( (te.binding), (tpb),
                (Unbound,   ),
                (Opaque,   ),
                (Struct,
                    markings_ptr = &tpb->m_markings;
                    ),
                (Union,
                    markings_ptr = &tpb->m_markings;
                    ),
                (Enum,
                    markings_ptr = &tpb->m_markings;
                    )
                )
                ASSERT_BUG(Span(), markings_ptr, "");
                
                if( markings_ptr->has_drop_impl )
                {
                    // Add the Drop impl to the codegen list
                    Trans_Enumerate_FillFrom_Path(out, crate,  ::HIR::Path( ty.clone(), crate.get_lang_item_path(Span(), "drop"), "drop"), {});
                    
                    constructors_added = true;
                }
            }
        }
        types_count = out.m_types.size();
    } while(constructors_added);
}

namespace {
    TAGGED_UNION(EntPtr, NotFound,
        (NotFound, struct{}),
        (AutoGenerate, struct{}),
        (Function, const ::HIR::Function*),
        (Static, const ::HIR::Static*),
        (Constant, const ::HIR::Constant*)
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
            (Enum,
                ASSERT_BUG(sp, i == path.m_components.size() - 2, "Enum found somewhere other than penultimate posiiton in " << path);
                // TODO: Check that this is a tuple variant
                return EntPtr::make_AutoGenerate({});
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
            // TODO: What to do with these?
            return EntPtr::make_AutoGenerate({});
            ),
        (Function,
            return EntPtr { &e };
            ),
        (Constant,
            return EntPtr { &e };
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
                        auto it = impl.m_statics.find(e.item);
                        if( it == impl.m_statics.end() ) {
                            DEBUG("Static " << e.item << " missing in trait " << e.trait << " for " << *e.type);
                            return false;
                        }
                        is_spec = it->second.is_specialisable;
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
                return EntPtr::make_AutoGenerate( {} );
            if( !best_impl )
                return EntPtr {};
            const auto& impl = *best_impl;

            impl_pp.m_types = mv$(best_impl_params);
            
            TU_MATCHA( (trait_vi), (ve),
            (Constant, TODO(sp, "Associated constant"); ),
            (Static,
                auto it = impl.m_statics.find(e.item);
                if( it != impl.m_statics.end() )
                {
                    DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                    return EntPtr { &it->second.data };
                }
                if( e.item == "#vtable" )
                    return EntPtr::make_AutoGenerate( {} );
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
    (AutoGenerate,
        // This is returned either if the item is <T as U>::#vtable or if it's <(Trait) as Trait>::method
        if( path_mono.m_data.is_Generic() )
        {
            // Leave generation of struct/enum constructors to codgen
        }
        else if( path_mono.m_data.as_UfcsKnown().item == "#vtable" )
        {
            if( out.add_vtable( path_mono.clone(), {} ) )
            {
                // Fill from the vtable
                Trans_Enumerate_FillFrom_VTable(out,crate, mv$(path_mono), sub_pp);
            }
        }
        else if( path_mono.m_data.as_UfcsKnown().type->m_data.is_TraitObject() )
        {
            // Must have been a dynamic dispatch request, just leave as-is
        }
        else if( path_mono.m_data.as_UfcsKnown().type->m_data.is_Function() )
        {
            // Must have been a dynamic dispatch request, just leave as-is
        }
        else
        {
            BUG(sp, "AutoGenerate returned for unknown path type - " << path_mono);
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
        ),
    (Constant,
        Trans_Enumerate_FillFrom_Literal(out, crate, e->m_value_res, sub_pp);
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
                        Trans_Enumerate_FillFrom_Path(out,crate, ce.p, pp);
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
            TU_MATCHA( (e.fcn), (e2),
            (Value,
                Trans_Enumerate_FillFrom_MIR_LValue(out,crate, e2, pp);
                ),
            (Path,
                Trans_Enumerate_FillFrom_Path(out,crate, e2, pp);
                ),
            (Intrinsic,
                )
            )
            for(const auto& arg : e.args)
                Trans_Enumerate_FillFrom_MIR_LValue(out,crate, arg, pp);
            )
        )
    }
}

void Trans_Enumerate_FillFrom_VTable(TransList& out, const ::HIR::Crate& crate, ::HIR::Path vtable_path, const Trans_Params& pp)
{
    static Span sp;
    const auto& type = *vtable_path.m_data.as_UfcsKnown().type;
    const auto& trait_path = vtable_path.m_data.as_UfcsKnown().trait;
    const auto& tr = crate.get_trait_by_path(Span(), trait_path.m_path);
    
    auto monomorph_cb_trait = monomorphise_type_get_cb(sp, &type, &trait_path.m_params, nullptr);
    for(const auto& m : tr.m_value_indexes)
    {
        DEBUG("- " << m.second.first << " = " << m.second.second << " :: " << m.first);
        auto gpath = monomorphise_genericpath_with(sp, m.second.second, monomorph_cb_trait, false);
        Trans_Enumerate_FillFrom_Path(out,crate, ::HIR::Path(type.clone(), mv$(gpath), m.first), {});
    }
}

void Trans_Enumerate_FillFrom_Literal(TransList& out, const ::HIR::Crate& crate, const ::HIR::Literal& lit, const Trans_Params& pp)
{
    TU_MATCHA( (lit), (e),
    (Invalid,
        ),
    (List,
        for(const auto& v : e)
            Trans_Enumerate_FillFrom_Literal(out, crate, v, pp);
        ),
    (Variant,
        for(const auto& v : e.vals)
            Trans_Enumerate_FillFrom_Literal(out, crate, v, pp);
        ),
    (Integer,
        ),
    (Float,
        ),
    (BorrowOf,
        Trans_Enumerate_FillFrom_Path(out, crate, e, pp);
        ),
    (String,
        )
    )
}

namespace {
    ::HIR::Function* find_function_by_link_name(const ::HIR::Module& mod, ::HIR::ItemPath mod_path,  const char* name,  ::HIR::SimplePath& out_path)
    {
        for(const auto& vi : mod.m_value_items)
        {
            TU_IFLET( ::HIR::ValueItem, vi.second->ent, Function, i,
                if( i.m_code.m_mir && i.m_linkage.name != "" && i.m_linkage.name == name )
                {
                    out_path = (mod_path + vi.first.c_str()).get_simple_path();
                    return &i;
                }
            )
        }
        
        for(const auto& ti : mod.m_mod_items)
        {
            TU_IFLET( ::HIR::TypeItem, ti.second->ent, Module, i,
                if( auto rv = find_function_by_link_name(i, mod_path + ti.first.c_str(), name,  out_path) )
                    return rv;
            )
        }
        
        return nullptr;
    }
    ::HIR::Function* find_function_by_link_name(const ::HIR::Crate& crate, const char* name,  ::HIR::SimplePath& out_path)
    {
        if(auto rv = find_function_by_link_name(crate.m_root_module, {}, name, out_path))
            return rv;
        for(const auto& e_crate : crate.m_ext_crates)
        {
            if(auto rv = find_function_by_link_name(e_crate.second->m_root_module, {}, name,  out_path))
            {
                out_path.m_crate_name = e_crate.first;
                return rv;
            }
        }
        return nullptr;
    }
}

void Trans_Enumerate_FillFrom(TransList& out, const ::HIR::Crate& crate, const ::HIR::Function& function, TransList_Function& out_fcn, Trans_Params pp)
{
    TRACE_FUNCTION_F("Function pp=" << pp.pp_method<<"+"<<pp.pp_impl);
    if( function.m_code.m_mir )
    {
        Trans_Enumerate_FillFrom_MIR(out, crate, *function.m_code.m_mir, pp);
    }
    else
    {
        if( function.m_linkage.name != "" )
        {
            // Search for a function with the same linkage name anywhere in the loaded crates
            ::HIR::SimplePath   path;
            if(const auto* f = find_function_by_link_name(crate, function.m_linkage.name.c_str(), path))
            {
                auto path_mono = ::HIR::Path( mv$(path) );
                if( auto* ptr = out.add_function(mv$(path_mono)) )
                {
                    Trans_Enumerate_FillFrom(out,crate, *f, *ptr, Trans_Params(pp.sp));
                }
            }
        }
        // External.
    }
    out_fcn.ptr = &function;
    out_fcn.pp = mv$(pp);
}
void Trans_Enumerate_FillFrom(TransList& out, const ::HIR::Crate& crate, const ::HIR::Static& item, TransList_Static& out_stat, Trans_Params pp)
{
    TRACE_FUNCTION;
    if( item.m_value.m_mir )
    {
        Trans_Enumerate_FillFrom_MIR(out, crate, *item.m_value.m_mir, pp);
    }
    else if( ! item.m_value_res.is_Invalid() )
    {
        Trans_Enumerate_FillFrom_Literal(out, crate, item.m_value_res, pp);
    }
    out_stat.ptr = &item;
    out_stat.pp = mv$(pp);
}

