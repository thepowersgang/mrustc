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
#include <deque>
#include <algorithm>

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
void Trans_Enumerate_FillFrom(EnumState& state, const ::HIR::Function& function, const Trans_Params& pp);
void Trans_Enumerate_FillFrom(EnumState& state, const ::HIR::Static& stat, TransList_Static& stat_out, Trans_Params pp={});
void Trans_Enumerate_FillFrom_VTable (EnumState& state, ::HIR::Path vtable_path, const Trans_Params& pp);
void Trans_Enumerate_FillFrom_Literal(EnumState& state, const ::HIR::Literal& lit, const Trans_Params& pp);
void Trans_Enumerate_FillFrom_MIR(EnumState& state, const ::MIR::Function& code, const Trans_Params& pp);

/// Enumerate trans items starting from `::main` (binary crate)
TransList Trans_Enumerate_Main(const ::HIR::Crate& crate)
{
    static Span sp;

    EnumState   state { crate };

    auto c_start_path = crate.get_lang_item_path_opt("mrustc-start");
    if( c_start_path == ::HIR::SimplePath() )
    {
        // "start" language item
        // - Takes main, and argc/argv as arguments
        {
            auto start_path = crate.get_lang_item_path(sp, "start");
            const auto& fcn = crate.get_function_by_path(sp, start_path);

            state.enum_fcn( start_path, fcn, {} );
        }

        // user entrypoint
        {
            auto main_path = crate.get_lang_item_path(Span(), "mrustc-main");
            const auto& fcn = crate.get_function_by_path(sp, main_path);

            state.enum_fcn( main_path, fcn, {} );
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
                if(e.m_type.m_data.is_Infer())
                    continue ;
                //state.enum_static(mod_path + vi.first, *e);
                auto* ptr = state.rv.add_static( get_path() );
                if(ptr)
                    Trans_Enumerate_FillFrom(state, e, *ptr);
            }
            } break;
        TU_ARM(vi, Function, e) {
            if( e.m_params.m_types.size() == 0 )
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
            Trans_Enumerate_ValItem(state, vi.second->ent, EMIT_ALL || (is_visible && vi.second->is_public), [&](){ return mod_path + vi.first; });
        }

        for(auto& ti : mod.m_mod_items)
        {
            if(auto* e = ti.second->ent.opt_Module() )
            {
                Trans_Enumerate_Public_Mod(state, *e, mod_path + ti.first, ti.second->is_public);
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
    for(auto& impl : crate.m_trait_impls)
    {
        const auto& impl_ty = impl.second.m_type;
        TRACE_FUNCTION_F("Impl " << impl.first << impl.second.m_trait_args << " for " << impl_ty);
        if( impl.second.m_params.m_types.size() == 0 )
        {
            auto cb_monomorph = monomorphise_type_get_cb(sp, &impl_ty, &impl.second.m_trait_args, nullptr);

            // Emit each method/static (in the trait itself)
            const auto& trait = crate.get_trait_by_path(sp, impl.first);
            for(const auto& vi : trait.m_values)
            {
                TRACE_FUNCTION_F("Item " << vi.first << " : " << vi.second.tag_str());
                // Constant, no codegen
                if( vi.second.is_Constant() )
                    ;
                // Generic method, no codegen
                else if( vi.second.is_Function() && vi.second.as_Function().m_params.m_types.size() > 0 )
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

                            auto b_ty_mono = monomorphise_type_with(sp, be.type, cb_monomorph); resolve.expand_associated_types(sp, b_ty_mono);
                            auto b_tp_mono = monomorphise_traitpath_with(sp, be.trait, cb_monomorph, false);
                            for(auto& ty : b_tp_mono.m_path.m_params.m_types) {
                                resolve.expand_associated_types(sp, ty);
                            }
                            for(auto& assoc_bound : b_tp_mono.m_type_bounds) {
                                resolve.expand_associated_types(sp, assoc_bound.second);
                            }

                            rv = resolve.find_impl(sp, b_tp_mono.m_path.m_path, b_tp_mono.m_path.m_params, b_ty_mono, [&](const auto& impl, bool) {
                                return true;
                                });
                            if( !rv )
                                break;
                        }
                        if( !rv )
                            continue ;
                    }
                    auto p = ::HIR::Path(impl_ty.clone(), ::HIR::GenericPath(impl.first, impl.second.m_trait_args.clone()), vi.first);
                    Trans_Enumerate_FillFrom_Path(state, p, {});
                }
            }
            for(auto& m : impl.second.m_methods)
            {
                if( m.second.data.m_params.m_types.size() > 0 )
                    m.second.data.m_save_code = true;
            }
        }
        else
        {
            for(auto& m : impl.second.m_methods)
            {
                m.second.data.m_save_code = true;
            }
        }
    }
    for(auto& impl : crate.m_type_impls)
    {
        if( impl.m_params.m_types.size() == 0 )
        {
            for(auto& fcn : impl.m_methods)
            {
                if( fcn.second.data.m_params.m_types.size() == 0 )
                {
                    auto p = ::HIR::Path(impl.m_type.clone(), fcn.first);
                    Trans_Enumerate_FillFrom_Path(state, p, {});
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

    auto rv = Trans_Enumerate_CommonPost(state);

    struct H
    {
        static bool is_generic(const ::HIR::TypeRef& ty)
        {
            return visit_ty_with(ty, [&](const auto& ty) {
                return ty.m_data.is_Generic();
                });
        }
        static bool is_generic(const ::HIR::PathParams& pp)
        {
            for(const auto& ty : pp.m_types)
                if( is_generic(ty) )
                    return true;
            return false;
        }
        static bool is_generic(const ::HIR::Path& p)
        {
            TU_MATCHA( (p.m_data), (pe),
            (Generic,
                return is_generic(pe.m_params);
                ),
            (UfcsKnown,
                if( is_generic(*pe.type) )
                    return true;
                if( is_generic(pe.trait.m_params) )
                    return true;
                if( is_generic(pe.params) )
                    return true;
                ),
            (UfcsInherent,
                if( is_generic(*pe.type) )
                    return true;
                if( is_generic(pe.params) )
                    return true;
                ),
            (UfcsUnknown,
                )
            )
            return false;
        }
    };

    // Strip out any functions/types/statics that are still generic?
    for(auto it = rv.m_functions.begin(); it != rv.m_functions.end(); )
    {
        if( H::is_generic(it->first) ) {
            rv.m_functions.erase(it++);
        }
        else {
            ++ it;
        }
    }
    for(auto it = rv.m_statics.begin(); it != rv.m_statics.end(); )
    {
        if( H::is_generic(it->first) ) {
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
    EnumState   state { crate };

    // TODO: Get a list of "root" functions (e.g. main, public functions, things used by public generics) and re-enumerate based on that.

    // Visit every function used
    for(const auto& ent : list.m_functions)
    {
        if( ent.second->monomorphised.code )
        {
            Trans_Enumerate_FillFrom_MIR(state, *ent.second->monomorphised.code, {});
        }
        else if( ent.second->ptr->m_code.m_mir )
        {
            Trans_Enumerate_FillFrom_MIR(state, *ent.second->ptr->m_code.m_mir, {});
        }
        else
        {
        }
    }

    // Remove any item in `list.m_functions` that doesn't appear in `state.rv.m_functions`
    for(auto it = list.m_functions.begin(); it != list.m_functions.end();)
    {
        auto it2 = state.rv.m_functions.find(it->first);
        if( it2 == state.rv.m_functions.end() )
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

namespace {
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

        ::std::map< ::HIR::TypeRef, bool > visited;
        ::std::set< const ::HIR::TypeRef*, PtrComp> active_set;

        TypeVisitor(const ::HIR::Crate& crate, ::std::vector< ::std::pair< ::HIR::TypeRef, bool > >& out_list):
            m_crate(crate),
            m_resolve(crate),
            out_list(out_list)
        {}

        void visit_struct(const ::HIR::GenericPath& path, const ::HIR::Struct& item) {
            static Span sp;
            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& {
                if( monomorphise_type_needed(x) ) {
                    tmp = monomorphise_type(sp, item.m_params, path.m_params, x);
                    m_resolve.expand_associated_types(sp, tmp);
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
            static Span sp;
            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& {
                if( monomorphise_type_needed(x) ) {
                    tmp = monomorphise_type(sp, item.m_params, path.m_params, x);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return x;
                }
                };
            for(const auto& variant : item.m_variants)
            {
                visit_type( monomorph(variant.second.ent) );
            }
        }
        void visit_enum(const ::HIR::GenericPath& path, const ::HIR::Enum& item) {
            static Span sp;
            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& {
                if( monomorphise_type_needed(x) ) {
                    tmp = monomorphise_type(sp, item.m_params, path.m_params, x);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return x;
                }
                };
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
            // If the type has already been visited, AND either this is a shallow visit, or the previous wasn't
            {
                auto it = visited.find(ty);
                if( it != visited.end() )
                {
                    if( it->second == false || mode == Mode::Shallow )
                    {
                        // Return early
                        return ;
                    }
                    DEBUG("-- " << ty << " already visited as shallow");
                    it->second = false;
                }
            }
            TRACE_FUNCTION_F(ty << " - " << (mode == Mode::Shallow ? "Shallow" : (mode == Mode::Normal ? "Normal" : "Deep")));

            if( mode == Mode::Shallow )
            {
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
                (
                    ),
                (Function,
                    visit_type(*te.m_rettype, Mode::Shallow);
                    for(const auto& sty : te.m_arg_types)
                        visit_type(sty, Mode::Shallow);
                    ),
                (Pointer,
                    visit_type(*te.inner, Mode::Shallow);
                    ),
                (Borrow,
                    visit_type(*te.inner, Mode::Shallow);
                    )
                )
            }
            else
            {
                if( active_set.find(&ty) != active_set.end() ) {
                    // TODO: Handle recursion
                    BUG(Span(), "- Type recursion on " << ty);
                }
                active_set.insert( &ty );

                TU_MATCHA( (ty.m_data), (te),
                // Impossible
                (Infer,
                    ),
                (Generic,
                    BUG(Span(), "Generic type hit in enumeration - " << ty);
                    ),
                (ErasedType,
                    //BUG(Span(), "ErasedType hit in enumeration - " << ty);
                    ),
                (Closure,
                    BUG(Span(), "Closure type hit in enumeration - " << ty);
                    ),
                // Nothing to do
                (Diverge,
                    ),
                (Primitive,
                    ),
                // Recursion!
                (Path,
                    TU_MATCHA( (te.binding), (tpb),
                    (Unbound,
                        BUG(Span(), "Unbound type hit in enumeration - " << ty);
                        ),
                    (Opaque,
                        BUG(Span(), "Opaque type hit in enumeration - " << ty);
                        ),
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

                    ASSERT_BUG(Span(), ! te.m_trait.m_path.m_path.m_components.empty(), "TODO: Data trait is empty, what can be done?");
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
                    visit_type(*te.inner, mode);
                    ),
                (Slice,
                    visit_type(*te.inner, mode);
                    ),
                (Borrow,
                    visit_type(*te.inner, mode != Mode::Deep ? Mode::Shallow : Mode::Deep);
                    ),
                (Pointer,
                    visit_type(*te.inner, mode != Mode::Deep ? Mode::Shallow : Mode::Deep);
                    ),
                (Tuple,
                    for(const auto& sty : te)
                        visit_type(sty, mode);
                    ),
                (Function,
                    // TODO: Should shallow=true for these too?
                    visit_type(*te.m_rettype, mode);
                    for(const auto& sty : te.m_arg_types)
                        visit_type(sty, mode);
                    )
                )
                active_set.erase( active_set.find(&ty) );
            }

            bool shallow = (mode == Mode::Shallow);
            {
                auto rv = visited.insert( ::std::make_pair(ty.clone(), shallow) );
                if( !rv.second && ! shallow )
                {
                    rv.first->second = false;
                }
            }
            out_list.push_back( ::std::make_pair(ty.clone(), shallow) );
            DEBUG("Add type " << ty << (shallow ? " (Shallow)": ""));
        }
    };
}

// Enumerate types required for the enumerated items
void Trans_Enumerate_Types(EnumState& state)
{
    static Span sp;
    TypeVisitor tv { state.crate, state.rv.m_types };

    unsigned int types_count = 0;
    bool constructors_added;
    do
    {
        // Visit all functions that haven't been type-visited yet
        for(unsigned int i = 0; i < state.fcns_to_type_visit.size(); i++)
        {
            auto p = state.fcns_to_type_visit[i];
            TRACE_FUNCTION_F("Function " << ::std::find_if(state.rv.m_functions.begin(), state.rv.m_functions.end(), [&](const auto&x){ return x.second.get() == p; })->first);
            assert(p->ptr);
            const auto& fcn = *p->ptr;
            const auto& pp = p->pp;

            ::HIR::TypeRef   tmp;
            auto monomorph = [&](const auto& ty)->const auto& {
                return monomorphise_type_needed(ty) ? tmp = pp.monomorph(tv.m_resolve, ty) : ty;
                };
            // Handle erased types in the return type.
            if( visit_ty_with(fcn.m_return, [](const auto& x) { return x.m_data.is_ErasedType()||x.m_data.is_Generic(); }) )
            {
                auto ret_ty = clone_ty_with(sp, fcn.m_return, [&](const auto& x, auto& out) {
                    if( const auto* te = x.m_data.opt_ErasedType() ) {
                        out = pp.monomorph(tv.m_resolve, fcn.m_code.m_erased_types.at(te->m_index));
                        return true;
                    }
                    else if( x.m_data.is_Generic() ) {
                        out = pp.monomorph(tv.m_resolve, x);
                        return true;
                    }
                    else {
                        return false;
                    }
                    });
                tv.m_resolve.expand_associated_types(sp, ret_ty);
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

                // TODO: Find all LValue::Deref instances and get the result type
                for(const auto& block : mir.blocks)
                {
                    struct H {
                        static const ::HIR::TypeRef& visit_lvalue(TypeVisitor& tv, const Trans_Params& pp, const ::HIR::Function& fcn, const ::MIR::LValue& lv, ::HIR::TypeRef* tmp_ty_ptr = nullptr) {
                            static ::HIR::TypeRef   blank;
                            TRACE_FUNCTION_F(lv << (tmp_ty_ptr ? " [type]" : ""));
                            auto monomorph_outer = [&](const auto& tpl)->const auto& {
                                assert(tmp_ty_ptr);
                                if( monomorphise_type_needed(tpl) ) {
                                    return *tmp_ty_ptr = pp.monomorph(tv.m_resolve, tpl);
                                }
                                else {
                                    return tpl;
                                }
                                };
                            // Recurse, if Deref get the type and add it to the visitor
                            TU_MATCHA( (lv), (e),
                            (Return,
                                if( tmp_ty_ptr ) {
                                    TODO(Span(), "Get return type for MIR type enumeration");
                                }
                                ),
                            (Argument,
                                if( tmp_ty_ptr ) {
                                    return monomorph_outer(fcn.m_args[e.idx].second);
                                }
                                ),
                            (Local,
                                if( tmp_ty_ptr ) {
                                    return monomorph_outer(fcn.m_code.m_mir->locals[e]);
                                }
                                ),
                            (Static,
                                if( tmp_ty_ptr ) {
                                    const auto& path = e;
                                    TU_MATCHA( (path.m_data), (pe),
                                    (Generic,
                                        ASSERT_BUG(Span(), pe.m_params.m_types.empty(), "Path params on static - " << path);
                                        const auto& s = tv.m_resolve.m_crate.get_static_by_path(Span(), pe.m_path);
                                        return s.m_type;
                                        ),
                                    (UfcsKnown,
                                        TODO(Span(), "LValue::Static - UfcsKnown - " << path);
                                        ),
                                    (UfcsUnknown,
                                        BUG(Span(), "Encountered UfcsUnknown in LValue::Static - " << path);
                                        ),
                                    (UfcsInherent,
                                        TODO(Span(), "LValue::Static - UfcsInherent - " << path);
                                        )
                                    )
                                }
                                ),
                            (Field,
                                const auto& ity = visit_lvalue(tv,pp,fcn,  *e.val, tmp_ty_ptr);
                                if( tmp_ty_ptr )
                                {
                                    TU_MATCH_DEF(::HIR::TypeRef::Data, (ity.m_data), (te),
                                    (
                                        BUG(Span(), "Field access of unexpected type - " << ity);
                                        ),
                                    (Tuple,
                                        return te[e.field_index];
                                        ),
                                    (Array,
                                        return *te.inner;
                                        ),
                                    (Slice,
                                        return *te.inner;
                                        ),
                                    (Path,
                                        ASSERT_BUG(Span(), te.binding.is_Struct(), "Field on non-Struct - " << ity);
                                        const auto& str = *te.binding.as_Struct();
                                        auto monomorph = [&](const auto& ty)->const auto& {
                                            if( monomorphise_type_needed(ty) ) {
                                                *tmp_ty_ptr = monomorphise_type(sp, str.m_params, te.path.m_data.as_Generic().m_params, ty);
                                                tv.m_resolve.expand_associated_types(sp, *tmp_ty_ptr);
                                                return *tmp_ty_ptr;
                                            }
                                            else {
                                                return ty;
                                            }
                                            };
                                        TU_MATCHA( (str.m_data), (se),
                                        (Unit,
                                            BUG(Span(), "Field on unit-like struct - " << ity);
                                            ),
                                        (Tuple,
                                            ASSERT_BUG(Span(), e.field_index < se.size(), "Field index out of range in struct " << te.path);
                                            return monomorph(se.at(e.field_index).ent);
                                            ),
                                        (Named,
                                            ASSERT_BUG(Span(), e.field_index < se.size(), "Field index out of range in struct " << te.path);
                                            return monomorph(se.at(e.field_index).second.ent);
                                            )
                                        )
                                        )
                                    )
                                }
                                ),
                            (Deref,
                                ::HIR::TypeRef  tmp;
                                if( !tmp_ty_ptr )   tmp_ty_ptr = &tmp;

                                const auto& ity = visit_lvalue(tv,pp,fcn,  *e.val, tmp_ty_ptr);
                                TU_MATCH_DEF(::HIR::TypeRef::Data, (ity.m_data), (te),
                                (
                                    BUG(Span(), "Deref of unexpected type - " << ity);
                                    ),
                                (Path,
                                    if( const auto* inner_ptr = tv.m_resolve.is_type_owned_box(ity) )
                                    {
                                        DEBUG("- Add type " << ity);
                                        tv.visit_type(*inner_ptr);
                                        return *inner_ptr;
                                    }
                                    else {
                                        BUG(Span(), "Deref on unexpected type - " << ity);
                                    }
                                    ),
                                (Borrow,
                                    DEBUG("- Add type " << ity);
                                    tv.visit_type(*te.inner);
                                    return *te.inner;
                                    ),
                                (Pointer,
                                    DEBUG("- Add type " << ity);
                                    tv.visit_type(*te.inner);
                                    return *te.inner;
                                    )
                                )
                                ),
                            (Index,
                                visit_lvalue(tv,pp,fcn,  *e.idx, tmp_ty_ptr);
                                const auto& ity = visit_lvalue(tv,pp,fcn,  *e.val, tmp_ty_ptr);
                                if( tmp_ty_ptr )
                                {
                                    TU_MATCH_DEF(::HIR::TypeRef::Data, (ity.m_data), (te),
                                    (
                                        BUG(Span(), "Index of unexpected type - " << ity);
                                        ),
                                    (Array,
                                        return *te.inner;
                                        ),
                                    (Slice,
                                        return *te.inner;
                                        )
                                    )
                                }
                                ),
                            (Downcast,
                                const auto& ity = visit_lvalue(tv,pp,fcn,  *e.val, tmp_ty_ptr);
                                if( tmp_ty_ptr )
                                {
                                    TU_MATCH_DEF( ::HIR::TypeRef::Data, (ity.m_data), (te),
                                    (
                                        BUG(Span(), "Downcast on unexpected type - " << ity);
                                        ),
                                    (Path,
                                        if( te.binding.is_Enum() )
                                        {
                                            const auto& enm = *te.binding.as_Enum();
                                            auto monomorph = [&](const auto& ty)->auto {
                                                ::HIR::TypeRef rv = monomorphise_type(pp.sp, enm.m_params, te.path.m_data.as_Generic().m_params, ty);
                                                tv.m_resolve.expand_associated_types(sp, rv);
                                                return rv;
                                                };
                                            ASSERT_BUG(Span(), enm.m_data.is_Data(), "");
                                            const auto& variants = enm.m_data.as_Data();
                                            ASSERT_BUG(Span(), e.variant_index < variants.size(), "Variant index out of range");
                                            const auto& raw_ty = variants[e.variant_index].type;
                                            if( monomorphise_type_needed(raw_ty) ) {
                                                return *tmp_ty_ptr = monomorph(raw_ty);
                                            }
                                            else {
                                                return raw_ty;
                                            }
                                        }
                                        else
                                        {
                                            const auto& unm = *te.binding.as_Union();
                                            ASSERT_BUG(Span(), e.variant_index < unm.m_variants.size(), "Variant index out of range");
                                            const auto& variant = unm.m_variants[e.variant_index];
                                            const auto& var_ty = variant.second.ent;

                                            if( monomorphise_type_needed(var_ty) ) {
                                                *tmp_ty_ptr = monomorphise_type(pp.sp, unm.m_params, te.path.m_data.as_Generic().m_params, variant.second.ent);
                                                tv.m_resolve.expand_associated_types(pp.sp, *tmp_ty_ptr);
                                                return *tmp_ty_ptr;
                                            }
                                            else {
                                                return var_ty;
                                            }
                                        }
                                        )
                                    )
                                }
                                )
                            )
                            return blank;
                        }

                        static void visit_param(TypeVisitor& tv, const Trans_Params& pp, const ::HIR::Function& fcn, const ::MIR::Param& p)
                        {
                            TU_MATCHA( (p), (e),
                            (LValue,
                                H::visit_lvalue(tv, pp, fcn, e);
                                ),
                            (Constant,
                                )
                            )
                        }
                    };
                    for(const auto& stmt : block.statements)
                    {
                        TU_MATCHA( (stmt), (se),
                        (Drop,
                            H::visit_lvalue(tv,pp,fcn, se.slot);
                            ),
                        (SetDropFlag,
                            ),
                        (Asm,
                            for(const auto& v : se.outputs)
                                H::visit_lvalue(tv,pp,fcn, v.second);
                            for(const auto& v : se.inputs)
                                H::visit_lvalue(tv,pp,fcn, v.second);
                            ),
                        (ScopeEnd,
                            ),
                        (Assign,
                            H::visit_lvalue(tv,pp,fcn, se.dst);
                            TU_MATCHA( (se.src), (re),
                            (Use,
                                H::visit_lvalue(tv,pp,fcn, re);
                                ),
                            (Constant,
                                ),
                            (SizedArray,
                                H::visit_param(tv,pp,fcn, re.val);
                                ),
                            (Borrow,
                                H::visit_lvalue(tv,pp,fcn, re.val);
                                ),
                            (Cast,
                                H::visit_lvalue(tv,pp,fcn, re.val);
                                ),
                            (BinOp,
                                H::visit_param(tv,pp,fcn, re.val_l);
                                H::visit_param(tv,pp,fcn, re.val_l);
                                ),
                            (UniOp,
                                H::visit_lvalue(tv,pp,fcn, re.val);
                                ),
                            (DstMeta,
                                H::visit_lvalue(tv,pp,fcn, re.val);
                                ),
                            (DstPtr,
                                H::visit_lvalue(tv,pp,fcn, re.val);
                                ),
                            (MakeDst,
                                H::visit_param(tv,pp,fcn, re.ptr_val);
                                H::visit_param(tv,pp,fcn, re.meta_val);
                                ),
                            (Tuple,
                                for(const auto& v : re.vals)
                                    H::visit_param(tv,pp,fcn, v);
                                ),
                            (Array,
                                for(const auto& v : re.vals)
                                    H::visit_param(tv,pp,fcn, v);
                                ),
                            (Variant,
                                H::visit_param(tv,pp,fcn, re.val);
                                ),
                            (Struct,
                                for(const auto& v : re.vals)
                                    H::visit_param(tv,pp,fcn, v);
                                )
                            )
                            )
                        )
                    }
                    TU_MATCHA( (block.terminator), (te),
                    (Incomplete, ),
                    (Return, ),
                    (Diverge, ),
                    (Goto, ),
                    (Panic, ),
                    (If,
                        H::visit_lvalue(tv,pp,fcn, te.cond);
                        ),
                    (Switch,
                        H::visit_lvalue(tv,pp,fcn, te.val);
                        ),
                    (SwitchValue,
                        H::visit_lvalue(tv,pp,fcn, te.val);
                        ),
                    (Call,
                        if( te.fcn.is_Value() )
                            H::visit_lvalue(tv,pp,fcn, te.fcn.as_Value());
                        else if( te.fcn.is_Intrinsic() )
                        {
                            for(const auto& ty : te.fcn.as_Intrinsic().params.m_types)
                                tv.visit_type(monomorph(ty));
                        }
                        H::visit_lvalue(tv,pp,fcn, te.ret_val);
                        for(const auto& arg : te.args)
                            H::visit_param(tv,pp,fcn, arg);
                        )
                    )
                }
            }
        }
        state.fcns_to_type_visit.clear();
        // TODO: Similarly restrict revisiting of statics.
        for(const auto& ent : state.rv.m_statics)
        {
            TRACE_FUNCTION_F("Enumerate static " << ent.first);
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

            auto vtable_ty_spath = gpath.m_path;
            vtable_ty_spath.m_components.back() += "#vtable";
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

            tv.visit_type( *ent.first.m_data.as_UfcsKnown().type );
            tv.visit_type( ::HIR::TypeRef( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref ) );
        }

        constructors_added = false;
        for(unsigned int i = types_count; i < state.rv.m_types.size(); i ++ )
        {
            const auto& ent = state.rv.m_types[i];
            // Shallow? Skip.
            if( ent.second )
                continue ;
            const auto& ty = ent.first;
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
                ASSERT_BUG(Span(), markings_ptr, "Path binding not set correctly - " << ty);

                // If the type has a drop impl, and it's either defined in this crate or has params (and thus was monomorphised)
                if( markings_ptr->has_drop_impl && (te.path.m_data.as_Generic().m_path.m_crate_name == state.crate.m_crate_name || te.path.m_data.as_Generic().m_params.has_params()) )
                {
                    // Add the Drop impl to the codegen list
                    Trans_Enumerate_FillFrom_Path(state,  ::HIR::Path( ty.clone(), state.crate.get_lang_item_path(sp, "drop"), "drop"), {});
                    constructors_added = true;
                }
            }

            if( const auto* ity = tv.m_resolve.is_type_owned_box(ty) )
            {
                // Reqire drop glue for inner type.
                // - Should that already exist?
                // Requires box_free lang item
                Trans_Enumerate_FillFrom_Path(state, ::HIR::GenericPath( state.crate.get_lang_item_path(sp, "box_free"), { ity->clone() } ), {});;
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
    EntPtr get_ent_simplepath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path)
    {

        const ::HIR::ValueItem* vip;
        if( path.m_components.size() > 1 )
        {
            const auto& mi = crate.get_typeitem_by_path(sp, path, /*ignore_crate_name=*/false, /*ignore_last_node=*/true);

            TU_MATCH_DEF( ::HIR::TypeItem, (mi), (e),
            (
                BUG(sp, "Node " << path.m_components.size()-1 << " of path " << path << " wasn't a module");
                ),
            (Enum,
                // TODO: Check that this is a tuple variant
                return EntPtr::make_AutoGenerate({});
                ),
            (Module,
                auto it = e.m_value_items.find( path.m_components.back() );
                if( it == e.m_value_items.end() ) {
                    return EntPtr {};
                }
                vip = &it->second->ent;
                )
            )
        }
        else
        {
            vip = &crate.get_valitem_by_path(sp, path);
        }


        TU_MATCH( ::HIR::ValueItem, (*vip), (e),
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
        BUG(sp, "Path " << path << " pointed to a invalid item - " << vip->tag_str());
    }
    EntPtr get_ent_fullpath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::Path& path, ::HIR::PathParams& impl_pp)
    {
        TRACE_FUNCTION_F(path);
        StaticTraitResolve  resolve { crate };

        if( const auto* pe = path.m_data.opt_Generic() )
        {
            return get_ent_simplepath(sp, crate, pe->m_path);
        }
        else if( const auto* pe = path.m_data.opt_UfcsInherent() )
        {
            // Easy (ish)
            EntPtr rv;
            crate.find_type_impls(*pe->type, [](const auto&x)->const auto& { return x; }, [&](const auto& impl) {
                DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                {
                    auto fit = impl.m_methods.find(pe->item);
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
        }
        else if( const auto* pe = path.m_data.opt_UfcsKnown() )
        {
            EntPtr rv;

            // Obtain trait pointer (for default impl and to know what the item type is)
            const auto& trait_ref = crate.get_trait_by_path(sp, pe->trait.m_path);
            auto trait_vi_it = trait_ref.m_values.find(pe->item);
            ASSERT_BUG(sp, trait_vi_it != trait_ref.m_values.end(), "Couldn't find item " << pe->item << " in trait " << pe->trait.m_path);
            const auto& trait_vi = trait_vi_it->second;

            bool is_dynamic = false;
            ::std::vector<::HIR::TypeRef>    best_impl_params;
            const ::HIR::TraitImpl* best_impl = nullptr;
            resolve.find_impl(sp, pe->trait.m_path, pe->trait.m_params, *pe->type, [&](auto impl_ref, auto is_fuzz) {
                DEBUG("[get_ent_fullpath] Found " << impl_ref);
                //ASSERT_BUG(sp, !is_fuzz, "Fuzzy match not allowed here");
                if( ! impl_ref.m_data.is_TraitImpl() ) {
                    DEBUG("Trans impl search found an invalid impl type");
                    is_dynamic = true;
                    // TODO: This can only really happen if it's a trait object magic impl, which should become a vtable lookup.
                    return true;
                }
                const auto& impl_ref_e = impl_ref.m_data.as_TraitImpl();
                const auto& impl = *impl_ref_e.impl;
                ASSERT_BUG(sp, impl.m_trait_args.m_types.size() == pe->trait.m_params.m_types.size(), "Trait parameter count mismatch " << impl.m_trait_args << " vs " << pe->trait.m_params);

                if( best_impl == nullptr || impl.more_specific_than(*best_impl) ) {
                    best_impl = &impl;
                    bool is_spec = false;
                    TU_MATCHA( (trait_vi), (ve),
                    (Constant,
                        auto it = impl.m_constants.find(pe->item);
                        if( it == impl.m_constants.end() ) {
                            DEBUG("Constant " << pe->item << " missing in trait " << pe->trait << " for " << *pe->type);
                            return false;
                        }
                        is_spec = it->second.is_specialisable;
                        ),
                    (Static,
                        if( pe->item == "vtable#" ) {
                            is_spec = true;
                            break;
                        }
                        auto it = impl.m_statics.find(pe->item);
                        if( it == impl.m_statics.end() ) {
                            DEBUG("Static " << pe->item << " missing in trait " << pe->trait << " for " << *pe->type);
                            return false;
                        }
                        is_spec = it->second.is_specialisable;
                        ),
                    (Function,
                        auto fit = impl.m_methods.find(pe->item);
                        if( fit == impl.m_methods.end() ) {
                            DEBUG("Method " << pe->item << " missing in trait " << pe->trait << " for " << *pe->type);
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
                    if( is_spec )
                        DEBUG("- Specialisable");
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
            (Constant,
                auto it = impl.m_constants.find(pe->item);
                if( it != impl.m_constants.end() )
                {
                    DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                    return EntPtr { &it->second.data };
                }
                TODO(sp, "Associated constant - " << path);
                ),
            (Static,
                if( pe->item == "vtable#" )
                {
                    DEBUG("VTable, autogen");
                    return EntPtr::make_AutoGenerate( {} );
                }
                auto it = impl.m_statics.find(pe->item);
                if( it != impl.m_statics.end() )
                {
                    DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                    return EntPtr { &it->second.data };
                }
                TODO(sp, "Associated static - " << path);
                ),
            (Function,
                auto fit = impl.m_methods.find(pe->item);
                if( fit != impl.m_methods.end() )
                {
                    DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                    return EntPtr { &fit->second.data };
                }
                impl_pp = pe->trait.m_params.clone();
                // HACK! By adding a new parameter here, the MIR will always be monomorphised
                impl_pp.m_types.push_back( ::HIR::TypeRef() );
                return EntPtr { &ve };
                )
            )
            BUG(sp, "");
        }
        else
        {
            // TODO: Are these valid at this point in compilation?
            TODO(sp, "get_ent_fullpath(path = " << path << ")");
        }
        throw "";
    }
}

void Trans_Enumerate_FillFrom_Path(EnumState& state, const ::HIR::Path& path, const Trans_Params& pp)
{
    TRACE_FUNCTION_F(path);
    Span    sp;
    auto path_mono = pp.monomorph(state.crate, path);
    DEBUG("- " << path_mono);
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
    auto item_ref = get_ent_fullpath(sp, state.crate, path_mono, sub_pp.pp_impl);
    TU_MATCHA( (item_ref), (e),
    (NotFound,
        BUG(sp, "Item not found for " << path_mono);
        ),
    (AutoGenerate,
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
        else if( path_mono.m_data.is_UfcsKnown() && path_mono.m_data.as_UfcsKnown().type->m_data.is_TraitObject() )
        {
            // Must have been a dynamic dispatch request, just leave as-is
        }
        // - <fn(...) as Fn*>::call*
        else if( path_mono.m_data.is_UfcsKnown() && path_mono.m_data.as_UfcsKnown().type->m_data.is_Function() )
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
        state.enum_fcn(mv$(path_mono), *e, mv$(sub_pp));
        ),
    (Static,
        if( auto* ptr = state.rv.add_static(mv$(path_mono)) )
        {
            Trans_Enumerate_FillFrom(state, *e, *ptr, mv$(sub_pp));
        }
        ),
    (Constant,
        Trans_Enumerate_FillFrom_Literal(state, e->m_value_res, sub_pp);
        )
    )
}
void Trans_Enumerate_FillFrom_MIR_LValue(EnumState& state, const ::MIR::LValue& lv, const Trans_Params& pp)
{
    TU_MATCHA( (lv), (e),
    (Return,
        ),
    (Argument,
        ),
    (Local,
        ),
    (Static,
        Trans_Enumerate_FillFrom_Path(state, e, pp);
        ),
    (Field,
        Trans_Enumerate_FillFrom_MIR_LValue(state, *e.val, pp);
        ),
    (Deref,
        Trans_Enumerate_FillFrom_MIR_LValue(state, *e.val, pp);
        ),
    (Index,
        Trans_Enumerate_FillFrom_MIR_LValue(state, *e.val, pp);
        Trans_Enumerate_FillFrom_MIR_LValue(state, *e.idx, pp);
        ),
    (Downcast,
        Trans_Enumerate_FillFrom_MIR_LValue(state, *e.val, pp);
        )
    )
}
void Trans_Enumerate_FillFrom_MIR_Constant(EnumState& state, const ::MIR::Constant& c, const Trans_Params& pp)
{
    TU_MATCHA( (c), (ce),
    (Int, ),
    (Uint,),
    (Float, ),
    (Bool, ),
    (Bytes, ),
    (StaticString, ),  // String
    (Const,
        //Trans_Enumerate_FillFrom_Path(state, ce.p, pp);
        ),
    (ItemAddr,
        Trans_Enumerate_FillFrom_Path(state, ce, pp);
        )
    )
}
void Trans_Enumerate_FillFrom_MIR_Param(EnumState& state, const ::MIR::Param& p, const Trans_Params& pp)
{
    TU_MATCHA( (p), (e),
    (LValue, Trans_Enumerate_FillFrom_MIR_LValue(state, e, pp); ),
    (Constant, Trans_Enumerate_FillFrom_MIR_Constant(state, e, pp); )
    )
}
void Trans_Enumerate_FillFrom_MIR(EnumState& state, const ::MIR::Function& code, const Trans_Params& pp)
{
    for(const auto& bb : code.blocks)
    {
        for(const auto& stmt : bb.statements)
        {
            TU_MATCHA((stmt), (se),
            (Assign,
                DEBUG("- " << se.dst << " = " << se.src);
                Trans_Enumerate_FillFrom_MIR_LValue(state, se.dst, pp);
                TU_MATCHA( (se.src), (e),
                (Use,
                    Trans_Enumerate_FillFrom_MIR_LValue(state, e, pp);
                    ),
                (Constant,
                    Trans_Enumerate_FillFrom_MIR_Constant(state, e, pp);
                    ),
                (SizedArray,
                    Trans_Enumerate_FillFrom_MIR_Param(state, e.val, pp);
                    ),
                (Borrow,
                    Trans_Enumerate_FillFrom_MIR_LValue(state, e.val, pp);
                    ),
                (Cast,
                    Trans_Enumerate_FillFrom_MIR_LValue(state, e.val, pp);
                    ),
                (BinOp,
                    Trans_Enumerate_FillFrom_MIR_Param(state, e.val_l, pp);
                    Trans_Enumerate_FillFrom_MIR_Param(state, e.val_r, pp);
                    ),
                (UniOp,
                    Trans_Enumerate_FillFrom_MIR_LValue(state, e.val, pp);
                    ),
                (DstMeta,
                    Trans_Enumerate_FillFrom_MIR_LValue(state, e.val, pp);
                    ),
                (DstPtr,
                    Trans_Enumerate_FillFrom_MIR_LValue(state, e.val, pp);
                    ),
                (MakeDst,
                    Trans_Enumerate_FillFrom_MIR_Param(state, e.ptr_val, pp);
                    Trans_Enumerate_FillFrom_MIR_Param(state, e.meta_val, pp);
                    ),
                (Tuple,
                    for(const auto& val : e.vals)
                        Trans_Enumerate_FillFrom_MIR_Param(state, val, pp);
                    ),
                (Array,
                    for(const auto& val : e.vals)
                        Trans_Enumerate_FillFrom_MIR_Param(state, val, pp);
                    ),
                (Variant,
                    Trans_Enumerate_FillFrom_MIR_Param(state, e.val, pp);
                    ),
                (Struct,
                    for(const auto& val : e.vals)
                        Trans_Enumerate_FillFrom_MIR_Param(state, val, pp);
                    )
                )
                ),
            (Asm,
                DEBUG("- asm! ...");
                for(const auto& v : se.inputs)
                    Trans_Enumerate_FillFrom_MIR_LValue(state, v.second, pp);
                for(const auto& v : se.outputs)
                    Trans_Enumerate_FillFrom_MIR_LValue(state, v.second, pp);
                ),
            (SetDropFlag,
                ),
            (ScopeEnd,
                ),
            (Drop,
                DEBUG("- DROP " << se.slot);
                Trans_Enumerate_FillFrom_MIR_LValue(state, se.slot, pp);
                // TODO: Ensure that the drop glue for this type is generated
                )
            )
        }
        DEBUG("> " << bb.terminator);
        TU_MATCHA( (bb.terminator), (e),
        (Incomplete, ),
        (Return, ),
        (Diverge, ),
        (Goto, ),
        (Panic, ),
        (If,
            Trans_Enumerate_FillFrom_MIR_LValue(state, e.cond, pp);
            ),
        (Switch,
            Trans_Enumerate_FillFrom_MIR_LValue(state, e.val, pp);
            ),
        (SwitchValue,
            Trans_Enumerate_FillFrom_MIR_LValue(state, e.val, pp);
            ),
        (Call,
            Trans_Enumerate_FillFrom_MIR_LValue(state, e.ret_val, pp);
            TU_MATCHA( (e.fcn), (e2),
            (Value,
                Trans_Enumerate_FillFrom_MIR_LValue(state, e2, pp);
                ),
            (Path,
                Trans_Enumerate_FillFrom_Path(state, e2, pp);
                ),
            (Intrinsic,
                if( e2.name == "type_id" ) {
                    // Add <T>::#type_id to the enumerate list
                    state.rv.m_typeids.insert( pp.monomorph(state.crate, e2.params.m_types.at(0)) );
                }
                )
            )
            for(const auto& arg : e.args)
                Trans_Enumerate_FillFrom_MIR_Param(state, arg, pp);
            )
        )
    }
}

void Trans_Enumerate_FillFrom_VTable(EnumState& state, ::HIR::Path vtable_path, const Trans_Params& pp)
{
    static Span sp;
    const auto& type = *vtable_path.m_data.as_UfcsKnown().type;
    const auto& trait_path = vtable_path.m_data.as_UfcsKnown().trait;
    const auto& tr = state.crate.get_trait_by_path(Span(), trait_path.m_path);

    ASSERT_BUG(sp, !type.m_data.is_Slice(), "Getting vtable for unsized type - " << vtable_path);

    auto monomorph_cb_trait = monomorphise_type_get_cb(sp, &type, &trait_path.m_params, nullptr);
    for(const auto& m : tr.m_value_indexes)
    {
        DEBUG("- " << m.second.first << " = " << m.second.second << " :: " << m.first);
        auto gpath = monomorphise_genericpath_with(sp, m.second.second, monomorph_cb_trait, false);
        Trans_Enumerate_FillFrom_Path(state, ::HIR::Path(type.clone(), mv$(gpath), m.first), {});
    }
}

void Trans_Enumerate_FillFrom_Literal(EnumState& state, const ::HIR::Literal& lit, const Trans_Params& pp)
{
    TU_MATCHA( (lit), (e),
    (Invalid,
        ),
    (List,
        for(const auto& v : e)
            Trans_Enumerate_FillFrom_Literal(state, v, pp);
        ),
    (Variant,
        Trans_Enumerate_FillFrom_Literal(state, *e.val, pp);
        ),
    (Integer,
        ),
    (Float,
        ),
    (BorrowPath,
        Trans_Enumerate_FillFrom_Path(state, e, pp);
        ),
    (BorrowData,
        Trans_Enumerate_FillFrom_Literal(state, *e, pp);
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
    TRACE_FUNCTION_F("Function pp=" << pp.pp_method<<"+"<<pp.pp_impl);
    if( function.m_code.m_mir )
    {
        Trans_Enumerate_FillFrom_MIR(state, *function.m_code.m_mir, pp);
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
    else*/ if( item.m_type.m_data.is_Infer() )
    {
        BUG(Span(), "Enumerating static with no assigned type (unused elevated literal)");
    }
    else if( ! item.m_value_res.is_Invalid() )
    {
        Trans_Enumerate_FillFrom_Literal(state, item.m_value_res, pp);
    }
    out_stat.ptr = &item;
    out_stat.pp = mv$(pp);
}

