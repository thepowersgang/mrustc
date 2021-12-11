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
#include <mir/helpers.hpp>
#include <hir_typeck/common.hpp>    // monomorph
#include <hir_typeck/static.hpp>    // StaticTraitResolve
#include <hir/item_path.hpp>
#include <deque>
#include <algorithm>
#include "target.hpp"

namespace {
    struct EnumState
    {
        const ::HIR::Crate& crate;
        TransList   rv;

        // Queue of items to enumerate
        ::std::deque<TransList_Function*>  fcn_queue;
        ::std::vector<TransList_Function*> fcns_to_type_visit;

        EnumState(const ::HIR::Crate& crate):
            crate(crate)
        {}

        void enum_fcn(::HIR::Path p, const ::HIR::Function& fcn, Trans_Params pp)
        {
            if(auto* e = rv.add_function(mv$(p)))
            {
                fcns_to_type_visit.push_back(e);
                e->ptr = &fcn;
                e->pp = mv$(pp);
                fcn_queue.push_back(e);
            }
        }
    };
}

TransList Trans_Enumerate_CommonPost(EnumState& state);
void Trans_Enumerate_Types(EnumState& state);
void Trans_Enumerate_FillFrom_Path(EnumState& state, const ::HIR::Path& path, const Trans_Params& pp);
void Trans_Enumerate_FillFrom_PathMono(EnumState& state, ::HIR::Path path);
void Trans_Enumerate_FillFrom(EnumState& state, const ::HIR::Function& function, const Trans_Params& pp);
void Trans_Enumerate_FillFrom(EnumState& state, const ::HIR::Static& stat, TransList_Static& stat_out, Trans_Params pp={});
void Trans_Enumerate_FillFrom_VTable (EnumState& state, ::HIR::Path vtable_path, const Trans_Params& pp);
void Trans_Enumerate_FillFrom_Literal(EnumState& state, const EncodedLiteral& lit, const Trans_Params& pp);
void Trans_Enumerate_FillFrom_MIR(MIR::EnumCache& state, const ::MIR::Function& code);


namespace MIR {
    struct EnumCache
    {
        ::std::vector<const ::HIR::Path*>  paths;
        ::std::vector<const ::HIR::TypeRef*>  typeids;
        EnumCache()
        {
        }
        void insert_path(const ::HIR::Path& new_path)
        {
            for(const auto* p : this->paths)
                if( *p == new_path )
                    return ;
            this->paths.push_back(&new_path);
        }
        void insert_typeid(const ::HIR::TypeRef& new_ty)
        {
            for(const auto* p : this->typeids)
                if( *p == new_ty )
                    return ;
            this->typeids.push_back(&new_ty);
        }

        void apply(EnumState& state, const Trans_Params& pp) const
        {
            TRACE_FUNCTION_F("");
            for(const auto* ty_p : this->typeids)
            {
                state.rv.m_typeids.insert( pp.monomorph(state.crate, *ty_p) );
            }
            for(const auto& path : this->paths)
            {
                Trans_Enumerate_FillFrom_Path(state, *path, pp);
            }
        }
    };
    EnumCachePtr::~EnumCachePtr()
    {
        delete this->p;
        this->p = nullptr;
    }
}

/// Enumerate trans items starting from `::main` (binary crate)
TransList Trans_Enumerate_Main(const ::HIR::Crate& crate)
{
    static Span sp;

    EnumState   state { crate };

    auto c_start_path = crate.get_lang_item_path_opt("mrustc-start");
    if( c_start_path == ::HIR::SimplePath() )
    {
        // user entrypoint
        auto main_path = crate.get_lang_item_path(Span(), "mrustc-main");
        const auto& main_fcn = crate.get_function_by_path(sp, main_path);

        state.enum_fcn( main_path, main_fcn, {} );

        // "start" language item
        // - Takes main, and argc/argv as arguments
        {
            auto start_path = crate.get_lang_item_path(sp, "start");
            const auto& fcn = crate.get_function_by_path(sp, start_path);

            Trans_Params    lang_start_pp;
            if( TARGETVER_LEAST_1_29 )
            {
                // With 1.29, this now takes main's return type as a type parameter
                lang_start_pp.pp_method.m_types.push_back( main_fcn.m_return.clone() );
            }
            state.enum_fcn( start_path, fcn, mv$(lang_start_pp) );
        }
    }
    else
    {
        const auto& fcn = crate.get_function_by_path(sp, c_start_path);

        state.enum_fcn( c_start_path, fcn, {} );
    }

    return Trans_Enumerate_CommonPost(state);
}

namespace {
    void Trans_Enumerate_ValItem(EnumState& state, const ::HIR::ValueItem& vi, bool is_visible, ::std::function<::HIR::SimplePath()> get_path)
    {
        switch(vi.tag())
        {
        case ::HIR::ValueItem::TAGDEAD: throw "";
        TU_ARM(vi, Import, e) {
            // TODO: If visible, ensure that target is visited.
            if( is_visible )
            {
                if( ! e.is_variant && e.path.m_crate_name == state.crate.m_crate_name )
                {
                    const auto& vi2 = state.crate.get_valitem_by_path(Span(), e.path, false);
                    Trans_Enumerate_ValItem(state, vi2, is_visible, [&](){ return e.path; });
                }
            }
            } break;
        TU_ARM(vi, StructConstant, e) {
            } break;
        TU_ARM(vi, StructConstructor, e) {
            } break;
        TU_ARM(vi, Constant, e) {
            } break;
        TU_ARM(vi, Static, e) {
            if( is_visible )
            {
                // HACK: Refuse to emit unused generated statics
                // - Needed because all items are visited (regardless of
                // visibility)
                if(e.m_type.data().is_Infer())
                    continue ;
                //state.enum_static(mod_path + vi.first, *e);
                auto* ptr = state.rv.add_static( get_path() );
                if(ptr)
                    Trans_Enumerate_FillFrom(state, e, *ptr);
            }
            } break;
        TU_ARM(vi, Function, e) {
            if( !e.m_params.is_generic() )
            {
                if( is_visible ) {
                    state.enum_fcn(get_path(), e, {});
                }
            }
            else
            {
                const_cast<::HIR::Function&>(e).m_save_code = true;
                // TODO: If generic, enumerate concrete functions used
            }
            } break;
        }
    }
    void Trans_Enumerate_Public_Mod(EnumState& state, ::HIR::Module& mod, ::HIR::SimplePath mod_path, bool is_visible)
    {
        // TODO: Fix the TODO in Function above (scan generics for the concretes they use) and set this to false again
        const bool EMIT_ALL = true;
        for(auto& vi : mod.m_value_items)
        {
            Trans_Enumerate_ValItem(state, vi.second->ent, EMIT_ALL || (is_visible && vi.second->publicity.is_global()), [&](){ return mod_path + vi.first; });
        }

        for(auto& ti : mod.m_mod_items)
        {
            if(auto* e = ti.second->ent.opt_Module() )
            {
                Trans_Enumerate_Public_Mod(state, *e, mod_path + ti.first, ti.second->publicity.is_global());
            }
        }
    }

    void Trans_Enumerate_Public_TraitImpl(EnumState& state, StaticTraitResolve& resolve, const ::HIR::SimplePath& trait_path, /*const*/ ::HIR::TraitImpl& impl)
    {
        static Span sp;
        const auto& impl_ty = impl.m_type;
        TRACE_FUNCTION_F("Impl " << trait_path << impl.m_trait_args << " for " << impl_ty);
        if( !impl.m_params.is_generic() )
        {
            auto cb_monomorph = MonomorphStatePtr(&impl_ty, &impl.m_trait_args, nullptr);

            // Emit each method/static (in the trait itself)
            const auto& trait = resolve.m_crate.get_trait_by_path(sp, trait_path);
            for(const auto& vi : trait.m_values)
            {
                TRACE_FUNCTION_F("Item " << vi.first << " : " << vi.second.tag_str());
                // Constant, no codegen
                if( vi.second.is_Constant() )
                    ;
                // Generic method, no codegen
                else if( vi.second.is_Function() && vi.second.as_Function().m_params.is_generic() )
                    ;
                // VTable, magic
                else if( vi.first == "vtable#" )
                    ;
                else
                {
                    // Check bounds before queueing for codegen
                    if( vi.second.is_Function() )
                    {
                        bool rv = true;
                        for(const auto& b : vi.second.as_Function().m_params.m_bounds)
                        {
                            if( !b.is_TraitBound() )    continue;
                            const auto& be = b.as_TraitBound();

                            auto b_ty_mono = resolve.monomorph_expand(sp, be.type, cb_monomorph);
                            auto b_tp_mono = cb_monomorph.monomorph_traitpath(sp, be.trait, false);
                            resolve.expand_associated_types_tp(sp, b_tp_mono);

                            rv = resolve.find_impl(sp, b_tp_mono.m_path.m_path, b_tp_mono.m_path.m_params, b_ty_mono, [&](const auto& impl, bool) {
                                return true;
                                });
                            if( !rv )
                                break;
                        }
                        if( !rv )
                            continue ;
                    }
                    auto path = ::HIR::Path(impl_ty.clone(), ::HIR::GenericPath(trait_path, impl.m_trait_args.clone()), vi.first);
                    Trans_Enumerate_FillFrom_PathMono(state, mv$(path));
                    //state.enum_fcn(mv$(path), fcn.second.data, {});
                }
            }
            for(auto& m : impl.m_methods)
            {
                if( m.second.data.m_params.is_generic() )
                    m.second.data.m_save_code = true;
            }
        }
        else
        {
            for(auto& m : impl.m_methods)
            {
                m.second.data.m_save_code = true;
            }
        }
    }
}

/// Enumerate trans items for all public non-generic items (library crate)
TransList Trans_Enumerate_Public(::HIR::Crate& crate)
{
    static Span sp;
    EnumState   state { crate };

    Trans_Enumerate_Public_Mod(state, crate.m_root_module,  ::HIR::SimplePath(crate.m_crate_name,{}), true);

    // Impl blocks
    StaticTraitResolve resolve { crate };
    for(auto& impl_group : crate.m_trait_impls)
    {
        const auto& trait_path = impl_group.first;
        for(auto& impl_list : impl_group.second.named)
        {
            for(auto& impl : impl_list.second)
            {
                Trans_Enumerate_Public_TraitImpl(state, resolve, trait_path, *impl);
            }
        }
        for(auto& impl : impl_group.second.non_named)
        {
            Trans_Enumerate_Public_TraitImpl(state, resolve, trait_path, *impl);
        }
        for(auto& impl : impl_group.second.generic)
        {
            Trans_Enumerate_Public_TraitImpl(state, resolve, trait_path, *impl);
        }
    }
    struct H1
    {
        static void enumerate_type_impl(EnumState& state, ::HIR::TypeImpl& impl)
        {
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << impl.m_type);
            if( !impl.m_params.is_generic() )
            {
                for(auto& fcn : impl.m_methods)
                {
                    DEBUG("fn " << fcn.first << fcn.second.data.m_params.fmt_args());
                    if( !fcn.second.data.m_params.is_generic() )
                    {
                        auto path = ::HIR::Path(impl.m_type.clone(), fcn.first);
                        state.enum_fcn(mv$(path), fcn.second.data, {});
                    }
                    else
                    {
                        fcn.second.data.m_save_code = true;
                    }
                }
            }
            else
            {
                for(auto& m : impl.m_methods)
                {
                    m.second.data.m_save_code = true;
                }
            }
        }
    };
    for(auto& impl_grp : crate.m_type_impls.named)
    {
        for(auto& impl : impl_grp.second)
        {
            H1::enumerate_type_impl(state, *impl);
        }
    }
    for(auto& impl : crate.m_type_impls.non_named)
    {
        H1::enumerate_type_impl(state, *impl);
    }
    for(auto& impl : crate.m_type_impls.generic)
    {
        H1::enumerate_type_impl(state, *impl);
    }

    auto rv = Trans_Enumerate_CommonPost(state);

    // Strip out any functions/types/statics that are still generic?
    for(auto it = rv.m_functions.begin(); it != rv.m_functions.end(); )
    {
        if( monomorphise_path_needed(it->first) ) {
            rv.m_functions.erase(it++);
        }
        else {
            ++ it;
        }
    }
    for(auto it = rv.m_statics.begin(); it != rv.m_statics.end(); )
    {
        if( monomorphise_path_needed(it->first) ) {
            rv.m_statics.erase(it++);
        }
        else {
            ++ it;
        }
    }
    return rv;
}

void Trans_Enumerate_Cleanup(const ::HIR::Crate& crate, TransList& list)
{
    // NOTE: Disabled, as full filtering is nigh-on impossible
    // - Could do partial filtering of unused locally generated versions of trait impls and inlines
#if 0
    EnumState   state { crate };


    // Visit every function used and determine the items it uses
    // - This doesn't consider the function itself as used, so unused functions will be pruned
    Trans_Params    tp;
    for(const auto& ent : list.m_functions)
    {
        const MIR::Function* mir;
        if( ent.second->monomorphised.code )
        {
            mir = &*ent.second->monomorphised.code;
        }
        else if( ent.second->ptr->m_code.m_mir )
        {
            mir = &*ent.second->ptr->m_code.m_mir;
        }
        else
        {
            continue;
        }

        MIR::EnumCache  ec;
        Trans_Enumerate_FillFrom_MIR(ec, *mir);
        ec.apply(state, tp);
    }
    for(const auto& ent : list.m_statics)
    {
        const auto& pp = ent.second->pp;
        const auto& item = *ent.second->ptr;
        if( ! item.m_value_res.is_Invalid() )
        {
            Trans_Enumerate_FillFrom_Literal(state, item.m_value_res, pp);
        }
    }

    // Remove any item in `list.m_functions` that doesn't appear in `state.rv.m_functions`
    for(auto it = list.m_functions.begin(); it != list.m_functions.end();)
    {
        auto it2 = state.rv.m_functions.find(it->first);
        // TODO: Only remove if it is not exported
        // - externally reachable
        // - used by exported generic
        // - part of a local trait impl
        if( it2 == state.rv.m_functions.end() && false )
        {
            DEBUG("Remove " << it->first);
            it = list.m_functions.erase(it);
        }
        else
        {
            DEBUG("Keep " << it->first);
            ++ it;
        }
    }

    // Sanity check: all items in `state.rv.m_functions` must exist in `list.m_functions`
    for(const auto& e : state.rv.m_functions)
    {
        auto it = list.m_functions.find(e.first);
        ASSERT_BUG(Span(), it != list.m_functions.end(), "Enumerate Error - New function appeared after monomorphisation - " << e.first);
    }
#endif
}

/// Common post-processing
void Trans_Enumerate_CommonPost_Run(EnumState& state)
{
    // Run the enumerate queue (keeps the recursion depth down)
    while( !state.fcn_queue.empty() )
    {
        auto& fcn_out = *state.fcn_queue.front();
        state.fcn_queue.pop_front();

        TRACE_FUNCTION_F("Function " << ::std::find_if(state.rv.m_functions.begin(), state.rv.m_functions.end(), [&](const auto&x){ return x.second.get() == &fcn_out; })->first);

        Trans_Enumerate_FillFrom(state, *fcn_out.ptr, fcn_out.pp);
    }
}
TransList Trans_Enumerate_CommonPost(EnumState& state)
{
    Trans_Enumerate_CommonPost_Run(state);
    Trans_Enumerate_Types(state);

    return mv$(state.rv);
}

namespace
{
    struct PtrComp
    {
        template<typename T>
        bool operator()(const T* lhs, const T* rhs) const { return *lhs < *rhs; }
    };

    struct TypeVisitor
    {
        const ::HIR::Crate& m_crate;
        ::StaticTraitResolve    m_resolve;
        ::std::vector< ::std::pair< ::HIR::TypeRef, bool> >& out_list;

        // TODO: Have a list of indexes into `out_list`, sorted by typeref ordering
        ::std::vector<size_t>   visited_map;
        ::std::set< const ::HIR::TypeRef*, PtrComp> active_set;

        TypeVisitor(const ::HIR::Crate& crate, ::std::vector< ::std::pair< ::HIR::TypeRef, bool > >& out_list):
            m_crate(crate),
            m_resolve(crate),
            out_list(out_list)
        {}

        ~TypeVisitor()
        {
            DEBUG("Visited a total of " << visited_map.size());
        }

        void visit_struct(const ::HIR::GenericPath& path, const ::HIR::Struct& item) {
            static Span sp;
            ::HIR::TypeRef  tmp;
            MonomorphStatePtr   ms(nullptr, &path.m_params, nullptr);
            auto monomorph = [&](const auto& x)->const auto& { return m_resolve.monomorph_expand_opt(sp, tmp, x, ms); };
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
            static Span sp;
            ::HIR::TypeRef  tmp;
            MonomorphStatePtr   ms(nullptr, &path.m_params, nullptr);
            auto monomorph = [&](const auto& x)->const auto& { return m_resolve.monomorph_expand_opt(sp, tmp, x, ms); };
            for(const auto& variant : item.m_variants)
            {
                visit_type( monomorph(variant.second.ent) );
            }
        }
        void visit_enum(const ::HIR::GenericPath& path, const ::HIR::Enum& item) {
            static Span sp;
            ::HIR::TypeRef  tmp;
            MonomorphStatePtr   ms(nullptr, &path.m_params, nullptr);
            auto monomorph = [&](const auto& x)->const auto& { return m_resolve.monomorph_expand_opt(sp, tmp, x, ms); };
            if( const auto* e = item.m_data.opt_Data() )
            {
                for(const auto& variant : *e)
                {
                    visit_type( monomorph(variant.type) );
                }
            }
        }

        enum class Mode {
            Shallow,
            Normal,
            Deep,
        };

        void visit_type(const ::HIR::TypeRef& ty, Mode mode = Mode::Normal)
        {
            Span    sp;
            // If the type has already been visited, AND either this is a shallow visit, or the previous wasn't
            {
                auto idx_it = ::std::lower_bound(visited_map.begin(), visited_map.end(), ty, [&](size_t i, const ::HIR::TypeRef& t){ return out_list[i].first < t; });
                if( idx_it != visited_map.end() && out_list[*idx_it].first == ty )
                {
                    auto it = &out_list[*idx_it];
                    if( it->second == false || mode == Mode::Shallow )
                    {
                        // Return early
                        return ;
                    }
                    DEBUG("-- " << ty << " already visited as shallow");
                }
            }
            TRACE_FUNCTION_F(ty << " - " << (mode == Mode::Shallow ? "Shallow" : (mode == Mode::Normal ? "Normal" : "Deep")));

            if( mode == Mode::Shallow )
            {
                TU_MATCH_HDRA( (ty.data()), {)
                default:
                    break;
                TU_ARMA(Infer, te) {
                    BUG(sp, "`_` type hit in enumeration");
                    }
                TU_ARMA(Path, te) {
                    TU_MATCHA( (te.binding), (tpb),
                    (Unbound,
                        BUG(sp, "Unbound type hit in enumeration - " << ty);
                        ),
                    (Opaque,
                        BUG(sp, "Opaque type hit in enumeration - " << ty);
                        ),
                    (ExternType,
                        ),
                    (Struct,
                        ),
                    (Union,
                        ),
                    (Enum,
                        )
                    )
                    }
                TU_ARMA(Array, te) {
                    ASSERT_BUG(sp, te.size.is_Known(), "Encountered unknown array size - " << ty);
                    }
                TU_ARMA(Function, te) {
                    visit_type(te.m_rettype, Mode::Shallow);
                    for(const auto& sty : te.m_arg_types)
                        visit_type(sty, Mode::Shallow);
                    }
                TU_ARMA(Pointer, te) {
                    visit_type(te.inner, Mode::Shallow);
                    }
                TU_ARMA(Borrow, te) {
                    visit_type(te.inner, Mode::Shallow);
                    }
                }
            }
            else
            {
                if( active_set.find(&ty) != active_set.end() ) {
                    // TODO: Handle recursion
                    BUG(sp, "- Type recursion on " << ty);
                }
                active_set.insert( &ty );

                TU_MATCH_HDRA( (ty.data()), {)
                // Impossible
                TU_ARMA(Infer, te) {
                    BUG(sp, "`_` type hit in enumeration");
                    }
                TU_ARMA(Generic, te) {
                    BUG(sp, "Generic type hit in enumeration - " << ty);
                    }
                TU_ARMA(ErasedType, te) {
                    //BUG(sp, "ErasedType hit in enumeration - " << ty);
                    }
                TU_ARMA(Closure, te) {
                    BUG(sp, "Closure type hit in enumeration - " << ty);
                    }
                TU_ARMA(Generator, te) {
                    BUG(sp, "Generator type hit in enumeration - " << ty);
                    }
                // Nothing to do
                TU_ARMA(Diverge, te) {
                    }
                TU_ARMA(Primitive, te) {
                    }
                // Recursion!
                TU_ARMA(Path, te) {
                    TU_MATCHA( (te.binding), (tpb),
                    (Unbound,
                        BUG(sp, "Unbound type hit in enumeration - " << ty);
                        ),
                    (Opaque,
                        BUG(sp, "Opaque type hit in enumeration - " << ty);
                        ),
                    (ExternType,
                        // No innards to visit
                        ),
                    (Struct,
                        visit_struct(te.path.m_data.as_Generic(), *tpb);
                        ),
                    (Union,
                        visit_union(te.path.m_data.as_Generic(), *tpb);
                        ),
                    (Enum,
                        // NOTE: Force repr generation before recursing into enums (allows layout optimisation to be calculated)
                        Target_GetTypeRepr(sp, m_resolve, ty);
                        visit_enum(te.path.m_data.as_Generic(), *tpb);
                        )
                    )
                    }
                TU_ARMA(TraitObject, te) {
                    static Span sp;
                    // Ensure that the data trait's vtable is present
                    const auto& trait = *te.m_trait.m_trait_ptr;

                    ASSERT_BUG(sp, ! te.m_trait.m_path.m_path.m_components.empty(), "TODO: Data trait is empty, what can be done?");
                    const auto& vtable_ty_spath = trait.m_vtable_path;
                    const auto& vtable_ref = m_crate.get_struct_by_path(sp, vtable_ty_spath);
                    // Copy the param set from the trait in the trait object
                    ::HIR::PathParams   vtable_params = te.m_trait.m_path.m_params.clone();
                    // - Include associated types on bound
                    for(const auto& ty_b : te.m_trait.m_type_bounds) {
                        auto idx = trait.m_type_indexes.at(ty_b.first);
                        if(vtable_params.m_types.size() <= idx)
                            vtable_params.m_types.resize(idx+1);
                        vtable_params.m_types[idx] = ty_b.second.type.clone();
                    }

                    visit_type( ::HIR::TypeRef::new_path( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref ) );
                    }
                TU_ARMA(Array, te) {
                    ASSERT_BUG(sp, te.size.is_Known(), "Encountered unknown array size - " << ty);
                    visit_type(te.inner, mode);
                    }
                TU_ARMA(Slice, te) {
                    visit_type(te.inner, mode);
                    }
                TU_ARMA(Borrow, te) {
                    visit_type(te.inner, mode != Mode::Deep ? Mode::Shallow : Mode::Deep);
                    }
                TU_ARMA(Pointer, te) {
                    visit_type(te.inner, mode != Mode::Deep ? Mode::Shallow : Mode::Deep);
                    }
                TU_ARMA(Tuple, te) {
                    for(const auto& sty : te)
                        visit_type(sty, mode);
                    }
                TU_ARMA(Function, te) {
                    visit_type(te.m_rettype, mode != Mode::Deep ? Mode::Shallow : Mode::Deep);
                    for(const auto& sty : te.m_arg_types)
                        visit_type(sty, mode != Mode::Deep ? Mode::Shallow : Mode::Deep);
                    }
                }
                active_set.erase( active_set.find(&ty) );
            }

            bool shallow = (mode == Mode::Shallow);
            {
                auto idx_it = ::std::lower_bound(visited_map.begin(), visited_map.end(), ty, [&](size_t i, const ::HIR::TypeRef& t){ return out_list[i].first < t; });
                if( idx_it == visited_map.end() || out_list[*idx_it].first != ty )
                {
                    // Add a new entry
                    visited_map.insert(idx_it, out_list.size());
                }
                else
                {
                    // Previous visit was shallow, but this one isn't
                    // - Update the entry to the to-be-pushed entry with shallow=false
                    if( !shallow && out_list[*idx_it].second )
                    {
                        *idx_it = out_list.size();
                    }
                }
            }
            auto i = out_list.size();
            out_list.push_back( ::std::make_pair(ty.clone(), shallow) );
            DEBUG("Add type " << ty << (shallow ? " (Shallow)": "") << " " << i);
        }

        void __attribute__ ((noinline)) visit_function(const ::HIR::Path& path, const ::HIR::Function& fcn, const Trans_Params& pp)
        {
            Span    sp;
            auto& tv = *this;

            ::HIR::TypeRef   tmp;
            auto monomorph = [&](const auto& ty)->const auto& {
                return pp.maybe_monomorph(m_resolve, tmp, ty);
                };
            bool has_erased = visit_ty_with(fcn.m_return, [&](const auto& x) { return x.data().is_ErasedType(); });
            // Handle erased types in the return type.
            if( has_erased || monomorphise_type_needed(fcn.m_return) )
            {
                // If there's an erased type, make a copy with the erased type expanded
                ::HIR::TypeRef  ret_ty;
                if( has_erased )
                {
                    ret_ty = clone_ty_with(sp, fcn.m_return, [&](const auto& x, auto& out) {
                        if( const auto* te = x.data().opt_ErasedType() ) {
                            out = fcn.m_code.m_erased_types.at(te->m_index).clone();
                            return true;
                        }
                        else {
                            return false;
                        }
                        });
                    ret_ty = pp.monomorph(tv.m_resolve, ret_ty);
                }
                else
                {
                    ret_ty = pp.monomorph(tv.m_resolve, fcn.m_return);
                }
                tv.visit_type(ret_ty);
            }
            else
            {
                tv.visit_type( fcn.m_return );
            }
            for(const auto& arg : fcn.m_args)
                tv.visit_type( monomorph(arg.second) );

            if( fcn.m_code.m_mir )
            {
                const auto& mir = *fcn.m_code.m_mir;
                for(const auto& ty : mir.locals)
                    tv.visit_type(monomorph(ty));

                // Find all LValue::Deref instances and get the result type
                ::MIR::TypeResolve::args_t    empty_args;
                ::HIR::TypeRef    empty_ty;
                ::MIR::TypeResolve  mir_res(sp, tv.m_resolve, FMT_CB(fcn_path), /*ret_ty=*/empty_ty, empty_args, mir);
                for(const auto& block : mir.blocks)
                {
                    struct MirVisitor
                        :public ::MIR::visit::Visitor
                    {
                        const Span& sp;
                        TypeVisitor&    tv;
                        const Trans_Params& pp;
                        const ::HIR::Function&  fcn;
                        const ::MIR::TypeResolve& mir_res;

                        MirVisitor(const Span& sp, TypeVisitor& tv, const Trans_Params& pp, const ::HIR::Function& fcn, const ::MIR::TypeResolve& mir_res)
                            :sp(sp)
                            ,tv(tv)
                            ,pp(pp)
                            ,fcn(fcn)
                            ,mir_res(mir_res)
                        {
                        }

                        bool visit_lvalue(const ::MIR::LValue& lv, MIR::visit::ValUsage /*vu*/) override
                        {
                            TRACE_FUNCTION_F(lv);
                            if( ::std::none_of(lv.m_wrappers.begin(), lv.m_wrappers.end(), [](const auto& w){ return w.is_Deref(); }) )
                            {
                                return false;
                            }
                            ::HIR::TypeRef  tmp;
                            auto monomorph_outer = [&](const auto& tpl)->const auto& {
                                return pp.maybe_monomorph(tv.m_resolve, tmp, tpl);
                                };
                            const ::HIR::TypeRef*   ty_p = nullptr;;
                            // Recurse, if Deref get the type and add it to the visitor
                            TU_MATCHA( (lv.m_root), (e),
                            (Return,
                                MIR_TODO(mir_res, "Get return type for MIR type enumeration");
                                ),
                            (Argument,
                                ty_p = &monomorph_outer(fcn.m_args[e].second);
                                ),
                            (Local,
                                ty_p = &monomorph_outer(fcn.m_code.m_mir->locals[e]);
                                ),
                            (Static,
                                // TODO: Monomorphise the path then hand to MIR::TypeResolve?
                                const auto& path = e;
                                TU_MATCHA( (path.m_data), (pe),
                                (Generic,
                                    MIR_ASSERT(mir_res, pe.m_params.m_types.empty(), "Path params on static - " << path);
                                    const auto& s = tv.m_resolve.m_crate.get_static_by_path(mir_res.sp, pe.m_path);
                                    ty_p = &s.m_type;
                                    ),
                                (UfcsKnown,
                                    MIR_TODO(mir_res, "LValue::Static - UfcsKnown - " << path);
                                    ),
                                (UfcsUnknown,
                                    MIR_BUG(mir_res, "Encountered UfcsUnknown in LValue::Static - " << path);
                                    ),
                                (UfcsInherent,
                                    MIR_TODO(mir_res, "LValue::Static - UfcsInherent - " << path);
                                    )
                                )
                                )
                            )
                            assert(ty_p);
                            for(const auto& w : lv.m_wrappers)
                            {
                                ty_p = &mir_res.get_unwrapped_type(tmp, w, *ty_p);
                                if( w.is_Deref() )
                                {
                                   tv.visit_type(*ty_p);
                                }
                            }
                            return false;
                        }

                        void visit_path(const HIR::Path& /*p*/) override
                        {
                            // Paths don't need visiting?
                        }
                        void visit_type(const HIR::TypeRef& ty) override
                        {
                            HIR::TypeRef    tmp;
                            tv.visit_type(pp.maybe_monomorph(tv.m_resolve, tmp, ty));
                        }
                    };
                    MirVisitor  mir_visit(sp, tv, pp, fcn, mir_res);
                    for(const auto& stmt : block.statements)
                    {
                        mir_visit.visit_stmt(stmt);
                    }
                    mir_visit.visit_terminator(block.terminator);

                    // HACK: Currently calling `caller_location` creates an empty location (so needs the type)
                    if( block.terminator.is_Call() && block.terminator.as_Call().fcn.is_Intrinsic() ) {
                        const auto& e2 = block.terminator.as_Call().fcn.as_Intrinsic();
                        if( e2.name == "caller_location" ) {
                            const auto& p = mir_res.m_resolve.m_crate.get_lang_item_path(sp, "panic_location");
                            const auto& s = mir_res.m_resolve.m_crate.get_struct_by_path(sp, p);
                            tv.visit_type(HIR::TypeRef::new_path(p, &s));
                        }

                    }
                }
            }
        }
    };  // struct TypeVisitor
} // namespace <empty>

// Enumerate types required for the enumerated items
void Trans_Enumerate_Types(EnumState& state)
{
    TRACE_FUNCTION;
    static Span sp;
    TypeVisitor tv { state.crate, state.rv.m_types };

    unsigned int types_count = 0;
    bool constructors_added;
    do
    {
        // Visit all functions that haven't been type-visited yet
        for(unsigned int i = 0; i < state.fcns_to_type_visit.size(); i++)
        {
            auto* p = state.fcns_to_type_visit[i];
            assert(p->path);
            assert(p->ptr);
            auto& fcn_path = *p->path;
            const auto& fcn = *p->ptr;
            const auto& pp = p->pp;

            TRACE_FUNCTION_F("Function " << fcn_path);
            tv.visit_function(fcn_path, fcn, pp);
        }
        state.fcns_to_type_visit.clear();
        // TODO: Similarly restrict revisiting of statics.
        // - Challenging, as they're stored as a std::map
        for(const auto& ent : state.rv.m_statics)
        {
            TRACE_FUNCTION_F("Enumerate static " << ent.first);
            assert(ent.second->ptr);
            const auto& stat = *ent.second->ptr;
            const auto& pp = ent.second->pp;

            tv.visit_type( pp.monomorph(tv.m_resolve, stat.m_type) );
        }
        // - Constants need visiting, as they will be expanded
        for(const auto& ent : state.rv.m_constants)
        {
            TRACE_FUNCTION_F("Enumerate constant " << ent.first);
            assert(ent.second->ptr);
            const auto& stat = *ent.second->ptr;
            const auto& pp = ent.second->pp;

            tv.visit_type( pp.monomorph(tv.m_resolve, stat.m_type) );
        }
        for(const auto& ent : state.rv.m_vtables)
        {
            TRACE_FUNCTION_F("vtable " << ent.first);
            const auto& gpath = ent.first.m_data.as_UfcsKnown().trait;
            const auto& trait = state.crate.get_trait_by_path(sp, gpath.m_path);

            const auto& vtable_ty_spath = trait.m_vtable_path;
            const auto& vtable_ref = state.crate.get_struct_by_path(sp, vtable_ty_spath);
            // Copy the param set from the trait in the trait object
            ::HIR::PathParams   vtable_params = gpath.m_params.clone();
            // - Include associated types on bound
            for(const auto& ty_idx : trait.m_type_indexes)
            {
                auto idx = ty_idx.second;
                if(vtable_params.m_types.size() <= idx)
                    vtable_params.m_types.resize(idx+1);
                auto p = ent.first.clone();
                p.m_data.as_UfcsKnown().item = ty_idx.first;
                vtable_params.m_types[idx] = ::HIR::TypeRef::new_path( mv$(p), {} );
                tv.m_resolve.expand_associated_types( sp, vtable_params.m_types[idx] );
            }
            DEBUG("VTable: " << vtable_ty_spath << vtable_params);

            const auto& ty = ent.first.m_data.as_UfcsKnown().type;
            tv.visit_type( ty );
            tv.visit_type( ::HIR::TypeRef::new_path( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref ) );

            // If this is for a function pointer, visit all arguments
            // - `auto_impls.cpp` will generate a vtable shim for it (which requires argument types to be fully known)
            // NOTE: Assumes that the trait is one of the Fn* traits (doesn't matter if it isn't here)
            if(const auto* te = ty.data().opt_Function())
            {
                for(const auto& t : te->m_arg_types)
                    tv.visit_type(t);
                tv.visit_type(te->m_rettype);

                if( gpath.m_params.m_types.size() >= 1 )
                {
                    tv.visit_type(gpath.m_params.m_types[0]);
                }
            }
        }

        constructors_added = false;
        for(unsigned int i = types_count; i < state.rv.m_types.size(); i ++ )
        {
            const auto& ent = state.rv.m_types[i];
            // Shallow? Skip.
            if( ent.second )
                continue ;
            const auto& ty = ent.first;
            if( ty.data().is_Path() )
            {
                const auto& te = ty.data().as_Path();
                ASSERT_BUG(sp, te.path.m_data.is_Generic(), "Non-Generic type path after enumeration - " << ty);
                const auto& gp = te.path.m_data.as_Generic();
                const ::HIR::TraitMarkings* markings_ptr = te.binding.get_trait_markings();
                ASSERT_BUG(sp, markings_ptr, "Path binding not set correctly - " << ty);

                // If the type has a drop impl, and it's either defined in this crate or has params (and thus was monomorphised)
                if( markings_ptr->has_drop_impl && (gp.m_path.m_crate_name == state.crate.m_crate_name || gp.m_params.has_params()) )
                {
                    // Add the Drop impl to the codegen list
                    Trans_Enumerate_FillFrom_PathMono(state,  ::HIR::Path( ty.clone(), state.crate.get_lang_item_path(sp, "drop"), "drop"));
                    constructors_added = true;
                }
            }

            if( const auto* ity = tv.m_resolve.is_type_owned_box(ty) )
            {
                // Reqire drop glue for inner type.
                // - Should that already exist?
                // Requires box_free lang item
                const auto& p = ty.data().as_Path().path.m_data.as_Generic().m_params;
                Trans_Enumerate_FillFrom_PathMono(state, ::HIR::GenericPath( state.crate.get_lang_item_path(sp, "box_free"), p.clone() ));
            }
        }
        types_count = state.rv.m_types.size();

        // Run queue
        Trans_Enumerate_CommonPost_Run(state);
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
    EntPtr get_ent_fullpath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::Path& path, ::HIR::PathParams& impl_pp, const ::HIR::GenericParams*& impl_def)
    {
        TRACE_FUNCTION_F(path);
        StaticTraitResolve  resolve { crate };

        MonomorphState  ms;
        impl_def = nullptr;
        auto ent = resolve.get_value(sp, path, ms, /*signature_only=*/false, &impl_def);
        if(ms.get_impl_params()) {
            impl_pp = ms.get_impl_params()->clone();
            if(impl_pp.has_params())
                assert(impl_def);
        }
        DEBUG(path << " = " << ent.tag_str() << " w/ impl" << impl_pp);
        TU_MATCH_HDRA( (ent), {)
        default:
            TODO(sp, path << " was " << ent.tag_str());
        TU_ARMA(NotYetKnown, _e) {
            const auto* pe = &path.m_data.as_UfcsKnown();
            // Options:
            // - VTable
            if( pe->item == "vtable#" ) {
                DEBUG("VTable, quick return");
                return EntPtr::make_AutoGenerate( {} );
            }
            // - Auto-generated impl (the only trait impl was a bound)
            //  > Need to check if the trait is impled bounded
            bool found_bound = false;
            bool found_impl = false;
            resolve.find_impl(sp, pe->trait.m_path, pe->trait.m_params, pe->type, [&](auto impl_ref, auto is_fuzz)->bool {
                DEBUG("[get_ent_fullpath] Found " << impl_ref);
                if(impl_ref.m_data.is_TraitImpl()) {
                    found_impl = true;
                }
                else {
                    found_bound = true;
                }
                return false;
                });
            if(found_bound) {
                return EntPtr::make_AutoGenerate( {} );
            }
            DEBUG("NotYetKnown -> NotFound");
            return EntPtr();
            }
        TU_ARMA(Function, f) {
            // Check for trait provided bodies
            // - They need a little hack to ensure that monomorph is run
            if( const auto* pe = path.m_data.opt_UfcsKnown() )
            {
                const auto& trait_ref = crate.get_trait_by_path(sp, pe->trait.m_path);
                const auto& trait_vi = trait_ref.m_values.at(pe->item);

                if( f == &trait_vi.as_Function() )
                {
                    DEBUG("Default trait body");
                    // HACK! By adding a new parameter here, the MIR will always be monomorphised
                    impl_pp.m_types.push_back( ::HIR::TypeRef() );
                }
            }
            return EntPtr { f };
            }
        TU_ARMA(Static, f) {
            return EntPtr { f };
            }
        TU_ARMA(Constant, f) {
            return EntPtr { f };
            }
        TU_ARMA(StructConstructor, _) {
            return EntPtr::make_AutoGenerate({});
            }
        TU_ARMA(EnumConstructor, _) {
            return EntPtr::make_AutoGenerate({});
            }
        }
        throw "";
    }
}


void Trans_Enumerate_FillFrom_Path(EnumState& state, const ::HIR::Path& path, const Trans_Params& pp)
{
    auto path_mono = pp.monomorph(state.crate, path);
    Trans_Enumerate_FillFrom_PathMono(state, mv$(path_mono));
}
void Trans_Enumerate_FillFrom_PathMono(EnumState& state, ::HIR::Path path_mono)
{
    TRACE_FUNCTION_F(path_mono);
    Span    sp;
    // TODO: If already in the list, return early
    if( state.rv.m_functions.count(path_mono) ) {
        DEBUG("> Already done function");
        return ;
    }
    if( state.rv.m_statics.count(path_mono) ) {
        DEBUG("> Already done static");
        return ;
    }
    if( state.rv.m_constants.count(path_mono) ) {
        DEBUG("> Already done constant");
        return ;
    }
    if( state.rv.m_vtables.count(path_mono) ) {
        DEBUG("> Already done vtable");
        return ;
    }

    Trans_Params  sub_pp(sp);
    TU_MATCH_HDRA( (path_mono.m_data), { )
    TU_ARMA(Generic, pe) {
        sub_pp.pp_method = pe.m_params.clone();
        }
    TU_ARMA(UfcsKnown, pe) {
        sub_pp.pp_method = pe.params.clone();
        sub_pp.self_type = pe.type.clone();
        }
    TU_ARMA(UfcsInherent, pe) {
        sub_pp.pp_method = pe.params.clone();
        sub_pp.pp_impl = pe.impl_params.clone();
        sub_pp.self_type = pe.type.clone();
        }
    TU_ARMA(UfcsUnknown, pe) {
        BUG(sp, "UfcsUnknown - " << path_mono);
        }
    }
    // Get the item type
    // - Valid types are Function and Static
    auto item_ref = get_ent_fullpath(sp, state.crate, path_mono, sub_pp.pp_impl, sub_pp.gdef_impl);
    DEBUG("sub_pp.pp_method = " << sub_pp.pp_method);
    DEBUG("sub_pp.pp_impl = " << sub_pp.pp_impl);
    TU_MATCH_HDRA( (item_ref), {)
    TU_ARMA(NotFound, e) {
        BUG(sp, "Item not found for " << path_mono);
        }
    TU_ARMA(AutoGenerate, e) {
        if( path_mono.m_data.is_Generic() )
        {
            // Leave generation of struct/enum constructors to codgen
            // TODO: Add to a list of required constructors
            state.rv.m_constructors.insert( mv$(path_mono.m_data.as_Generic()) );
        }
        // - <T as U>::#vtable
        else if( path_mono.m_data.is_UfcsKnown() && path_mono.m_data.as_UfcsKnown().item == "vtable#" )
        {
            if( state.rv.add_vtable( path_mono.clone(), {} ) )
            {
                // Fill from the vtable
                Trans_Enumerate_FillFrom_VTable(state, mv$(path_mono), sub_pp);
            }
        }
        // - <(Trait) as Trait>::method
        else if( path_mono.m_data.is_UfcsKnown() && path_mono.m_data.as_UfcsKnown().type.data().is_TraitObject() )
        {
            state.rv.trait_object_methods.insert( mv$(path_mono) );
        }
        // - <fn(...) as Fn*>::call*
        else if( path_mono.m_data.is_UfcsKnown() && path_mono.m_data.as_UfcsKnown().type.data().is_Function() && (
               path_mono.m_data.as_UfcsKnown().trait.m_path == state.crate.get_lang_item_path_opt("fn")
            || path_mono.m_data.as_UfcsKnown().trait.m_path == state.crate.get_lang_item_path_opt("fn_mut")
            || path_mono.m_data.as_UfcsKnown().trait.m_path == state.crate.get_lang_item_path_opt("fn_once")
            ) )
        {
            // Must have been a dynamic dispatch request, just leave as-is
            // - However, ensure that all arguments are visited?
            //const auto& fcn_ty = path_mono.m_data.as_UfcsKnown().type.data().as_Function();
            //for(const auto& ty : fcn_ty.m_arg_types)
            //    state.rv.vi
        }
        // <* as Clone>::clone
        else if( TARGETVER_LEAST_1_29 && path_mono.m_data.is_UfcsKnown() && path_mono.m_data.as_UfcsKnown().trait == state.crate.get_lang_item_path_opt("clone") )
        {
            const auto& pe = path_mono.m_data.as_UfcsKnown();
            ASSERT_BUG(sp, pe.item == "clone" || pe.item == "clone_from", "Unexpected Clone method called, " << path_mono);
            const auto& inner_ty = pe.type;
            // If this is !Copy, then we need to ensure that the inner type's clone impls are also available
            ::StaticTraitResolve    resolve { state.crate };
            if( !resolve.type_is_copy(sp, inner_ty) )
            {
                auto enum_impl = [&](const ::HIR::TypeRef& ity) {
                    if( !resolve.type_is_copy(sp, ity) )
                    {
                        Trans_Enumerate_FillFrom_PathMono(state, ::HIR::Path(ity.clone(), pe.trait.clone(), pe.item));
                    }
                    };
                if( const auto* te = inner_ty.data().opt_Tuple() ) {
                    for(const auto& ity : *te)
                    {
                        enum_impl(ity);
                    }
                }
                else if( const auto* te = inner_ty.data().opt_Array() ) {
                    enum_impl(te->inner);
                }
                else if( TU_TEST1(inner_ty.data(), Path, .is_closure()) ) {
                    const auto& gp = inner_ty.data().as_Path().path.m_data.as_Generic();
                    const auto& str = state.crate.get_struct_by_path(sp, gp.m_path);
                    auto p = Trans_Params::new_impl(sp, {}, gp.m_params.clone());
                    for(const auto& fld : str.m_data.as_Tuple())
                    {
                        ::HIR::TypeRef  tmp;
                        const auto& ty_m = monomorphise_type_needed(fld.ent) ? (tmp = p.monomorph(resolve, fld.ent)) : fld.ent;
                        enum_impl(ty_m);
                    }
                }
                else {
                    BUG(sp, "Unhandled magic clone in enumerate - " << inner_ty);
                }
            }
            // Add this type to a list of types that will have the impl auto-generated
            state.rv.auto_clone_impls.insert( inner_ty.clone() );
        }
        else
        {
            BUG(sp, "AutoGenerate returned for unknown path type - " << path_mono);
        }
        }
    TU_ARMA(Function, e) {
        // Add this path (monomorphised) to the queue
        state.enum_fcn(mv$(path_mono), *e, mv$(sub_pp));
        }
    TU_ARMA(Static, e) {
        if( auto* ptr = state.rv.add_static(mv$(path_mono)) )
        {
            Trans_Enumerate_FillFrom(state, *e, *ptr, mv$(sub_pp));
        }
        }
    TU_ARMA(Constant, e) {
        switch(e->m_value_state)
        {
        case HIR::Constant::ValueState::Unknown:
            BUG(sp, "Unevaluated constant: " << path_mono);
        case HIR::Constant::ValueState::Generic:
            if( auto* slot = state.rv.add_const(mv$(path_mono)) )
            {
                MIR::EnumCache  es;
                Trans_Enumerate_FillFrom_MIR(es, *e->m_value.m_mir);
                es.apply(state, sub_pp);
                slot->ptr = e;
                slot->pp = ::std::move(sub_pp);
            }
            break;
        case HIR::Constant::ValueState::Known:
            Trans_Enumerate_FillFrom_Literal(state, e->m_value_res, sub_pp);
            break;
        }
        }
    }
}

void Trans_Enumerate_FillFrom_MIR_LValue(MIR::EnumCache& state, const ::MIR::LValue& lv)
{
    if( lv.m_root.is_Static() )
    {
        state.insert_path(lv.m_root.as_Static());
    }
}
void Trans_Enumerate_FillFrom_MIR_Constant(MIR::EnumCache& state, const ::MIR::Constant& c)
{
    TU_MATCHA( (c), (ce),
    (Int, ),
    (Uint,),
    (Float, ),
    (Bool, ),
    (Bytes, ),
    (StaticString, ),  // String
    (Const,
        // - Check if this constant has a value of Defer
        state.insert_path(*ce.p);
        ),
    (Generic,
        ),
    (ItemAddr,
        if(ce)
            state.insert_path(*ce);
        )
    )
}
void Trans_Enumerate_FillFrom_MIR_Param(MIR::EnumCache& state, const ::MIR::Param& p)
{
    TU_MATCHA( (p), (e),
    (LValue, Trans_Enumerate_FillFrom_MIR_LValue(state, e); ),
    (Borrow, Trans_Enumerate_FillFrom_MIR_LValue(state, e.val); ),
    (Constant, Trans_Enumerate_FillFrom_MIR_Constant(state, e); )
    )
}
void Trans_Enumerate_FillFrom_MIR(MIR::EnumCache& state, const ::MIR::Function& code)
{
    TRACE_FUNCTION_F("");
    for(const auto& bb : code.blocks)
    {
        for(const auto& stmt : bb.statements)
        {
            TU_MATCH_HDRA((stmt), {)
            TU_ARMA(Assign, se) {
                DEBUG("- " << se.dst << " = " << se.src);
                Trans_Enumerate_FillFrom_MIR_LValue(state, se.dst);
                TU_MATCHA( (se.src), (e),
                (Use,
                    Trans_Enumerate_FillFrom_MIR_LValue(state, e);
                    ),
                (Constant,
                    Trans_Enumerate_FillFrom_MIR_Constant(state, e);
                    ),
                (SizedArray,
                    Trans_Enumerate_FillFrom_MIR_Param(state, e.val);
                    ),
                (Borrow,
                    Trans_Enumerate_FillFrom_MIR_LValue(state, e.val);
                    ),
                (Cast,
                    Trans_Enumerate_FillFrom_MIR_LValue(state, e.val);
                    ),
                (BinOp,
                    Trans_Enumerate_FillFrom_MIR_Param(state, e.val_l);
                    Trans_Enumerate_FillFrom_MIR_Param(state, e.val_r);
                    ),
                (UniOp,
                    Trans_Enumerate_FillFrom_MIR_LValue(state, e.val);
                    ),
                (DstMeta,
                    Trans_Enumerate_FillFrom_MIR_LValue(state, e.val);
                    ),
                (DstPtr,
                    Trans_Enumerate_FillFrom_MIR_LValue(state, e.val);
                    ),
                (MakeDst,
                    Trans_Enumerate_FillFrom_MIR_Param(state, e.ptr_val);
                    Trans_Enumerate_FillFrom_MIR_Param(state, e.meta_val);
                    ),
                (Tuple,
                    for(const auto& val : e.vals)
                        Trans_Enumerate_FillFrom_MIR_Param(state, val);
                    ),
                (Array,
                    for(const auto& val : e.vals)
                        Trans_Enumerate_FillFrom_MIR_Param(state, val);
                    ),
                (UnionVariant,
                    Trans_Enumerate_FillFrom_MIR_Param(state, e.val);
                    ),
                (EnumVariant,
                    for(const auto& val : e.vals)
                        Trans_Enumerate_FillFrom_MIR_Param(state, val);
                    ),
                (Struct,
                    for(const auto& val : e.vals)
                        Trans_Enumerate_FillFrom_MIR_Param(state, val);
                    )
                )
                }
            TU_ARMA(Asm2, e) {
                for(auto& p : e.params)
                {
                    TU_MATCH_HDRA( (p), { )
                    TU_ARMA(Const, v) Trans_Enumerate_FillFrom_MIR_Constant(state, v);
                    TU_ARMA(Sym, v) state.insert_path(v);
                    TU_ARMA(Reg, v) {
                        if(v.input)
                            Trans_Enumerate_FillFrom_MIR_Param(state, *v.input);
                        if(v.output)
                            Trans_Enumerate_FillFrom_MIR_LValue(state, *v.output);
                        }
                    }
                }
                }
            TU_ARMA(Asm, se) {
                DEBUG("- llvm_asm! ...");
                for(const auto& v : se.inputs)
                    Trans_Enumerate_FillFrom_MIR_LValue(state, v.second);
                for(const auto& v : se.outputs)
                    Trans_Enumerate_FillFrom_MIR_LValue(state, v.second);
                }
            TU_ARMA(SetDropFlag, se) {
                }
            TU_ARMA(ScopeEnd, se) {
                }
            TU_ARMA(Drop, se) {
                DEBUG("- DROP " << se.slot);
                Trans_Enumerate_FillFrom_MIR_LValue(state, se.slot);
                // TODO: Ensure that the drop glue for this type is generated
                }
            }
        }
        DEBUG("> " << bb.terminator);
        TU_MATCHA( (bb.terminator), (e),
        (Incomplete, ),
        (Return, ),
        (Diverge, ),
        (Goto, ),
        (Panic, ),
        (If,
            Trans_Enumerate_FillFrom_MIR_LValue(state, e.cond);
            ),
        (Switch,
            Trans_Enumerate_FillFrom_MIR_LValue(state, e.val);
            ),
        (SwitchValue,
            Trans_Enumerate_FillFrom_MIR_LValue(state, e.val);
            ),
        (Call,
            Trans_Enumerate_FillFrom_MIR_LValue(state, e.ret_val);
            TU_MATCHA( (e.fcn), (e2),
            (Value,
                Trans_Enumerate_FillFrom_MIR_LValue(state, e2);
                ),
            (Path,
                state.insert_path(e2);
                ),
            (Intrinsic,
                if( e2.name == "type_id" ) {
                    // Add <T>::#type_id to the enumerate list
                    state.insert_typeid(e2.params.m_types.at(0));
                }
                )
            )
            for(const auto& arg : e.args)
                Trans_Enumerate_FillFrom_MIR_Param(state, arg);
            )
        )
    }
}

void Trans_Enumerate_FillFrom_VTable(EnumState& state, ::HIR::Path vtable_path, const Trans_Params& pp)
{
    static Span sp;
    const auto& type = vtable_path.m_data.as_UfcsKnown().type;
    const auto& trait_path = vtable_path.m_data.as_UfcsKnown().trait;
    const auto& tr = state.crate.get_trait_by_path(Span(), trait_path.m_path);

    ASSERT_BUG(sp, !type.data().is_Slice(), "Getting vtable for unsized type - " << vtable_path);

    auto monomorph_cb_trait = MonomorphStatePtr(&type, &trait_path.m_params, nullptr);
    for(const auto& m : tr.m_value_indexes)
    {
        DEBUG("- " << m.second.first << " = " << m.second.second << " :: " << m.first);
        auto gpath = monomorph_cb_trait.monomorph_genericpath(sp, m.second.second, false);
        Trans_Enumerate_FillFrom_PathMono(state, ::HIR::Path(type.clone(), mv$(gpath), m.first));
    }
}

void Trans_Enumerate_FillFrom_Literal(EnumState& state, const EncodedLiteral& lit, const Trans_Params& pp)
{
    for(const auto& r : lit.relocations)
    {
        if( r.p ) {
            Trans_Enumerate_FillFrom_Path(state, *r.p, pp);
        }
    }
}

namespace {
    ::HIR::Function* find_function_by_link_name(const ::HIR::Module& mod, ::HIR::ItemPath mod_path,  const char* name,  ::HIR::SimplePath& out_path)
    {
        for(const auto& vi : mod.m_value_items)
        {
            TU_IFLET( ::HIR::ValueItem, vi.second->ent, Function, i,
                if( i.m_code.m_mir && i.m_linkage.name != "" && i.m_linkage.name == name )
                {
                    out_path = (mod_path + vi.first).get_simple_path();
                    return &i;
                }
            )
        }

        for(const auto& ti : mod.m_mod_items)
        {
            TU_IFLET( ::HIR::TypeItem, ti.second->ent, Module, i,
                if( auto rv = find_function_by_link_name(i, mod_path + ti.first, name,  out_path) )
                    return rv;
            )
        }

        return nullptr;
    }
    ::HIR::Function* find_function_by_link_name(const ::HIR::Crate& crate, const char* name,  ::HIR::SimplePath& out_path)
    {
        if(auto rv = find_function_by_link_name(crate.m_root_module, {crate.m_crate_name}, name, out_path))
            return rv;
        for(const auto& e_crate : crate.m_ext_crates)
        {
            if(auto rv = find_function_by_link_name(e_crate.second.m_data->m_root_module, {e_crate.first}, name,  out_path))
            {
                assert( out_path.m_crate_name == e_crate.first );
                return rv;
            }
        }
        return nullptr;
    }
}

void Trans_Enumerate_FillFrom(EnumState& state, const ::HIR::Function& function, const Trans_Params& pp)
{
    TRACE_FUNCTION_F("Function pp=" << pp.pp_impl << " + " << pp.pp_method);
    if( function.m_code.m_mir )
    {
        const auto& mir_fcn = *function.m_code.m_mir;
        if( !mir_fcn.trans_enum_state )
        {
            auto* esp = new MIR::EnumCache();
            Trans_Enumerate_FillFrom_MIR(*esp, *function.m_code.m_mir);
            mir_fcn.trans_enum_state = ::MIR::EnumCachePtr(esp);
        }
        // TODO: Ensure that all types have drop glue generated too? (Iirc this is unconditional currently)
        mir_fcn.trans_enum_state->apply(state, pp);
    }
    else
    {
        if( function.m_linkage.name != "" )
        {
            // Search for a function with the same linkage name anywhere in the loaded crates
            ::HIR::SimplePath   path;
            if(const auto* f = find_function_by_link_name(state.crate, function.m_linkage.name.c_str(), path))
            {
                state.enum_fcn( ::HIR::Path(mv$(path)), *f, Trans_Params(pp.sp) );
            }
        }
        // External.
    }
}
void Trans_Enumerate_FillFrom(EnumState& state, const ::HIR::Static& item, TransList_Static& out_stat, Trans_Params pp)
{
    TRACE_FUNCTION;
    /*if( item.m_value.m_mir )
    {
        Trans_Enumerate_FillFrom_MIR(state, *item.m_value.m_mir, pp);
    }
    else*/ if( item.m_type.data().is_Infer() )
    {
        BUG(Span(), "Enumerating static with no assigned type (unused elevated literal)");
    }
    else if( item.m_value_generated )
    {
        Trans_Enumerate_FillFrom_Literal(state, item.m_value_res, pp);
    }
    out_stat.ptr = &item;
    out_stat.pp = mv$(pp);
}
