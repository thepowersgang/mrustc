/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/optimise.cpp
 * - MIR Optimisations
 *
 */
#include "main_bindings.hpp"
#include "mir.hpp"
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>
#include <mir/helpers.hpp>
#include <mir/operations.hpp>
#include <mir/visit_crate_mir.hpp>
#include <algorithm>
#include <iomanip>
#include <trans/target.hpp>
#include <trans/trans_list.hpp> // Note: This is included for inlining after enumeration and monomorph

#include <hir/expr.hpp> // HACK


#define DUMP_BEFORE_ALL 1
#define DUMP_BEFORE_CONSTPROPAGATE 0
#define CHECK_AFTER_PASS    1
#define CHECK_AFTER_ALL     1
#define DUMP_AFTER_PASS     1

#define DUMP_AFTER_DONE     0
#define CHECK_AFTER_DONE    2   // 1 = Check before GC, 2 = check before and after GC

namespace {
    ::MIR::BasicBlockId get_new_target(const ::MIR::TypeResolve& state, ::MIR::BasicBlockId bb)
    {
        const auto& target = state.get_block(bb);
        if( target.statements.size() != 0 )
        {
            return bb;
        }
        else if( !target.terminator.is_Goto() )
        {
            return bb;
        }
        else
        {
            // Make sure we don't infinite loop
            if( bb == target.terminator.as_Goto() )
                return bb;

            auto rv = get_new_target(state, target.terminator.as_Goto());
            DEBUG(bb << " => " << rv);
            return rv;
        }
    }

    enum class ValUsage {
        Move,   // Moving read (even if T: Copy)
        Read,   // Non-moving read (e.g. indexing or deref, TODO: &move pointers?)
        Write,  // Mutation
        Borrow, // Any borrow
    };

    bool visit_mir_lvalue_mut(::MIR::LValue& lv, ValUsage u, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        //TRACE_FUNCTION_F(lv);
        if( cb(lv, u) )
            return true;
        TU_MATCHA( (lv), (e),
        (Return,
            ),
        (Argument,
            ),
        (Local,
            ),
        (Static,
            ),
        (Field,
            // HACK: If "moving", use a "Read" value usage (covers some quirks)
            return visit_mir_lvalue_mut(*e.val, u == ValUsage::Move ? ValUsage::Read : u, cb);
            ),
        (Deref,
            return visit_mir_lvalue_mut(*e.val, u == ValUsage::Borrow ? u : ValUsage::Read, cb);
            ),
        (Index,
            bool rv = false;
            rv |= visit_mir_lvalue_mut(*e.val, u, cb);
            rv |= visit_mir_lvalue_mut(*e.idx, ValUsage::Read, cb);
            return rv;
            ),
        (Downcast,
            return visit_mir_lvalue_mut(*e.val, u, cb);
            )
        )
        return false;
    }
    bool visit_mir_lvalue(const ::MIR::LValue& lv, ValUsage u, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        return visit_mir_lvalue_mut( const_cast<::MIR::LValue&>(lv), u, [&](auto& v, auto u) { return cb(v,u); } );
    }

    bool visit_mir_lvalue_mut(::MIR::Param& p, ValUsage u, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        if( auto* e = p.opt_LValue() )
        {
            return visit_mir_lvalue_mut(*e, u, cb);
        }
        else
        {
            return false;
        }
    }
    bool visit_mir_lvalue(const ::MIR::Param& p, ValUsage u, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        if( const auto* e = p.opt_LValue() )
        {
            return visit_mir_lvalue(*e, u, cb);
        }
        else
        {
            return false;
        }
    }

    bool visit_mir_lvalues_mut(::MIR::RValue& rval, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        bool rv = false;
        TU_MATCHA( (rval), (se),
        (Use,
            rv |= visit_mir_lvalue_mut(se, ValUsage::Move, cb); // Can move
            ),
        (Constant,
            ),
        (SizedArray,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Read, cb); // Has to be Read
            ),
        (Borrow,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Borrow, cb);
            ),
        (Cast,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Read, cb); // Also has to be read
            ),
        (BinOp,
            rv |= visit_mir_lvalue_mut(se.val_l, ValUsage::Read, cb);   // Same
            rv |= visit_mir_lvalue_mut(se.val_r, ValUsage::Read, cb);
            ),
        (UniOp,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Read, cb);
            ),
        (DstMeta,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Read, cb); // Reads
            ),
        (DstPtr,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Read, cb);
            ),
        (MakeDst,
            rv |= visit_mir_lvalue_mut(se.ptr_val, ValUsage::Move, cb);
            rv |= visit_mir_lvalue_mut(se.meta_val, ValUsage::Read, cb);    // Note, metadata has to be Copy
            ),
        (Tuple,
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, ValUsage::Move, cb);
            ),
        (Array,
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, ValUsage::Move, cb);
            ),
        (Variant,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Move, cb);
            ),
        (Struct,
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, ValUsage::Move, cb);
            )
        )
        return rv;
    }
    bool visit_mir_lvalues(const ::MIR::RValue& rval, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        return visit_mir_lvalues_mut(const_cast<::MIR::RValue&>(rval), [&](auto& lv, auto u){ return cb(lv, u); });
    }

    bool visit_mir_lvalues_mut(::MIR::Statement& stmt, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        bool rv = false;
        TU_MATCHA( (stmt), (e),
        (Assign,
            rv |= visit_mir_lvalues_mut(e.src, cb);
            rv |= visit_mir_lvalue_mut(e.dst, ValUsage::Write, cb);
            ),
        (Asm,
            for(auto& v : e.inputs)
                rv |= visit_mir_lvalue_mut(v.second, ValUsage::Read, cb);
            for(auto& v : e.outputs)
                rv |= visit_mir_lvalue_mut(v.second, ValUsage::Write, cb);
            ),
        (SetDropFlag,
            ),
        (Drop,
            // Well, it mutates...
            rv |= visit_mir_lvalue_mut(e.slot, ValUsage::Write, cb);
            ),
        (ScopeEnd,
            )
        )
        return rv;
    }
    bool visit_mir_lvalues(const ::MIR::Statement& stmt, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        return visit_mir_lvalues_mut(const_cast<::MIR::Statement&>(stmt), [&](auto& lv, auto im){ return cb(lv, im); });
    }

    void visit_mir_lvalues_mut(::MIR::Terminator& term, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        TU_MATCHA( (term), (e),
        (Incomplete,
            ),
        (Return,
            ),
        (Diverge,
            ),
        (Goto,
            ),
        (Panic,
            ),
        (If,
            visit_mir_lvalue_mut(e.cond, ValUsage::Read, cb);
            ),
        (Switch,
            visit_mir_lvalue_mut(e.val, ValUsage::Read, cb);
            ),
        (SwitchValue,
            visit_mir_lvalue_mut(e.val, ValUsage::Read, cb);
            ),
        (Call,
            if( e.fcn.is_Value() ) {
                visit_mir_lvalue_mut(e.fcn.as_Value(), ValUsage::Read, cb);
            }
            for(auto& v : e.args)
                visit_mir_lvalue_mut(v, ValUsage::Move, cb);
            visit_mir_lvalue_mut(e.ret_val, ValUsage::Write, cb);
            )
        )
    }
    void visit_mir_lvalues(const ::MIR::Terminator& term, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        visit_mir_lvalues_mut(const_cast<::MIR::Terminator&>(term), [&](auto& lv, auto im){ return cb(lv, im); });
    }

    void visit_mir_lvalues_mut(::MIR::TypeResolve& state, ::MIR::Function& fcn, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        for(unsigned int block_idx = 0; block_idx < fcn.blocks.size(); block_idx ++)
        {
            auto& block = fcn.blocks[block_idx];
            for(auto& stmt : block.statements)
            {
                state.set_cur_stmt(block_idx, (&stmt - &block.statements.front()));
                visit_mir_lvalues_mut(stmt, cb);
            }
            if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;
            state.set_cur_stmt_term(block_idx);
            visit_mir_lvalues_mut(block.terminator, cb);
        }
    }
    void visit_mir_lvalues(::MIR::TypeResolve& state, const ::MIR::Function& fcn, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        visit_mir_lvalues_mut(state, const_cast<::MIR::Function&>(fcn), [&](auto& lv, auto im){ return cb(lv, im); });
    }

    struct ParamsSet {
        ::HIR::PathParams   impl_params;
        const ::HIR::PathParams*  fcn_params;
        const ::HIR::TypeRef*   self_ty;

        ParamsSet():
            fcn_params(nullptr),
            self_ty(nullptr)
        {}

        t_cb_generic get_cb(const Span& sp) const {
            return monomorphise_type_get_cb(sp, self_ty, &impl_params, fcn_params, nullptr);
        }
    };
    const ::MIR::Function* get_called_mir(const ::MIR::TypeResolve& state, const TransList* list, const ::HIR::Path& path, ParamsSet& params)
    {
        // If a TransList is avaliable, then all referenced functions must be in it.
        if( list )
        {
            auto it = list->m_functions.find(path);
            if( it == list->m_functions.end() )
            {
                MIR_BUG(state, "Enumeration failure - Function " << path << " not in TransList");
            }
            const auto& hir_fcn = *it->second->ptr;
            if( it->second->monomorphised.code ) {
                return &*it->second->monomorphised.code;
            }
            else if( hir_fcn.m_code.m_mir ) {
                MIR_ASSERT(state, hir_fcn.m_params.m_types.empty(), "Enumeration failure - Function had params, but wasn't monomorphised - " << path);
                // TODO: Check for trait methods too?
                return &*hir_fcn.m_code.m_mir;
            }
            else {
                MIR_ASSERT(state, !hir_fcn.m_code, "LowerMIR failure - No MIR but HIR is present?! - " << path);
                // External function (no MIR present)
                return nullptr;
            }
        }

        TU_MATCHA( (path.m_data), (pe),
        (Generic,
            const auto& fcn = state.m_crate.get_function_by_path(state.sp, pe.m_path);
            if( fcn.m_code.m_mir )
            {
                params.fcn_params = &pe.m_params;
                return &*fcn.m_code.m_mir;
            }
            ),
        (UfcsKnown,
            TRACE_FUNCTION_F(path);

            // Obtain trait pointer (for default impl and to know what the item type is)
            const auto& trait_ref = state.m_resolve.m_crate.get_trait_by_path(state.sp, pe.trait.m_path);
            auto trait_vi_it = trait_ref.m_values.find(pe.item);
            MIR_ASSERT(state, trait_vi_it != trait_ref.m_values.end(), "Couldn't find item " << pe.item << " in trait " << pe.trait.m_path);
            const auto& trait_vi = trait_vi_it->second;
            MIR_ASSERT(state, trait_vi.is_Function(), "Item '" << pe.item << " in trait " << pe.trait.m_path << " isn't a function");
            const auto& ve = trait_vi.as_Function();

            bool bound_found = false;
            bool is_spec = false;
            ::std::vector<::HIR::TypeRef>    best_impl_params;
            const ::HIR::TraitImpl* best_impl = nullptr;
            state.m_resolve.find_impl(state.sp, pe.trait.m_path, pe.trait.m_params, *pe.type, [&](auto impl_ref, auto is_fuzz) {
                DEBUG("[get_called_mir] Found " << impl_ref);
                if( ! impl_ref.m_data.is_TraitImpl() ) {
                    MIR_ASSERT(state, best_impl == nullptr, "Generic impl and `impl` block collided");
                    bound_found = true;
                    return true;
                }
                const auto& impl_ref_e = impl_ref.m_data.as_TraitImpl();
                const auto& impl = *impl_ref_e.impl;
                MIR_ASSERT(state, impl.m_trait_args.m_types.size() == pe.trait.m_params.m_types.size(), "Trait parameter count mismatch " << impl.m_trait_args << " vs " << pe.trait.m_params);

                if( best_impl == nullptr || impl.more_specific_than(*best_impl) ) {
                    best_impl = &impl;

                    auto fit = impl.m_methods.find(pe.item);
                    if( fit == impl.m_methods.end() ) {
                        DEBUG("[get_called_mir] Method " << pe.item << " missing in impl " << pe.trait << " for " << *pe.type);
                        return false;
                    }
                    best_impl_params.clear();
                    for(unsigned int i = 0; i < impl_ref_e.params.size(); i ++)
                    {
                        if( impl_ref_e.params[i] )
                            best_impl_params.push_back( impl_ref_e.params[i]->clone() );
                        else if( ! impl_ref_e.params_ph[i].m_data.is_Generic() || impl_ref_e.params_ph[i].m_data.as_Generic().binding >> 8 != 2 )
                            best_impl_params.push_back( impl_ref_e.params_ph[i].clone() );
                        else
                            MIR_BUG(state, "[get_called_mir] Parameter " << i << " unset");
                    }
                    is_spec = fit->second.is_specialisable;
                    return !is_spec;
                }
                return false;
                });

            if( bound_found ) {
                return nullptr;
            }
            MIR_ASSERT(state, best_impl, "Couldn't find an impl for " << path);
            if( is_spec )
            {
                DEBUG(path << " pointed to a specialisable impl, not inlining");
                return nullptr;
            }
            const auto& impl = *best_impl;

            params.self_ty = &*pe.type;
            params.fcn_params = &pe.params;
            // Search for the method in the impl
            auto fit = impl.m_methods.find(pe.item);
            if( fit != impl.m_methods.end() )
            {
                params.impl_params.m_types = mv$(best_impl_params);
                DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                if( fit->second.data.m_code.m_mir )
                    return &*fit->second.data.m_code.m_mir;
            }
            else
            {
                params.impl_params = pe.trait.m_params.clone();
                if( ve.m_code.m_mir )
                    return &*ve.m_code.m_mir;
            }
            return nullptr;
            ),
        (UfcsInherent,
            const ::HIR::TypeImpl* best_impl;
            state.m_resolve.m_crate.find_type_impls(*pe.type, [](const auto&x)->const auto& { return x; }, [&](const auto& impl) {
                DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                // TODO: Specialisation.
                auto fit = impl.m_methods.find(pe.item);
                if( fit != impl.m_methods.end() )
                {
                    best_impl = &impl;
                    return true;
                }
                return false;
                });
            MIR_ASSERT(state, best_impl, "Couldn't find an impl for " << path);
            auto fit = best_impl->m_methods.find(pe.item);
            MIR_ASSERT(state, fit != best_impl->m_methods.end(), "Couldn't find method in best inherent impl");
            if( fit->second.data.m_code.m_mir )
            {
                params.self_ty = &*pe.type;
                params.fcn_params = &pe.params;
                params.impl_params = pe.impl_params.clone();
                return &*fit->second.data.m_code.m_mir;
            }
            return nullptr;
            ),
        (UfcsUnknown,
            MIR_BUG(state, "UfcsUnknown hit - " << path);
            )
        )
        return nullptr;
    }


    void visit_terminator_target_mut(::MIR::Terminator& term, ::std::function<void(::MIR::BasicBlockId&)> cb) {
        TU_MATCHA( (term), (e),
        (Incomplete,
            ),
        (Return,
            ),
        (Diverge,
            ),
        (Goto,
            cb(e);
            ),
        (Panic,
            ),
        (If,
            cb(e.bb0);
            cb(e.bb1);
            ),
        (Switch,
            for(auto& target : e.targets)
                cb(target);
            ),
        (SwitchValue,
            for(auto& target : e.targets)
                cb(target);
            cb(e.def_target);
            ),
        (Call,
            cb(e.ret_block);
            cb(e.panic_block);
            )
        )
    }
    void visit_terminator_target(const ::MIR::Terminator& term, ::std::function<void(const ::MIR::BasicBlockId&)> cb) {
        visit_terminator_target_mut(const_cast<::MIR::Terminator&>(term), cb);
    }

    void visit_blocks_mut(::MIR::TypeResolve& state, ::MIR::Function& fcn, ::std::function<void(::MIR::BasicBlockId, ::MIR::BasicBlock&)> cb)
    {
        ::std::vector<bool> visited( fcn.blocks.size() );
        ::std::vector< ::MIR::BasicBlockId> to_visit;
        to_visit.push_back( 0 );
        while( to_visit.size() > 0 )
        {
            auto bb = to_visit.back(); to_visit.pop_back();
            if( visited[bb] )   continue;
            visited[bb] = true;
            auto& block = fcn.blocks[bb];

            cb(bb, block);

            visit_terminator_target(block.terminator, [&](auto e){
                if( !visited[e] )
                    to_visit.push_back(e);
                });
        }
    }
    void visit_blocks(::MIR::TypeResolve& state, const ::MIR::Function& fcn, ::std::function<void(::MIR::BasicBlockId, const ::MIR::BasicBlock&)> cb) {
        visit_blocks_mut(state, const_cast<::MIR::Function&>(fcn), [cb](auto id, auto& blk){ cb(id, blk); });
    }

    bool statement_invalidates_lvalue(const ::MIR::Statement& stmt, const ::MIR::LValue& lv)
    {
        return visit_mir_lvalues(stmt, [&](const auto& v, auto vu) {
            if( v == lv ) {
                return vu != ValUsage::Read;
            }
            return false;
            });
    }
    bool terminator_invalidates_lvalue(const ::MIR::Terminator& term, const ::MIR::LValue& lv)
    {
        if( const auto* e = term.opt_Call() )
        {
            return visit_mir_lvalue(e->ret_val, ValUsage::Write, [&](const auto& v, auto vu) {
                if( v == lv ) {
                    return vu != ValUsage::Read;
                }
                return false;
                });
        }
        else
        {
            return false;
        }
    }
}

// TODO: Move this block of definitions+code above the namespace above.

bool MIR_Optimise_BlockSimplify(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_Inlining(::MIR::TypeResolve& state, ::MIR::Function& fcn, bool minimal, const TransList* list=nullptr);
bool MIR_Optimise_SplitAggregates(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_PropagateSingleAssignments(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_PropagateKnownValues(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_DeTemporary(::MIR::TypeResolve& state, ::MIR::Function& fcn); // Eliminate useless temporaries
bool MIR_Optimise_UnifyTemporaries(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_CommonStatements(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_UnifyBlocks(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_ConstPropagte(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_DeadDropFlags(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_DeadAssignments(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_NoopRemoval(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_GarbageCollect_Partial(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_GarbageCollect(::MIR::TypeResolve& state, ::MIR::Function& fcn);

/// A minimum set of optimisations:
/// - Inlines `#[inline(always)]` functions
/// - Simplifies the call graph (by removing chained gotos)
/// - Sorts blocks into a rough flow order
void MIR_OptimiseMin(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    static Span sp;
    TRACE_FUNCTION_F(path);
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };

    while( MIR_Optimise_Inlining(state, fcn, true) )
    {
        MIR_Cleanup(resolve, path, fcn, args, ret_type);
        //MIR_Dump_Fcn(::std::cout, fcn);
        #if CHECK_AFTER_ALL
        MIR_Validate(resolve, path, fcn, args, ret_type);
        #endif
    }

    MIR_Optimise_BlockSimplify(state, fcn);
    MIR_Optimise_UnifyBlocks(state, fcn);

    //MIR_Optimise_GarbageCollect_Partial(state, fcn);

    MIR_Optimise_GarbageCollect(state, fcn);
    //MIR_Validate_Full(resolve, path, fcn, args, ret_type);
    MIR_SortBlocks(resolve, path, fcn);

#if CHECK_AFTER_DONE > 1
    MIR_Validate(resolve, path, fcn, args, ret_type);
#endif
    return ;
}
/// Optimise doing inlining then cleaning up the mess
///
/// Returns true if any optimisation was performed
///
/// NOTE: This function can only be called after enumeration and monomorphisation, so it takes the TransList by reference not nullable pointer
bool MIR_OptimiseInline(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type, const TransList& list)
{
    static Span sp;
    bool rv = false;
    TRACE_FUNCTION_FR(path, rv);
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };

    while( MIR_Optimise_Inlining(state, fcn, false, &list) )
    {
        MIR_Cleanup(resolve, path, fcn, args, ret_type);
#if CHECK_AFTER_ALL
        MIR_Validate(resolve, path, fcn, args, ret_type);
#endif
        rv = true;
    }

    if( rv )
    {
        MIR_Optimise_BlockSimplify(state, fcn);
        MIR_Optimise_UnifyBlocks(state, fcn);

        MIR_Optimise_GarbageCollect(state, fcn);
        //MIR_Validate_Full(resolve, path, fcn, args, ret_type);
        MIR_SortBlocks(resolve, path, fcn);

#if CHECK_AFTER_DONE > 1
        MIR_Validate(resolve, path, fcn, args, ret_type);
#endif
    }

    return rv;
}
void MIR_Optimise(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    static Span sp;
    TRACE_FUNCTION_F(path);
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };

    bool change_happened;
    unsigned int pass_num = 0;
    do
    {
        MIR_ASSERT(state, pass_num < 100, "Too many MIR optimisation iterations");

        change_happened = false;
        TRACE_FUNCTION_FR("Pass " << pass_num, change_happened);

        // >> Simplify call graph (removes gotos to blocks with a single use)
        MIR_Optimise_BlockSimplify(state, fcn);

        // >> Apply known constants
        change_happened |= MIR_Optimise_ConstPropagte(state, fcn);
        #if CHECK_AFTER_ALL
        MIR_Validate(resolve, path, fcn, args, ret_type);
        #endif

        // Attempt to remove useless temporaries
        while( MIR_Optimise_DeTemporary(state, fcn) )
        {
            change_happened = true;
        }
#if CHECK_AFTER_ALL
        MIR_Validate(resolve, path, fcn, args, ret_type);
#endif

        // TODO: Split apart aggregates (just tuples?) where it's never used
        // as an aggregate. (Written once, never used directly)
        change_happened |= MIR_Optimise_SplitAggregates(state, fcn);

        // >> Replace values from composites if they're known
        //   - Undoes the inefficiencies from the `match (a, b) { ... }` pattern
        change_happened |= MIR_Optimise_PropagateKnownValues(state, fcn);
#if CHECK_AFTER_ALL
        MIR_Validate(resolve, path, fcn, args, ret_type);
#endif

        // TODO: Convert `&mut *mut_foo` into `mut_foo` if the source is movable and not used afterwards

#if DUMP_BEFORE_ALL || DUMP_BEFORE_PSA
        if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
        // >> Propagate/remove dead assignments
        while( MIR_Optimise_PropagateSingleAssignments(state, fcn) )
            change_happened = true;
#if CHECK_AFTER_ALL
        MIR_Validate(resolve, path, fcn, args, ret_type);
#endif

        // >> Move common statements (assignments) across gotos.
        change_happened |= MIR_Optimise_CommonStatements(state, fcn);

        // >> Combine Duplicate Blocks
        change_happened |= MIR_Optimise_UnifyBlocks(state, fcn);
        // >> Remove assignments of unsed drop flags
        change_happened |= MIR_Optimise_DeadDropFlags(state, fcn);
        // >> Remove assignments that are never read
        change_happened |= MIR_Optimise_DeadAssignments(state, fcn);
        // >> Remove no-op assignments
        change_happened |= MIR_Optimise_NoopRemoval(state, fcn);

        #if CHECK_AFTER_ALL
        MIR_Validate(resolve, path, fcn, args, ret_type);
        #endif

        // >> Inline short functions
        if( !change_happened )
        {
            bool inline_happened = MIR_Optimise_Inlining(state, fcn, false);
            if( inline_happened )
            {
                // Apply cleanup again (as monomorpisation in inlining may have exposed a vtable call)
                MIR_Cleanup(resolve, path, fcn, args, ret_type);
                //MIR_Dump_Fcn(::std::cout, fcn);
                change_happened = true;
            }
            #if CHECK_AFTER_ALL
            MIR_Validate(resolve, path, fcn, args, ret_type);
            #endif
        }

        if( change_happened )
        {
            #if DUMP_AFTER_PASS
            if( debug_enabled() ) {
                MIR_Dump_Fcn(::std::cout, fcn);
            }
            #endif
            #if CHECK_AFTER_PASS && !CHECK_AFTER_ALL
            MIR_Validate(resolve, path, fcn, args, ret_type);
            #endif
        }

        MIR_Optimise_GarbageCollect_Partial(state, fcn);
        pass_num += 1;
    } while( change_happened );

    // Run UnifyTemporaries last, then unify blocks, then run some
    // optimisations that might be affected
    if(MIR_Optimise_UnifyTemporaries(state, fcn))
    {
#if CHECK_AFTER_ALL
        MIR_Validate(resolve, path, fcn, args, ret_type);
#endif
        MIR_Optimise_UnifyBlocks(state, fcn);
        //MIR_Optimise_ConstPropagte(state, fcn);
        MIR_Optimise_NoopRemoval(state, fcn);
    }


    #if DUMP_AFTER_DONE
    if( debug_enabled() ) {
        MIR_Dump_Fcn(::std::cout, fcn);
    }
    #endif
    #if CHECK_AFTER_DONE
    // DEFENCE: Run validation _before_ GC (so validation errors refer to the pre-gc numbers)
    MIR_Validate(resolve, path, fcn, args, ret_type);
    #endif
    // GC pass on blocks and variables
    // - Find unused blocks, then delete and rewrite all references.
    MIR_Optimise_GarbageCollect(state, fcn);

    //MIR_Validate_Full(resolve, path, fcn, args, ret_type);

    MIR_SortBlocks(resolve, path, fcn);
#if CHECK_AFTER_DONE > 1
    MIR_Validate(resolve, path, fcn, args, ret_type);
#endif
}

// --------------------------------------------------------------------
// Performs basic simplications on the call graph (merging/removing blocks)
// --------------------------------------------------------------------
bool MIR_Optimise_BlockSimplify(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    // >> Replace targets that point to a block that is just a goto
    for(auto& block : fcn.blocks)
    {
        // Unify sequential ScopeEnd statements
        if( block.statements.size() > 1 )
        {
            for(auto it = block.statements.begin() + 1; it != block.statements.end(); )
            {
                if( (it-1)->is_ScopeEnd() && it->is_ScopeEnd() )
                {
                    auto& dst = (it-1)->as_ScopeEnd();
                    const auto& src = it->as_ScopeEnd();
                    DEBUG("Unify " << *(it-1) << " and " << *it);
                    for(auto v : src.slots)
                        dst.slots.push_back(v);
                    ::std::sort(dst.slots.begin(), dst.slots.end());
                    it = block.statements.erase(it);
                }
                else
                {
                    ++ it;
                }
            }
        }

        visit_terminator_target_mut(block.terminator, [&](auto& e) {
            if( &fcn.blocks[e] != &block )
                e = get_new_target(state, e);
            });
    }

    // >> Merge blocks where a block goto-s to a single-use block.
    {
        ::std::vector<bool> visited( fcn.blocks.size() );
        ::std::vector<unsigned int> uses( fcn.blocks.size() );
        ::std::vector< ::MIR::BasicBlockId> to_visit;
        to_visit.push_back( 0 );
        uses[0] ++;
        while( to_visit.size() > 0 )
        {
            auto bb = to_visit.back(); to_visit.pop_back();
            if( visited[bb] )
                continue ;
            visited[bb] = true;
            const auto& block = fcn.blocks[bb];

            visit_terminator_target(block.terminator, [&](const auto& e) {
                if( !visited[e] )   to_visit.push_back(e);
                uses[e] ++;
                });
        }

        unsigned int i = 0;
        for(auto& block : fcn.blocks)
        {
            if( visited[i] )
            {
                while( block.terminator.is_Goto() )
                {
                    auto tgt = block.terminator.as_Goto();
                    if( uses[tgt] != 1 )
                        break ;
                    if( tgt == i )
                        break;
                    DEBUG("Append bb " << tgt << " to bb" << i);

                    assert( &fcn.blocks[tgt] != &block );
                    // Move contents of source block, then set the TAGDEAD terminator to Incomplete
                    auto src_block = mv$(fcn.blocks[tgt]);
                    fcn.blocks[tgt].terminator = ::MIR::Terminator::make_Incomplete({});

                    for(auto& stmt : src_block.statements)
                        block.statements.push_back( mv$(stmt) );
                    block.terminator = mv$( src_block.terminator );
                }
            }
            i ++;
        }
    }

    // NOTE: Not strictly true, but these can't trigger other optimisations
    return false;
}


// --------------------------------------------------------------------
// If two temporaries don't overlap in lifetime (blocks in which they're valid), unify the two
// --------------------------------------------------------------------
bool MIR_Optimise_Inlining(::MIR::TypeResolve& state, ::MIR::Function& fcn, bool minimal, const TransList* list/*=nullptr*/)
{
    bool inline_happened = false;
    TRACE_FUNCTION_FR("", inline_happened);

    struct H
    {
        static bool can_inline(const ::HIR::Path& path, const ::MIR::Function& fcn, bool minimal)
        {
            // TODO: If the function is marked as `inline(always)`, then inline it regardless of the contents

            if( minimal ) {
                return false;
            }

            // TODO: If the function is marked as `inline(never)`, then don't inline

            // TODO: Allow functions that are just a switch on an input.
            if( fcn.blocks.size() == 1 )
            {
                return fcn.blocks[0].statements.size() < 10 && ! fcn.blocks[0].terminator.is_Goto();
            }
            else if( fcn.blocks.size() == 3 && fcn.blocks[0].terminator.is_Call() )
            {
                const auto& blk0_te = fcn.blocks[0].terminator.as_Call();
                if( !(fcn.blocks[1].terminator.is_Diverge() || fcn.blocks[1].terminator.is_Return()) )
                    return false;
                if( !(fcn.blocks[2].terminator.is_Diverge() || fcn.blocks[2].terminator.is_Return()) )
                    return false;
                if( fcn.blocks[0].statements.size() + fcn.blocks[1].statements.size() + fcn.blocks[2].statements.size() > 10 )
                    return false;
                // Detect and avoid simple recursion.
                // - This won't detect mutual recursion - that also needs prevention.
                if( blk0_te.fcn.is_Path() && blk0_te.fcn.as_Path() == path )
                    return false;
                return true;
            }
            else if( fcn.blocks.size() > 1 && fcn.blocks[0].terminator.is_Switch() )
            {
                // Setup + Arms + Return + Panic
                // - Handles the atomit wrappers
                if( fcn.blocks.size() != fcn.blocks[0].terminator.as_Switch().targets.size()+3 )
                    return false;
                // TODO: Check for the parameter being a Constant?
                for(size_t i = 1; i < fcn.blocks.size(); i ++)
                {
                    if( fcn.blocks[i].terminator.is_Call() )
                    {
                        const auto& te = fcn.blocks[i].terminator.as_Call();
                        // Recursion, don't inline.
                        if( te.fcn.is_Path() && te.fcn.as_Path() == path )
                            return false;
                    }
                }
                return true;
            }
            else
            {
                return false;
            }
        }
    };
    struct Cloner
    {
        const Span& sp;
        const ::StaticTraitResolve& resolve;
        const ::MIR::Terminator::Data_Call& te;
        ::std::vector<unsigned> copy_args;  // Local indexes containing copies of Copy args
        ParamsSet   params;
        unsigned int bb_base = ~0u;
        unsigned int tmp_base = ~0u;
        unsigned int var_base = ~0u;
        unsigned int df_base = ~0u;

        size_t tmp_end = 0;
        mutable ::std::vector< ::MIR::Param >  const_assignments;

        ::MIR::LValue   retval;

        Cloner(const Span& sp, const ::StaticTraitResolve& resolve, ::MIR::Terminator::Data_Call& te):
            sp(sp),
            resolve(resolve),
            te(te),
            copy_args(te.args.size(), ~0u)
        {
        }

        ::HIR::TypeRef monomorph(const ::HIR::TypeRef& ty) const {
            TRACE_FUNCTION_F(ty);
            auto rv = monomorphise_type_with(sp, ty, params.get_cb(sp));
            resolve.expand_associated_types(sp, rv);
            return rv;
        }
        ::HIR::GenericPath monomorph(const ::HIR::GenericPath& ty) const {
            TRACE_FUNCTION_F(ty);
            auto rv = monomorphise_genericpath_with(sp, ty, params.get_cb(sp), false);
            for(auto& arg : rv.m_params.m_types)
                resolve.expand_associated_types(sp, arg);
            return rv;
        }
        ::HIR::Path monomorph(const ::HIR::Path& ty) const {
            TRACE_FUNCTION_F(ty);
            auto rv = monomorphise_path_with(sp, ty, params.get_cb(sp), false);
            TU_MATCH(::HIR::Path::Data, (rv.m_data), (e2),
            (Generic,
                for(auto& arg : e2.m_params.m_types)
                    resolve.expand_associated_types(sp, arg);
                ),
            (UfcsInherent,
                resolve.expand_associated_types(sp, *e2.type);
                for(auto& arg : e2.params.m_types)
                    resolve.expand_associated_types(sp, arg);
                // TODO: impl params too?
                for(auto& arg : e2.impl_params.m_types)
                    resolve.expand_associated_types(sp, arg);
                ),
            (UfcsKnown,
                resolve.expand_associated_types(sp, *e2.type);
                for(auto& arg : e2.trait.m_params.m_types)
                    resolve.expand_associated_types(sp, arg);
                for(auto& arg : e2.params.m_types)
                    resolve.expand_associated_types(sp, arg);
                ),
            (UfcsUnknown,
                BUG(sp, "Encountered UfcsUnknown");
                )
            )
            return rv;
        }
        ::HIR::PathParams monomorph(const ::HIR::PathParams& ty) const {
            TRACE_FUNCTION_F(ty);
            auto rv = monomorphise_path_params_with(sp, ty, params.get_cb(sp), false);
            for(auto& arg : rv.m_types)
                resolve.expand_associated_types(sp, arg);
            return rv;
        }

        ::MIR::BasicBlock clone_bb(const ::MIR::BasicBlock& src, unsigned src_idx, unsigned new_idx) const
        {
            ::MIR::BasicBlock   rv;
            rv.statements.reserve( src.statements.size() );
            for(const auto& stmt : src.statements)
            {
                DEBUG("BB" << src_idx << "->BB" << new_idx << "/" << rv.statements.size() << ": " << stmt);
                TU_MATCHA( (stmt), (se),
                (Assign,
                    rv.statements.push_back( ::MIR::Statement::make_Assign({
                        this->clone_lval(se.dst),
                        this->clone_rval(se.src)
                        }) );
                    ),
                (Asm,
                    rv.statements.push_back( ::MIR::Statement::make_Asm({
                        se.tpl,
                        this->clone_name_lval_vec(se.outputs),
                        this->clone_name_lval_vec(se.inputs),
                        se.clobbers,
                        se.flags
                        }) );
                    ),
                (SetDropFlag,
                    rv.statements.push_back( ::MIR::Statement::make_SetDropFlag({
                        this->df_base + se.idx,
                        se.new_val,
                        se.other == ~0u ? ~0u : this->df_base + se.other
                        }) );
                    ),
                (Drop,
                    rv.statements.push_back( ::MIR::Statement::make_Drop({
                        se.kind,
                        this->clone_lval(se.slot),
                        se.flag_idx == ~0u ? ~0u : this->df_base + se.flag_idx
                        }) );
                    ),
                (ScopeEnd,
                    ::MIR::Statement::Data_ScopeEnd new_se;
                    new_se.slots.reserve(se.slots.size());
                    for(auto idx : se.slots)
                        new_se.slots.push_back(this->var_base + idx);
                    rv.statements.push_back(::MIR::Statement( mv$(new_se) ));
                    )
                )
                DEBUG("-> " << rv.statements.back());
            }
            DEBUG("BB" << src_idx << "->BB" << new_idx << "/" << rv.statements.size() << ": " << src.terminator);
            if(src.terminator.is_Return())
            {
                rv.statements.push_back(::MIR::Statement::make_Assign({ this->te.ret_val.clone(), this->retval.clone() }));
                DEBUG("++ " << rv.statements.back());
            }
            rv.terminator = this->clone_term(src.terminator);
            DEBUG("-> " << rv.terminator);
            return rv;
        }
        ::MIR::Terminator clone_term(const ::MIR::Terminator& src) const
        {
            TU_MATCHA( (src), (se),
            (Incomplete,
                return ::MIR::Terminator::make_Incomplete({});
                ),
            (Return,
                return ::MIR::Terminator::make_Goto(this->te.ret_block);
                ),
            (Diverge,
                return ::MIR::Terminator::make_Goto(this->te.panic_block);
                ),
            (Panic,
                return ::MIR::Terminator::make_Panic({});
                ),
            (Goto,
                return ::MIR::Terminator::make_Goto(se + this->bb_base);
                ),
            (If,
                return ::MIR::Terminator::make_If({
                    this->clone_lval(se.cond),
                    se.bb0 + this->bb_base,
                    se.bb1 + this->bb_base
                    });
                ),
            (Switch,
                ::std::vector<::MIR::BasicBlockId>  arms;
                arms.reserve(se.targets.size());
                for(const auto& bbi : se.targets)
                    arms.push_back( bbi + this->bb_base );
                return ::MIR::Terminator::make_Switch({ this->clone_lval(se.val), mv$(arms) });
                ),
            (SwitchValue,
                ::std::vector<::MIR::BasicBlockId>  arms;
                arms.reserve(se.targets.size());
                for(const auto& bbi : se.targets)
                    arms.push_back( bbi + this->bb_base );
                return ::MIR::Terminator::make_SwitchValue({ this->clone_lval(se.val), se.def_target + this->bb_base, mv$(arms), se.values.clone() });
                ),
            (Call,
                ::MIR::CallTarget   tgt;
                TU_MATCHA( (se.fcn), (ste),
                (Value,
                    tgt = ::MIR::CallTarget::make_Value( this->clone_lval(ste) );
                    ),
                (Path,
                    tgt = ::MIR::CallTarget::make_Path( this->monomorph(ste) );
                    ),
                (Intrinsic,
                    tgt = ::MIR::CallTarget::make_Intrinsic({ ste.name, this->monomorph(ste.params) });
                    )
                )
                return ::MIR::Terminator::make_Call({
                    this->bb_base + se.ret_block,
                    this->bb_base + se.panic_block,
                    this->clone_lval(se.ret_val),
                    mv$(tgt),
                    this->clone_param_vec(se.args)
                    });
                )
            )
            throw "";
        }
        ::std::vector< ::std::pair<::std::string,::MIR::LValue> > clone_name_lval_vec(const ::std::vector< ::std::pair<::std::string,::MIR::LValue> >& src) const
        {
            ::std::vector< ::std::pair<::std::string,::MIR::LValue> >  rv;
            rv.reserve(src.size());
            for(const auto& e : src)
                rv.push_back(::std::make_pair(e.first, this->clone_lval(e.second)));
            return rv;
        }
        ::std::vector<::MIR::LValue> clone_lval_vec(const ::std::vector<::MIR::LValue>& src) const
        {
            ::std::vector<::MIR::LValue>    rv;
            rv.reserve(src.size());
            for(const auto& lv : src)
                rv.push_back( this->clone_lval(lv) );
            return rv;
        }
        ::std::vector<::MIR::Param> clone_param_vec(const ::std::vector<::MIR::Param>& src) const
        {
            ::std::vector<::MIR::Param>    rv;
            rv.reserve(src.size());
            for(const auto& lv : src)
                rv.push_back( this->clone_param(lv) );
            return rv;
        }

        ::MIR::LValue clone_lval(const ::MIR::LValue& src) const
        {
            TU_MATCHA( (src), (se),
            (Return,
                return this->retval.clone();
                ),
            (Argument,
                const auto& arg = this->te.args.at(se.idx);
                if( this->copy_args[se.idx] != ~0u )
                {
                    return ::MIR::LValue::make_Local(this->copy_args[se.idx]);
                }
                else
                {
                    assert( !arg.is_Constant() );   // Should have been handled in the above
                    return arg.as_LValue().clone();
                }
                ),
            (Local,
                return ::MIR::LValue::make_Local(this->var_base + se);
                ),
            (Static,
                return this->monomorph( se );
                ),
            (Deref,
                return ::MIR::LValue::make_Deref({ box$(this->clone_lval(*se.val)) });
                ),
            (Field,
                return ::MIR::LValue::make_Field({ box$(this->clone_lval(*se.val)), se.field_index });
                ),
            (Index,
                return ::MIR::LValue::make_Index({
                    box$(this->clone_lval(*se.val)),
                    box$(this->clone_lval(*se.idx))
                    });
                ),
            (Downcast,
                return ::MIR::LValue::make_Downcast({ box$(this->clone_lval(*se.val)), se.variant_index });
                )
            )
            throw "";
        }
        ::MIR::Constant clone_constant(const ::MIR::Constant& src) const
        {
            TU_MATCHA( (src), (ce),
            (Int  , return ::MIR::Constant(ce);),
            (Uint , return ::MIR::Constant(ce);),
            (Float, return ::MIR::Constant(ce);),
            (Bool , return ::MIR::Constant(ce);),
            (Bytes, return ::MIR::Constant(ce);),
            (StaticString, return ::MIR::Constant(ce);),
            (Const,
                return ::MIR::Constant::make_Const({ this->monomorph(ce.p) });
                ),
            (ItemAddr,
                return ::MIR::Constant::make_ItemAddr(this->monomorph(ce));
                )
            )
            throw "";
        }
        ::MIR::Param clone_param(const ::MIR::Param& src) const
        {
            TU_MATCHA( (src), (se),
            (LValue,
                // NOTE: No need to use `copy_args` here as all uses of Param are copies/moves
                //if( const auto* ae = se.opt_Argument() )
                //    return this->te.args.at(ae->idx).clone();
                return clone_lval(se);
                ),
            (Constant, return clone_constant(se); )
            )
            throw "";
        }
        ::MIR::RValue clone_rval(const ::MIR::RValue& src) const
        {
            TU_MATCHA( (src), (se),
            (Use,
                //if( const auto* ae = se.opt_Argument() )
                //    if( const auto* e = this->te.args.at(ae->idx).opt_Constant() )
                //        return e->clone();
                return ::MIR::RValue( this->clone_lval(se) );
                ),
            (Constant,
                return this->clone_constant(se);
                ),
            (SizedArray,
                return ::MIR::RValue::make_SizedArray({ this->clone_param(se.val), se.count });
                ),
            (Borrow,
                // TODO: Region IDs
                return ::MIR::RValue::make_Borrow({ se.region, se.type, this->clone_lval(se.val) });
                ),
            (Cast,
                return ::MIR::RValue::make_Cast({ this->clone_lval(se.val), this->monomorph(se.type) });
                ),
            (BinOp,
                return ::MIR::RValue::make_BinOp({ this->clone_param(se.val_l), se.op, this->clone_param(se.val_r) });
                ),
            (UniOp,
                return ::MIR::RValue::make_UniOp({ this->clone_lval(se.val), se.op });
                ),
            (DstMeta,
                return ::MIR::RValue::make_DstMeta({ this->clone_lval(se.val) });
                ),
            (DstPtr,
                return ::MIR::RValue::make_DstPtr({ this->clone_lval(se.val) });
                ),
            (MakeDst,
                return ::MIR::RValue::make_MakeDst({ this->clone_param(se.ptr_val), this->clone_param(se.meta_val) });
                ),
            (Tuple,
                return ::MIR::RValue::make_Tuple({ this->clone_param_vec(se.vals) });
                ),
            (Array,
                return ::MIR::RValue::make_Array({ this->clone_param_vec(se.vals) });
                ),
            (Variant,
                return ::MIR::RValue::make_Variant({ this->monomorph(se.path), se.index, this->clone_param(se.val) });
                ),
            (Struct,
                return ::MIR::RValue::make_Struct({ this->monomorph(se.path), this->clone_param_vec(se.vals) });
                )
            )
            throw "";
        }
    };

    for(unsigned int i = 0; i < fcn.blocks.size(); i ++)
    {
        state.set_cur_stmt_term(i);
        if(auto* te = fcn.blocks[i].terminator.opt_Call())
        {
            if( ! te->fcn.is_Path() )
                continue ;
            const auto& path = te->fcn.as_Path();

            Cloner  cloner { state.sp, state.m_resolve, *te };
            const auto* called_mir = get_called_mir(state, list, path,  cloner.params);
            if( !called_mir )
                continue ;
            if( called_mir == &fcn )
            {
                DEBUG("Can't inline - recursion");
                continue ;
            }

            // Check the size of the target function.
            // Inline IF:
            // - First BB ends with a call and total count is 3
            // - Statement count smaller than 10
            if( ! H::can_inline(path, *called_mir, minimal) )
            {
                DEBUG("Can't inline " << path);
                continue ;
            }
            DEBUG(state << fcn.blocks[i].terminator);
            TRACE_FUNCTION_F("Inline " << path);

            // Allocate a temporary for the return value
            {
                cloner.retval = ::MIR::LValue::make_Local( fcn.locals.size() );
                DEBUG("- Storing return value in " << cloner.retval);
                ::HIR::TypeRef  tmp_ty;
                fcn.locals.push_back( state.get_lvalue_type(tmp_ty, te->ret_val).clone() );
                //fcn.local_names.push_back( "" );
            }

            // Monomorph locals and append
            cloner.var_base = fcn.locals.size();
            for(const auto& ty : called_mir->locals)
                fcn.locals.push_back( cloner.monomorph(ty) );
            cloner.tmp_end = fcn.locals.size();

            cloner.df_base = fcn.drop_flags.size();
            fcn.drop_flags.insert( fcn.drop_flags.end(), called_mir->drop_flags.begin(), called_mir->drop_flags.end() );
            cloner.bb_base = fcn.blocks.size();

            // Store all Copy lvalue arguments and Constants in variables
            for(size_t i = 0; i < te->args.size(); i++)
            {
                const auto& a = te->args[i];
                if( !a.is_LValue() || state.lvalue_is_copy(a.as_LValue()) )
                {
                    cloner.copy_args[i] = cloner.tmp_end + cloner.const_assignments.size();
                    cloner.const_assignments.push_back( a.clone() );
                    DEBUG("- Taking a copy of arg " << i << " (" << a << ") in Local(" << cloner.copy_args[i] << ")");
                }
            }

            // Append monomorphised copy of all blocks.
            // > Arguments replaced by input lvalues
            ::std::vector<::MIR::BasicBlock>    new_blocks;
            new_blocks.reserve( called_mir->blocks.size() );
            for(const auto& bb : called_mir->blocks)
            {
                new_blocks.push_back( cloner.clone_bb(bb, (&bb - called_mir->blocks.data()), fcn.blocks.size() + new_blocks.size()) );
            }

            // > Append new temporaries
            DEBUG("- Insert argument lval assignments");
            for(auto& val : cloner.const_assignments)
            {
                ::HIR::TypeRef  tmp;
                auto ty = val.is_Constant() ? state.get_const_type(val.as_Constant()) : state.get_lvalue_type(tmp, val.as_LValue()).clone();
                auto lv = ::MIR::LValue::make_Local( static_cast<unsigned>(fcn.locals.size()) );
                fcn.locals.push_back( mv$(ty) );
                auto rval = val.is_Constant() ? ::MIR::RValue(mv$(val.as_Constant())) : ::MIR::RValue( mv$(val.as_LValue()) );
                auto stmt = ::MIR::Statement::make_Assign({ mv$(lv), mv$(rval) });
                DEBUG("++ " << stmt);
                new_blocks[0].statements.insert( new_blocks[0].statements.begin(), mv$(stmt) );
            }
            cloner.const_assignments.clear();

            // Apply
            DEBUG("- Append new blocks");
            fcn.blocks.reserve( fcn.blocks.size() + new_blocks.size() );
            for(auto& b : new_blocks)
            {
                fcn.blocks.push_back( mv$(b) );
            }
            fcn.blocks[i].terminator = ::MIR::Terminator::make_Goto( cloner.bb_base );
            inline_happened = true;
        }
    }
    return inline_happened;
}

// --------------------------------------------------------------------
// Replaces uses of stack slots with what they were assigned with (when
// possible)
// --------------------------------------------------------------------
bool MIR_Optimise_DeTemporary(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);

    for(unsigned int bb_idx = 0; bb_idx < fcn.blocks.size(); bb_idx ++)
    {
        auto& bb = fcn.blocks[bb_idx];
        ::std::map<unsigned,unsigned>   local_assignments;  // Local number -> statement index
        ::std::vector<unsigned> statements_to_remove;   // List of statements that have to be removed

        // ----- Helper closures -----
        // > Check if a recorded assignment is no longer valid.
        auto cb_check_invalidate = [&](const ::MIR::LValue& lv, ValUsage vu) {
                for(auto it = local_assignments.begin(); it != local_assignments.end(); )
                {
                    bool invalidated = false;
                    const auto& src_rvalue = bb.statements[it->second].as_Assign().src;

                    // Destination invalidated?
                    if( lv.is_Local() && it->first == lv.as_Local() )
                    {
                        switch(vu)
                        {
                        case ValUsage::Borrow:
                        case ValUsage::Write:
                            DEBUG(state << "> Mutate/Borrowed " << lv);
                            invalidated = true;
                            break;
                        default:
                            break;
                        }
                    }
                    // Source invalidated?
                    else
                    {
                        switch(vu)
                        {
                        case ValUsage::Borrow:  // Borrows are annoying, assume they invalidate anything used
                        case ValUsage::Write:   // Mutated? It's invalidated
                        case ValUsage::Move:    // Moved? Now invalid
                            visit_mir_lvalues(src_rvalue, [&](const auto& s_lv, auto /*s_vu*/) {
                                if( s_lv == lv )
                                {
                                    DEBUG(state << "> Invalidates source of Local(" << it->first << ") - " << src_rvalue);
                                    invalidated = true;
                                    return true;
                                }
                                return false;
                                });
                            break;
                        case ValUsage::Read:    // Read is Ok
                            break;
                        }
                    }

                    if( invalidated )
                    {
                        it = local_assignments.erase(it);
                    }
                    else
                    {
                        ++ it;
                    }
                }
                return false;
            };
            // ^^^ Check for invalidations
        auto cb_apply_replacements = [&](auto& top_lv, auto top_usage) {
            // NOTE: Visits only the top-level LValues
            // - The inner `visit_mir_lvalue_mut` handles sub-values

            // TODO: Handle partial moves (only delete assignment if the value is fully used)
            // > For now, don't do the replacement if it would delete the assignment UNLESS it's directly being used)

            // 2. Search for replacements
            bool top_level = true;
            visit_mir_lvalue_mut(top_lv, top_usage, [&](auto& ilv, auto /*i_usage*/) {
                if( ilv.is_Local() )
                {
                    auto it = local_assignments.find(ilv.as_Local());
                    if( it != local_assignments.end() )
                    {
                        // - Copy? All is good.
                        if( state.lvalue_is_copy(ilv) )
                        {
                            ilv = bb.statements[it->second].as_Assign().src.as_Use().clone();
                            DEBUG(state << "> Replace (and keep) Local(" << it->first << ") with " << ilv);
                        }
                        // - Top-level (directly used) also good.
                        else if( top_level && top_usage == ValUsage::Move )
                        {
                            // TODO: DstMeta/DstPtr _doesn't_ move, so shouldn't trigger this.
                            ilv = bb.statements[it->second].as_Assign().src.as_Use().clone();
                            DEBUG(state << "> Replace (and remove) Local(" << it->first << ") with " << ilv);
                            statements_to_remove.push_back( it->second );
                            local_assignments.erase(it);
                        }
                        // - Otherwise, remove the record.
                        else
                        {
                            DEBUG(state << "> Non-copy value used within a LValue, remove record of Local(" << it->first << ")");
                            local_assignments.erase(it);
                        }
                    }
                }
                top_level = false;
                return false;
                });
            // Return true to prevent recursion
            return true;
            };

        // ----- Top-level algorithm ------
        // - Find expressions matching the pattern `Local(N) = Use(...)`
        //  > Delete entry when destination is mutated
        //  > Delete entry when source is mutated or invalidated (moved)
        for(unsigned int stmt_idx = 0; stmt_idx < bb.statements.size(); stmt_idx ++ )
        {
            auto& stmt = bb.statements[stmt_idx];
            state.set_cur_stmt(bb_idx, stmt_idx);
            DEBUG(state << stmt);

            // - Check if this statement mutates or borrows a recorded local
            //  > (meaning that the slot isn't a temporary)
            // - Check if this statement mutates or moves the source
            //  > (thus making it invalid to move the source forwards)
            visit_mir_lvalues(stmt, cb_check_invalidate);

            // - Apply known relacements
            visit_mir_lvalues_mut(stmt, cb_apply_replacements);

            // - Check if this is a new assignment
            if( stmt.is_Assign() && stmt.as_Assign().dst.is_Local() && stmt.as_Assign().src.is_Use() )
            {
                if( visit_mir_lvalue(stmt.as_Assign().src.as_Use(), ValUsage::Read, [&](const auto& lv, auto /*vu*/) {
                        return lv == stmt.as_Assign().dst;
                        }) )
                {
                    DEBUG(state << "> Don't record, self-referrential");
                }
                else
                {
                    local_assignments.insert(::std::make_pair( stmt.as_Assign().dst.as_Local(), stmt_idx ));
                    DEBUG(state << "> Record assignment");
                }
            }
        } // for(stmt in bb.statements)

        // TERMINATOR
        state.set_cur_stmt_term(bb_idx);
        DEBUG(state << bb.terminator);
        // > Check for invalidations (e.g. move of a source value)
        visit_mir_lvalues(bb.terminator, cb_check_invalidate);
        // > THEN check for replacements
        if( ! bb.terminator.is_Switch() )
        {
            visit_mir_lvalues_mut(bb.terminator, cb_apply_replacements);
        }

        // Remove assignments
        ::std::sort(statements_to_remove.begin(), statements_to_remove.end());
        while(!statements_to_remove.empty())
        {
            // TODO: Handle partial moves here?
            // TODO: Is there some edge case I'm missing where the assignment shouldn't be removed?
            // > It isn't removed if it's used as a Copy, so that's not a problem.
            bb.statements.erase( bb.statements.begin() + statements_to_remove.back() );
            statements_to_remove.pop_back();
        }
    }

    return changed;
}


// --------------------------------------------------------------------
// Detect common statements between all source arms of a block
// --------------------------------------------------------------------
bool MIR_Optimise_CommonStatements(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);

    for(size_t bb_idx = 0; bb_idx < fcn.blocks.size(); bb_idx ++)
    {
        state.set_cur_stmt(bb_idx, 0);

        bool skip = false;
        ::std::vector<size_t>   sources;
        // Find source blocks
        for(size_t bb2_idx = 0; bb2_idx < fcn.blocks.size() && !skip; bb2_idx ++)
        {
            const auto& blk = fcn.blocks[bb2_idx];
            // TODO: Handle non-Goto branches? (e.g. calls)
            if( blk.terminator.is_Goto() && blk.terminator.as_Goto() == bb_idx )
            {
                if( blk.statements.empty() )
                {
                    DEBUG(state << " BB" << bb2_idx << " empty");
                    skip = true;
                    break ;
                }
                if( !sources.empty() )
                {
                    if( blk.statements.back() != fcn.blocks[sources.front()].statements.back() )
                    {
                        DEBUG(state << " BB" << bb2_idx << " doesn't end with " << fcn.blocks[sources.front()].statements.back() << " instead " << blk.statements.back());
                        skip = true;
                        break;
                    }
                }
                sources.push_back(bb2_idx);
            }
            else
            {
                visit_terminator_target(blk.terminator, [&](const auto& dst_idx) {
                    // If this terminator points to the current BB, don't attempt to merge
                    if( dst_idx == bb_idx ) {
                        DEBUG(state << " BB" << bb2_idx << " doesn't end Goto - instead " << blk.terminator);
                        skip = true;
                    }
                });
            }
        }

        if( !skip && sources.size() > 1 )
        {
            // TODO: Should this search for any common statements?
            
            // Found a common assignment, add to the start and remove from sources.
            auto stmt = ::std::move(fcn.blocks[sources.front()].statements.back());
            MIR_DEBUG(state, "Move common final statements from " << sources << " to " << bb_idx << " - " << stmt);
            for(auto idx : sources)
            {
                fcn.blocks[idx].statements.pop_back();
            }
            fcn.blocks[bb_idx].statements.insert(fcn.blocks[bb_idx].statements.begin(), ::std::move(stmt));
        }
    }
    return changed;
}


// --------------------------------------------------------------------
// If two temporaries don't overlap in lifetime (blocks in which they're valid), unify the two
// --------------------------------------------------------------------
bool MIR_Optimise_UnifyTemporaries(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool replacement_needed = false;
    TRACE_FUNCTION_FR("", replacement_needed);
    ::std::vector<bool> replacable( fcn.locals.size() );
    // 1. Enumerate which (if any) temporaries share the same type
    {
        unsigned int n_found = 0;
        for(unsigned int tmpidx = 0; tmpidx < fcn.locals.size(); tmpidx ++)
        {
            if( replacable[tmpidx] )
                continue ;
            for(unsigned int i = tmpidx+1; i < fcn.locals.size(); i ++ )
            {
                if( replacable[i] )
                    continue ;
                if( fcn.locals[i] == fcn.locals[tmpidx] )
                {
                    replacable[i] = true;
                    replacable[tmpidx] = true;
                    n_found ++;
                }
            }
        }
        if( n_found == 0 )
            return false;
    }

    // TODO: Only calculate lifetimes for replacable locals
    auto lifetimes = MIR_Helper_GetLifetimes(state, fcn, /*dump_debug=*/true, /*mask=*/&replacable);
    ::std::vector<::MIR::ValueLifetime>  slot_lifetimes = mv$(lifetimes.m_slots);

    // 2. Unify variables of the same type with distinct non-overlapping lifetimes
    ::std::map<unsigned int, unsigned int> replacements;
    ::std::vector<bool> visited( fcn.locals.size() );
    for(unsigned int local_idx = 0; local_idx < fcn.locals.size(); local_idx ++)
    {
        if( ! replacable[local_idx] )  continue ;
        if( visited[local_idx] )   continue ;
        if( ! slot_lifetimes[local_idx].is_used() )  continue ;
        visited[local_idx] = true;

        for(unsigned int i = local_idx+1; i < fcn.locals.size(); i ++)
        {
            if( !replacable[i] )
                continue ;
            if( fcn.locals[i] != fcn.locals[local_idx] )
                continue ;
            if( ! slot_lifetimes[i].is_used() )
                continue ;
            // Variables are of the same type, check if they overlap
            if( slot_lifetimes[local_idx].overlaps( slot_lifetimes[i] ) )
                continue ;
            // They don't overlap, unify
            slot_lifetimes[local_idx].unify( slot_lifetimes[i] );
            replacements[i] = local_idx;
            replacement_needed = true;
            visited[i] = true;
        }
    }

    if( replacement_needed )
    {
        DEBUG("Replacing temporaries using {" << replacements << "}");
        visit_mir_lvalues_mut(state, fcn, [&](auto& lv, auto ) {
            if( auto* ve = lv.opt_Local() ) {
                auto it = replacements.find(*ve);
                if( it != replacements.end() )
                {
                    MIR_DEBUG(state, lv << " => Local(" << it->second << ")");
                    *ve = it->second;
                    return true;
                }
            }
            return false;
            });

        // TODO: Replace in ScopeEnd too?
    }

    return replacement_needed;
}

// --------------------------------------------------------------------
// Combine identical blocks
// --------------------------------------------------------------------
bool MIR_Optimise_UnifyBlocks(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);
    struct H {
        static bool blocks_equal(const ::MIR::BasicBlock& a, const ::MIR::BasicBlock& b) {
            if( a.statements.size() != b.statements.size() )
                return false;
            for(unsigned int i = 0; i < a.statements.size(); i ++)
            {
                if( a.statements[i].tag() != b.statements[i].tag() )
                    return false;
                TU_MATCHA( (a.statements[i], b.statements[i]), (ae, be),
                (Assign,
                    if( ae.dst != be.dst )
                        return false;
                    if( ae.src != be.src )
                        return false;
                    ),
                (Asm,
                    if( ae.tpl != be.tpl )
                        return false;
                    if( ae.outputs != be.outputs )
                        return false;
                    if( ae.inputs != be.inputs )
                        return false;
                    if( ae.clobbers != be.clobbers )
                        return false;
                    if( ae.flags != be.flags )
                        return false;
                    ),
                (SetDropFlag,
                    if( ae.idx != be.idx )
                        return false;
                    if( ae.new_val != be.new_val )
                        return false;
                    if( ae.other != be.other )
                        return false;
                    ),
                (Drop,
                    if( ae.kind != be.kind )
                        return false;
                    if( ae.flag_idx != be.flag_idx )
                        return false;
                    if( ae.slot != be.slot )
                        return false;
                    ),
                (ScopeEnd,
                    if( ae.slots != be.slots )
                        return false;
                    )
                )
            }
            if( a.terminator.tag() != b.terminator.tag() )
                return false;
            TU_MATCHA( (a.terminator, b.terminator), (ae, be),
            (Incomplete,
                ),
            (Return,
                ),
            (Diverge,
                ),
            (Goto,
                if( ae != be )
                    return false;
                ),
            (Panic,
                if( ae.dst != be.dst )
                    return false;
                ),
            (If,
                if( ae.cond != be.cond )
                    return false;
                if( ae.bb0 != be.bb0 )
                    return false;
                if( ae.bb1 != be.bb1 )
                    return false;
                ),
            (Switch,
                if( ae.val != be.val )
                    return false;
                if( ae.targets != be.targets )
                    return false;
                ),
            (SwitchValue,
                if( ae.val != be.val )
                    return false;
                if( ae.targets != be.targets )
                    return false;
                if( ae.def_target != be.def_target )
                    return false;
                if( ae.values.tag() != be.values.tag() )
                    return false;
                TU_MATCHA( (ae.values, be.values), (ae2, be2),
                (Unsigned,
                    if( ae2 != be2 )
                        return false;
                    ),
                (Signed,
                    if( ae2 != be2 )
                        return false;
                    ),
                (String,
                    if( ae2 != be2 )
                        return false;
                    )
                )
                ),
            (Call,
                if( ae.ret_block != be.ret_block )
                    return false;
                if( ae.panic_block != be.panic_block )
                    return false;
                if( ae.ret_val != be.ret_val )
                    return false;
                if( ae.args != be.args )
                    return false;

                if( ae.fcn.tag() != be.fcn.tag() )
                    return false;
                TU_MATCHA( (ae.fcn, be.fcn), (af, bf),
                (Value,
                    if( af != bf )
                        return false;
                    ),
                (Path,
                    if( af != bf )
                        return false;
                    ),
                (Intrinsic,
                    if( af.name != bf.name )
                        return false;
                    if( af.params != bf.params )
                        return false;
                    )
                )
                )
            )
            return true;
        }
    };
    // Locate duplicate blocks and replace
    ::std::vector<bool> visited( fcn.blocks.size() );
    ::std::map<unsigned int, unsigned int>  replacements;
    for(unsigned int bb_idx = 0; bb_idx < fcn.blocks.size(); bb_idx ++)
    {
        if( fcn.blocks[bb_idx].terminator.tag() == ::MIR::Terminator::TAGDEAD )
            continue ;
        if( fcn.blocks[bb_idx].terminator.is_Incomplete() && fcn.blocks[bb_idx].statements.size() == 0 )
            continue ;
        if( visited[bb_idx] )
            continue ;
        for(unsigned int i = bb_idx+1; i < fcn.blocks.size(); i ++)
        {
            if( visited[i] )
                continue ;
            if( H::blocks_equal(fcn.blocks[bb_idx], fcn.blocks[i]) ) {
                replacements[i] = bb_idx;
                visited[i] = true;
            }
        }
    }

    if( ! replacements.empty() )
    {
        //MIR_TODO(state, "Unify blocks - " << replacements);
        DEBUG("Unify blocks (old: new) - " << replacements);
        auto patch_tgt = [&replacements](::MIR::BasicBlockId& tgt) {
            auto it = replacements.find(tgt);
            if( it != replacements.end() )
            {
                //DEBUG("BB" << tgt << " => BB" << it->second);
                tgt = it->second;
            }
            };
        for(auto& bb : fcn.blocks)
        {
            if( bb.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;
            visit_terminator_target_mut(bb.terminator, [&](auto& te) {
                patch_tgt(te);
                });
            //DEBUG("- " << bb.terminator);
        }

        for(const auto& r : replacements)
        {
            fcn.blocks[r.first] = ::MIR::BasicBlock {};
            //auto _ = mv$(fcn.blocks[r.first].terminator);
        }

        changed = true;
    }
    return changed;
}

// --------------------------------------------------------------------
// Propagate source values when a composite (tuple) is read
// --------------------------------------------------------------------
bool MIR_Optimise_PropagateKnownValues(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool change_happend = false;
    TRACE_FUNCTION_FR("", change_happend);
    // 1. Determine reference counts for blocks (allows reversing up BB tree)
    ::std::vector<size_t>   block_origins( fcn.blocks.size(), SIZE_MAX );
    {
        ::std::vector<unsigned int> block_uses( fcn.blocks.size() );
        ::std::vector<bool> visited( fcn.blocks.size() );
        ::std::vector< ::MIR::BasicBlockId> to_visit;
        to_visit.push_back( 0 );
        block_uses[0] ++;
        while( to_visit.size() > 0 )
        {
            auto bb = to_visit.back(); to_visit.pop_back();
            if( visited[bb] )
                continue ;
            visited[bb] = true;
            const auto& block = fcn.blocks[bb];

            visit_terminator_target(block.terminator, [&](const auto& idx) {
                if( !visited[idx] )
                    to_visit.push_back(idx);
                if(block_uses[idx] == 0)
                    block_origins[idx] = bb;
                else
                    block_origins[idx] = SIZE_MAX;
                block_uses[idx] ++;
                });
        }
    }

    // 2. Find any assignments (or function uses?) of the form FIELD(LOCAL, _)
    //  > Restricted to simplify logic (and because that's the inefficient pattern observed)
    // 3. Search backwards from that point until the referenced local is assigned
    auto get_field = [&](const ::MIR::LValue& slot_lvalue, unsigned field, size_t start_bb_idx, size_t start_stmt_idx)->const ::MIR::LValue* {
        TRACE_FUNCTION_F(slot_lvalue << "." << field << " BB" << start_bb_idx << "/" << start_stmt_idx);
        // NOTE: An infinite loop is (theoretically) impossible.
        auto bb_idx = start_bb_idx;
        auto stmt_idx = start_stmt_idx;
        for(;;)
        {
            const auto& bb = fcn.blocks[bb_idx];
            while(stmt_idx --)
            {
                if( stmt_idx == bb.statements.size() )
                {
                    DEBUG("BB" << bb_idx << "/TERM - " << bb.terminator);
                    if( terminator_invalidates_lvalue(bb.terminator, slot_lvalue) ) {
                        return nullptr;
                    }
                    continue ;
                }
                const auto& stmt = bb.statements[stmt_idx];
                DEBUG("BB" << bb_idx << "/" << stmt_idx << " - " << stmt);
                if( const auto* se = stmt.opt_Assign() )
                {
                    if( se->dst == slot_lvalue )
                    {
                        if( !se->src.is_Tuple() )
                            return nullptr;
                        const auto& src_param = se->src.as_Tuple().vals.at(field);
                        DEBUG("> Found a source " << src_param);
                        // TODO: Support returning a Param
                        if( !src_param.is_LValue() )
                            return nullptr;
                        const auto& src_lval = src_param.as_LValue();
                        // Visit all statements between the start and here, checking for mutation of this value.
                        auto end_bb_idx = bb_idx;
                        auto end_stmt_idx = stmt_idx;
                        bb_idx = start_bb_idx;
                        stmt_idx = start_stmt_idx;
                        for(;;)
                        {
                            const auto& bb = fcn.blocks[bb_idx];
                            while(stmt_idx--)
                            {
                                if(bb_idx == end_bb_idx && stmt_idx == end_stmt_idx)
                                    return &src_lval;
                                if(stmt_idx == bb.statements.size())
                                {
                                    DEBUG("BB" << bb_idx << "/TERM - " << bb.terminator);
                                    if( terminator_invalidates_lvalue(bb.terminator, src_lval) ) {
                                        // Invalidated: Return.
                                        return nullptr;
                                    }
                                    continue ;
                                }
                                if( statement_invalidates_lvalue(bb.statements[stmt_idx], src_lval) ) {
                                    // Invalidated: Return.
                                    return nullptr;
                                }
                            }
                            assert( block_origins[bb_idx] != SIZE_MAX );
                            bb_idx = block_origins[bb_idx];
                            stmt_idx = fcn.blocks[bb_idx].statements.size() + 1;
                        }
                        throw "";
                    }
                }

                // Check if the slot is invalidated (mutated)
                if( statement_invalidates_lvalue(stmt, slot_lvalue) ) {
                    return nullptr;
                }
            }
            if( block_origins[bb_idx] == SIZE_MAX )
                break;
            bb_idx = block_origins[bb_idx];
            stmt_idx = fcn.blocks[bb_idx].statements.size() + 1;
        }
        return nullptr;
        };
    for(auto& block : fcn.blocks)
    {
        size_t bb_idx = &block - &fcn.blocks.front();
        for(size_t i = 0; i < block.statements.size(); i++)
        {
            state.set_cur_stmt(bb_idx, i);
            DEBUG(state << block.statements[i]);
            visit_mir_lvalues_mut(block.statements[i], [&](::MIR::LValue& lv, auto vu) {
                    if(const auto* e = lv.opt_Field())
                    {
                        if(vu == ValUsage::Read && e->val->is_Local() ) {
                            // TODO: This value _must_ be Copy for this optimisation to work.
                            // - OR, it has to somehow invalidate the original tuple
                            DEBUG(state << "Locating origin of " << lv);
                            ::HIR::TypeRef  tmp;
                            if( !state.m_resolve.type_is_copy(state.sp, state.get_lvalue_type(tmp, *e->val)) )
                            {
                                DEBUG(state << "- not Copy, can't optimise");
                                return false;
                            }
                            const auto* source_lvalue = get_field(*e->val, e->field_index, bb_idx, i);
                            if( source_lvalue )
                            {
                                if( lv != *source_lvalue )
                                {
                                    DEBUG(state << "Source is " << *source_lvalue);
                                    lv = source_lvalue->clone();
                                    change_happend = true;
                                }
                                else
                                {
                                    DEBUG(state << "No change");
                                }
                                return false;
                            }
                        }
                    }
                    return false;
                    });
        }
    }
    return change_happend;
}

// --------------------------------------------------------------------
// Propagate constants and eliminate known paths
// --------------------------------------------------------------------
bool MIR_Optimise_ConstPropagte(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
#if DUMP_BEFORE_ALL || DUMP_BEFORE_CONSTPROPAGATE
    if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);

    // - Remove calls to `size_of` and `align_of` (replace with value if known)
    for(auto& bb : fcn.blocks)
    {
        state.set_cur_stmt_term(bb);
        MIR_DEBUG(state, bb.terminator);
        if( !bb.terminator.is_Call() )
            continue ;
        auto& te = bb.terminator.as_Call();
        if( !te.fcn.is_Intrinsic() )
            continue ;
        const auto& tef = te.fcn.as_Intrinsic();
        if( tef.name == "size_of" )
        {
            size_t size_val = 0;
            if( Target_GetSizeOf(state.sp, state.m_resolve, tef.params.m_types.at(0), size_val) )
            {
                DEBUG("size_of = " << size_val);
                auto val = ::MIR::Constant::make_Uint({ size_val, ::HIR::CoreType::Usize });
                bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(te.ret_val), mv$(val) }));
                bb.terminator = ::MIR::Terminator::make_Goto(te.ret_block);
                changed = true;
            }
        }
        else if( tef.name == "size_of_val" )
        {
            size_t size_val = 0, tmp;
            if( Target_GetSizeAndAlignOf(state.sp, state.m_resolve, tef.params.m_types.at(0), size_val, tmp) && size_val != SIZE_MAX )
            {
                DEBUG("size_of_val = " << size_val);
                auto val = ::MIR::Constant::make_Uint({ size_val, ::HIR::CoreType::Usize });
                bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(te.ret_val), mv$(val) }));
                bb.terminator = ::MIR::Terminator::make_Goto(te.ret_block);
                changed = true;
            }
        }
        else if( tef.name == "align_of" || tef.name == "min_align_of" )
        {
            size_t align_val = 0;
            if( Target_GetAlignOf(state.sp, state.m_resolve, tef.params.m_types.at(0), align_val) )
            {
                DEBUG("align_of = " << align_val);
                auto val = ::MIR::Constant::make_Uint({ align_val, ::HIR::CoreType::Usize });
                bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(te.ret_val), mv$(val) }));
                bb.terminator = ::MIR::Terminator::make_Goto(te.ret_block);
                changed = true;
            }
        }
        else if( tef.name == "min_align_of_val" )
        {
            size_t align_val = 0;
            size_t size_val = 0;
            // Note: Trait object returns align_val = 0 (slice-based types have an alignment)
            if( Target_GetSizeAndAlignOf(state.sp, state.m_resolve, tef.params.m_types.at(0), size_val, align_val) && align_val > 0 )
            {
                DEBUG("min_align_of_val = " << align_val);
                auto val = ::MIR::Constant::make_Uint({ align_val, ::HIR::CoreType::Usize });
                bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(te.ret_val), mv$(val) }));
                bb.terminator = ::MIR::Terminator::make_Goto(te.ret_block);
                changed = true;
            }
        }
        // NOTE: Quick special-case for bswap<u8/i8> (a no-op)
        else if( tef.name == "bswap" && (tef.params.m_types.at(0) == ::HIR::CoreType::U8 || tef.params.m_types.at(0) == ::HIR::CoreType::I8) )
        {
            DEBUG("bswap<u8> is a no-op");
            if( auto* e = te.args.at(0).opt_LValue() )
                bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(te.ret_val), mv$(*e) }));
            else
                bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(te.ret_val), mv$(te.args.at(0).as_Constant()) }));
            bb.terminator = ::MIR::Terminator::make_Goto(te.ret_block);
            changed = true;
        }
        else if( tef.name == "mrustc_slice_len" )
        {
            MIR_ASSERT(state, te.args.at(0).is_LValue(), "Argument to `mrustc_slice_len` must be a lvalue");
            auto& e = te.args.at(0).as_LValue();
            bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(te.ret_val), ::MIR::RValue::make_DstMeta({ mv$(e) }) }));
            bb.terminator = ::MIR::Terminator::make_Goto(te.ret_block);
            changed = true;
        }
        else
        {
            // Ignore any other intrinsics
        }
    }

    // - Propage constants within BBs
    //  > Evaluate BinOp with known values
    //  > Understand intrinsics like overflowing_* (with correct semantics)
    //   > NOTE: No need to locally stitch blocks, next pass will do that
    // TODO: Use ValState to do full constant propagation across blocks

    // Remove redundant temporaries and evaluate known binops
    for(auto& bb : fcn.blocks)
    {
        auto bbidx = &bb - &fcn.blocks.front();

        ::std::map< ::MIR::LValue, ::MIR::Constant >    known_values;
        // Known enum variants
        ::std::map< ::MIR::LValue, unsigned >   known_values_var;
        ::std::map< unsigned, bool >    known_drop_flags;

        auto check_param = [&](::MIR::Param& p) {
            if(const auto* pe = p.opt_LValue()) {
                auto it = known_values.find(*pe);
                if( it != known_values.end() )
                {
                    DEBUG(state << "Value " << *pe << " known to be " << it->second);
                    p = it->second.clone();
                }
            }
            };

        for(auto& stmt : bb.statements)
        {
            auto stmtidx = &stmt - &bb.statements.front();
            state.set_cur_stmt(bbidx, stmtidx);
            // Scan statements forwards:
            // - If a known temporary is used as Param::LValue, replace LValue with the value
            // - If a UniOp has its input known, evaluate
            // - If a BinOp has both values known, evaluate
            if( auto* e = stmt.opt_Assign() )
            {
                TU_MATCHA( (e->src), (se),
                (Use,
                    auto it = known_values.find(se);
                    if( it != known_values.end() )
                    {
                        DEBUG(state << "Value " << se << " known to be" << it->second);
                        e->src = it->second.clone();
                    }
                    ),
                (Constant,
                    // Ignore (knowledge done below)
                    ),
                (SizedArray,
                    check_param(se.val);
                    ),
                (Borrow,
                    ),
                (Cast,
                    ),
                (BinOp,
                    check_param(se.val_l);
                    check_param(se.val_r);

                    if( se.val_l.is_Constant() && se.val_r.is_Constant() )
                    {
                        const auto& val_l = se.val_l.as_Constant();
                        const auto& val_r = se.val_r.as_Constant();

                        ::MIR::Constant new_value;
                        bool replace = false;
                        switch(se.op)
                        {
                        case ::MIR::eBinOp::EQ:
                            if( val_l.is_Const() || val_r.is_Const() )
                                ;
                            else
                            {
                                replace = true;
                                new_value = ::MIR::Constant::make_Bool({val_l == val_r});
                            }
                            break;
                        case ::MIR::eBinOp::NE:
                            if( val_l.is_Const() || val_r.is_Const() )
                                ;
                            else
                            {
                                replace = true;
                                new_value = ::MIR::Constant::make_Bool({val_l != val_r});
                            }
                            break;
                        case ::MIR::eBinOp::LT:
                            if( val_l.is_Const() || val_r.is_Const() )
                                ;
                            else
                            {
                                replace = true;
                                new_value = ::MIR::Constant::make_Bool({val_l < val_r});
                            }
                            break;
                        case ::MIR::eBinOp::LE:
                            if( val_l.is_Const() || val_r.is_Const() )
                                ;
                            else
                            {
                                replace = true;
                                new_value = ::MIR::Constant::make_Bool({val_l <= val_r});
                            }
                            break;
                        case ::MIR::eBinOp::GT:
                            if( val_l.is_Const() || val_r.is_Const() )
                                ;
                            else
                            {
                                replace = true;
                                new_value = ::MIR::Constant::make_Bool({val_l > val_r});
                            }
                            break;
                        case ::MIR::eBinOp::GE:
                            if( val_l.is_Const() || val_r.is_Const() )
                                ;
                            else
                            {
                                replace = true;
                                new_value = ::MIR::Constant::make_Bool({val_l >= val_r});
                            }
                            break;
                        // TODO: Other binary operations
                        // Could emit a TODO?
                        default:
                            break;
                        }

                        if( replace )
                        {
                            DEBUG(state << " " << e->src << " = " << new_value);
                            e->src = mv$(new_value);
                            changed = true;
                        }
                    }
                    ),
                (UniOp,
                    auto it = known_values.find(se.val);
                    if( it != known_values.end() )
                    {
                        const auto& val = it->second;
                        ::MIR::Constant new_value;
                        bool replace = false;
                        // TODO: Evaluate UniOp
                        switch( se.op )
                        {
                        case ::MIR::eUniOp::INV:
                            TU_MATCHA( (val), (ve),
                            (Uint,
                                auto val = ve.v;
                                switch(ve.t)
                                {
                                case ::HIR::CoreType::U8:   val = (~val) & 0xFF;  break;
                                case ::HIR::CoreType::U16:  val = (~val) & 0xFFFF;  break;
                                case ::HIR::CoreType::U32:  val = (~val) & 0xFFFFFFFF;  break;
                                case ::HIR::CoreType::Usize:
                                case ::HIR::CoreType::U64:
                                    val = ~val;
                                    break;
                                case ::HIR::CoreType::U128:
                                    val = ~val;
                                    break;
                                case ::HIR::CoreType::Char:
                                    MIR_BUG(state, "Invalid use of ! on char");
                                    break;
                                default:
                                    // Invalid type for Uint literal
                                    break;
                                }
                                new_value = ::MIR::Constant::make_Uint({ val, ve.t });
                                replace = true;
                                ),
                            (Int,
                                // Is ! valid on Int?
                                ),
                            (Float,
                                // Not valid?
                                ),
                            (Bool,
                                new_value = ::MIR::Constant::make_Bool({ !ve.v });
                                replace = true;
                                ),
                            (Bytes, ),
                            (StaticString, ),
                            (Const,
                                // TODO:
                                ),
                            (ItemAddr,
                                )
                            )
                            break;
                        case ::MIR::eUniOp::NEG:
                            TU_MATCHA( (val), (ve),
                            (Uint,
                                // Not valid?
                                ),
                            (Int,
                                new_value = ::MIR::Constant::make_Int({ -ve.v, ve.t });
                                replace = true;
                                ),
                            (Float,
                                new_value = ::MIR::Constant::make_Float({ -ve.v, ve.t });
                                replace = true;
                                ),
                            (Bool,
                                // Not valid?
                                ),
                            (Bytes, ),
                            (StaticString, ),
                            (Const,
                                // TODO:
                                ),
                            (ItemAddr,
                                )
                            )
                            break;
                        }
                        if( replace )
                        {
                            DEBUG(state << " " << e->src << " = " << new_value);
                            e->src = mv$(new_value);
                            changed = true;
                        }
                    }
                    ),
                (DstMeta,
                    ),
                (DstPtr,
                    ),
                (MakeDst,
                    check_param(se.ptr_val);
                    check_param(se.meta_val);
                    ),
                (Tuple,
                    for(auto& p : se.vals)
                        check_param(p);
                    ),
                (Array,
                    for(auto& p : se.vals)
                        check_param(p);
                    ),
                (Variant,
                    check_param(se.val);
                    ),
                (Struct,
                    for(auto& p : se.vals)
                        check_param(p);
                    )
                )
            }
            else if( const auto* se = stmt.opt_SetDropFlag() )
            {
                if( se->other == ~0u )
                {
                    known_drop_flags.insert(::std::make_pair( se->idx, se->new_val ));
                }
                else
                {
                    auto it = known_drop_flags.find(se->other);
                    if( it != known_drop_flags.end() )
                    {
                        known_drop_flags.insert(::std::make_pair( se->idx, se->new_val != it->second ));
                    }
                }
            }
            else if( auto* se = stmt.opt_Drop() )
            {
                if( se->flag_idx != ~0u )
                {
                    auto it = known_drop_flags.find(se->flag_idx);
                    if( it != known_drop_flags.end() )
                    {
                        if( it->second ) {
                            se->flag_idx = ~0u;
                        }
                        else {
                            // TODO: Delete drop
                            stmt = ::MIR::Statement::make_ScopeEnd({ });
                        }
                    }
                }
            }
            // - If a known temporary is borrowed mutably or mutated somehow, clear its knowledge
            visit_mir_lvalues(stmt, [&known_values,&known_values_var](const ::MIR::LValue& lv, ValUsage vu)->bool {
                if( vu == ValUsage::Write ) {
                    known_values.erase(lv);
                    known_values_var.erase(lv);
                }
                return false;
                });
            // - Locate `temp = SOME_CONST` and record value
            if( const auto* e = stmt.opt_Assign() )
            {
                if( e->dst.is_Local() )
                {
                    // Known constant
                    if( const auto* ce = e->src.opt_Constant() )
                    {
                        known_values.insert(::std::make_pair( e->dst.clone(), ce->clone() ));
                        DEBUG(state << stmt);
                    }
                    // Known variant
                    else if( const auto* ce = e->src.opt_Variant() )
                    {
                        known_values_var.insert(::std::make_pair( e->dst.clone(), ce->index ));
                        DEBUG(state << stmt);
                    }
                    // Propagate knowledge through Local=Local assignments
                    else if( const auto* ce = e->src.opt_Use() )
                    {
                        if( ce->is_Local() )
                        {
                            auto it1 = known_values.find(*ce);
                            auto it2 = known_values_var.find(*ce);
                            assert( !(it1 != known_values.end() && it2 != known_values_var.end()) );
                            if( it1 != known_values.end() ) {
                                known_values.insert(::std::make_pair( e->dst.clone(), it1->second.clone() ));
                                DEBUG(state << stmt);
                            }
                            else if( it1 != known_values.end() ) {
                                known_values_var.insert(::std::make_pair( e->dst.clone(), it2->second ));
                                DEBUG(state << stmt);
                            }
                            else {
                                // Neither known, don't propagate
                            }
                        }
                    }
                }
            }
        }

        state.set_cur_stmt_term(bbidx);
        switch(bb.terminator.tag())
        {
        case ::MIR::Terminator::TAGDEAD:    throw "";
        TU_ARM(bb.terminator, Switch, te) {
            auto it = known_values_var.find(te.val);
            if( it != known_values_var.end() ) {
                MIR_ASSERT(state, it->second < te.targets.size(), "Terminator::Switch with known variant index out of bounds"
                    << " (#" << it->second << " with " << bb.terminator << ")");
                auto new_bb = te.targets.at(it->second);
                DEBUG(state << "Convert " << bb.terminator << " into Goto(" << new_bb << ") because variant known to be #" << it->second);
                bb.terminator = ::MIR::Terminator::make_Goto(new_bb);

                changed = true;
            }
            } break;
        TU_ARM(bb.terminator, If, te) {
            auto it = known_values.find(te.cond);
            if( it != known_values.end() ) {
                MIR_ASSERT(state, it->second.is_Bool(), "Terminator::If with known value not Bool - " << it->second);
                auto new_bb = (it->second.as_Bool().v ? te.bb0 : te.bb1);
                DEBUG(state << "Convert " << bb.terminator << " into Goto(" << new_bb << ") because condition known to be " << it->second);
                bb.terminator = ::MIR::Terminator::make_Goto(new_bb);

                changed = true;
            }
            } break;
        default:
            break;
        }
    }

    // - Remove based on known booleans within a single block
    //  > Eliminates `if false`/`if true` branches
    // TODO: Is this now defunct after the handling of Terminator::If above?
    for(auto& bb : fcn.blocks)
    {
        auto bbidx = &bb - &fcn.blocks.front();
        if( ! bb.terminator.is_If() )   continue;
        const auto& te = bb.terminator.as_If();

        // Restrict condition to being a temporary/variable
        if( te.cond.is_Local() )
            ;
        else
            continue;

        auto has_cond = [&](const auto& lv, auto ut)->bool {
            return lv == te.cond;
            };
        bool val_known = false;
        bool known_val;
        for(unsigned int i = bb.statements.size(); i --; )
        {
            if( bb.statements[i].is_Assign() )
            {
                const auto& se = bb.statements[i].as_Assign();
                // If the condition was mentioned, don't assume it has the same value
                // TODO: What if the condition is a field/index and something else is edited?
                if( visit_mir_lvalues(se.src, has_cond) )
                    break;

                if( se.dst != te.cond )
                    continue;
                if( se.src.is_Constant() && se.src.as_Constant().is_Bool() )
                {
                    val_known = true;
                    known_val = se.src.as_Constant().as_Bool().v;
                }
                else
                {
                    val_known = false;
                }
                break;
            }
            else
            {
                if( visit_mir_lvalues(bb.statements[i], has_cond) )
                    break;
            }
        }
        if( val_known )
        {
            DEBUG("bb" << bbidx << ": Condition known to be " << known_val);
            bb.terminator = ::MIR::Terminator::make_Goto( known_val ? te.bb0 : te.bb1 );
            changed = true;
        }
    }

    return changed;
}


// --------------------------------------------------------------------
// Split `var = Tuple(...,)` into `varN = ...` if the tuple isn't used by
// value.
// --------------------------------------------------------------------
bool MIR_Optimise_SplitAggregates(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);

    // 1. Locate all potential aggregates
    ::std::map<unsigned,::std::pair<unsigned,bool>> potentials;
    for(const auto& blk : fcn.blocks)
    {
        for(const auto& stmt : blk.statements)
        {
            if( const auto* se = stmt.opt_Assign() )
            {
                if( se->dst.is_Local() )
                {
                    potentials[se->dst.as_Local()].first += 1;
                    potentials[se->dst.as_Local()].second = se->src.is_Tuple();
                }
            }
        }
        if(const auto* te = blk.terminator.opt_Call())
        {
            if( te->ret_val.is_Local() )
            {
                // Force to 2 if written in a terminator, it can't be decomposed.
                potentials[te->ret_val.as_Local()].first += 1;
            }
        }
    }
    // - Remove multi-assigned values from the list of potential replacements
    for(auto it = potentials.begin(); it != potentials.end();)
    {
        if(it->second.first > 1 || !it->second.second) {
            it = potentials.erase(it);
        }
        else {
            ++ it;
        }
    }

    // 2. Find any top-level uses of these potentials
    // - Covers borrows, moves, and drops
    if( !potentials.empty() )
    {
        for(const auto& blk : fcn.blocks)
        {
            auto cb = [&](const auto& lv, auto vu) {
                if( lv.is_Local() && vu != ValUsage::Write )
                {
                    auto it = potentials.find(lv.as_Local());
                    if( it != potentials.end() )
                    {
                        potentials.erase(it);
                    }
                }
                return true;
                };
            for(const auto& stmt : blk.statements)
            {
                //state.set_cur_stmt(blk, stmt);
                visit_mir_lvalues(stmt, cb);
                if( stmt.is_Drop() && stmt.as_Drop().slot.is_Local() )
                {
                    auto it = potentials.find(stmt.as_Drop().slot.as_Local());
                    if( it != potentials.end() )
                    {
                        potentials.erase(it);
                    }
                }
            }
            //state.set_cur_stmt_term(blk);
            visit_mir_lvalues(blk.terminator, cb);
        }
    }

    // 3. For each potential, allocate a new local for each field and replace
    if( !potentials.empty() )
    {
        ::std::map<unsigned, ::std::vector<unsigned>>   replacements;
        for(const auto& ent : potentials)
        {
            auto idx = ent.first;
            MIR_ASSERT(state, fcn.locals[idx].m_data.is_Tuple(), "_SplitAggregates - Local("<<idx<<") isn't a tuple - " << fcn.locals[idx]);
            auto inner_tys = ::std::move( fcn.locals[idx].m_data.as_Tuple() );

            ::std::vector<unsigned> new_locals;
            new_locals.reserve(inner_tys.size());
            for(auto& ty : inner_tys)
            {
                new_locals.push_back( fcn.locals.size() );
                fcn.locals.push_back( ::std::move(ty) );
            }
            replacements.insert( ::std::make_pair(idx, ::std::move(new_locals)) );
        }
        DEBUG(state << replacements);

        for(auto& blk : fcn.blocks)
        {
            auto cb = [&](auto& lv, auto _vu) {
                if(lv.is_Field() && lv.as_Field().val->is_Local())
                {
                    auto fld_idx = lv.as_Field().field_index;
                    auto it = replacements.find( lv.as_Field().val->as_Local() );
                    if( it != replacements.end() )
                    {
                        MIR_ASSERT(state, fld_idx < it->second.size(), "Tuple field index out of range");
                        DEBUG(state << "Replace " << lv << " with Local(" << it->second.at(fld_idx) << ")");
                        lv = ::MIR::LValue::make_Local(it->second.at(fld_idx));
                    }
                }
                return false;
                };
            for(auto it = blk.statements.begin(); it != blk.statements.end(); )
            {
                state.set_cur_stmt(blk, *it);
                // Replace field accesses
                visit_mir_lvalues_mut(*it, cb);
                // Explode assignment
                if( it->is_Assign() && it->as_Assign().dst.is_Local() )
                {
                    auto rit = replacements.find(it->as_Assign().dst.as_Local());
                    if( rit != replacements.end() )
                    {
                        DEBUG(state << "Explode assignment " << *it);
                        auto vals = ::std::move(it->as_Assign().src.as_Tuple().vals);
                        it = blk.statements.erase(it);

                        for(size_t i = 0; i < vals.size(); i ++)
                        {
                            auto lv = ::MIR::LValue::make_Local(rit->second[i]);
                            auto rv = vals[i].is_LValue()
                                ? ::MIR::RValue(::std::move( vals[i].as_LValue() ))
                                : ::MIR::RValue(::std::move( vals[i].as_Constant() ))
                                ;
                            it = blk.statements.insert(it,
                                ::MIR::Statement::make_Assign({ ::std::move(lv), ::std::move(rv) })
                                )+1;
                        }

                        continue ;
                    }
                }
                ++ it;
            }
            state.set_cur_stmt_term(blk);
            visit_mir_lvalues_mut(blk.terminator, cb);
        }

        changed = true;
    }

    return changed;
}
// --------------------------------------------------------------------
// Replace `tmp = RValue::Use()` where the temp is only used once
// --------------------------------------------------------------------
bool MIR_Optimise_PropagateSingleAssignments(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool replacement_happend;
    TRACE_FUNCTION_FR("", replacement_happend);

    // TODO: This requires kowing that doing so has no effect.
    // - Can use little heristics like a Call pointing to an assignment of its RV
    // - Count the read/write count of a variable, if it's 1,1 then this optimisation is correct.
    // - If the count is read=*,write=1 and the write is of an argument, replace with the argument.
    struct ValUse {
        unsigned int    read = 0;
        unsigned int    write = 0;
        unsigned int    borrow = 0;
    };
    struct {
        ::std::vector<ValUse> local_uses;

        void use_lvalue(const ::MIR::LValue& lv, ValUsage ut) {
            TU_MATCHA( (lv), (e),
            (Return,
                ),
            (Argument,
                ),
            (Local,
                auto& vu = local_uses[e];
                switch(ut)
                {
                case ValUsage::Move:
                case ValUsage::Read:    vu.read += 1;   break;
                case ValUsage::Write:   vu.write += 1;  break;
                case ValUsage::Borrow:  vu.borrow += 1; break;
                }
                ),
            (Static,
                ),
            (Field,
                use_lvalue(*e.val, ut);
                ),
            (Deref,
                use_lvalue(*e.val, ut);
                ),
            (Index,
                use_lvalue(*e.val, ut);
                use_lvalue(*e.idx, ValUsage::Read);
                ),
            (Downcast,
                use_lvalue(*e.val, ut);
                )
            )
        }
    } val_uses = {
        ::std::vector<ValUse>(fcn.locals.size())
        };
    visit_mir_lvalues(state, fcn, [&](const auto& lv, auto ut){ val_uses.use_lvalue(lv, ut); return false; });

    // --- Eliminate `tmp = Use(...)` (moves lvalues downwards)
    // > Find an assignment `tmp = Use(...)` where the temporary is only written and read once
    // > Locate the usage of this temporary
    //  - Stop on any conditional terminator
    // > Any lvalues in the source lvalue must not be mutated between the source assignment and the usage.
    //  - This includes mutation, borrowing, or moving.
    // > Replace usage with the inner of the original `Use`
    {
        // 1. Assignments (forward propagate)
        ::std::map< ::MIR::LValue, ::MIR::RValue>    replacements;
        for(const auto& block : fcn.blocks)
        {
            if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;

            for(unsigned int stmt_idx = 0; stmt_idx < block.statements.size(); stmt_idx ++)
            {
                const auto& stmt = block.statements[stmt_idx];
                // > Assignment
                if( ! stmt.is_Assign() )
                    continue ;
                const auto& e = stmt.as_Assign();
                // > Of a temporary from with a RValue::Use
                if( const auto* de = e.dst.opt_Local() )
                {
                    const auto& vu = val_uses.local_uses[*de];
                    DEBUG(e.dst << " - VU " << e.dst << " R:" << vu.read << " W:" << vu.write << " B:" << vu.borrow);
                    // TODO: Allow write many?
                    // > Where the variable is written once and read once
                    if( !( vu.read == 1 && vu.write == 1 && vu.borrow == 0 ) )
                        continue ;
                }
                else
                {
                    continue ;
                }
                DEBUG(e.dst << " = " << e.src);
                if( e.src.is_Use() )
                {
                    // Keep the complexity down
                    const auto* srcp = &e.src.as_Use();
                    while( srcp->is_Field() )
                        srcp = &*srcp->as_Field().val;
                    if( !srcp->is_Local() )
                        continue ;

                    if( replacements.find(*srcp) != replacements.end() )
                    {
                        DEBUG("> Can't replace, source has pending replacement");
                        continue;
                    }
                }
                // TODO: Allow any rvalue, but that currently breaks due to chaining
                //else if( e.src.is_Borrow() )
                //{
                //}
                else
                {
                    continue ;
                }
                bool src_is_lvalue = e.src.is_Use();

                auto is_lvalue_usage = [&](const auto& lv, auto ){ return lv == e.dst; };

                // Returns `true` if the passed lvalue is used as a part of the source
                auto is_lvalue_in_val = [&](const auto& lv) {
                    return visit_mir_lvalues(e.src, [&](const auto& slv, auto ) { return lv == slv; });
                    };
                // Eligable for replacement
                // Find where this value is used
                // - Stop on a conditional block terminator
                // - Stop if any value mentioned in the source is mutated/invalidated
                bool stop = false;
                bool found = false;
                for(unsigned int si2 = stmt_idx+1; si2 < block.statements.size(); si2 ++)
                {
                    const auto& stmt2 = block.statements[si2];
                    DEBUG("[find usage] " << stmt2);

                    // Usage found.
                    if( visit_mir_lvalues(stmt2, is_lvalue_usage) )
                    {
                        // If the source isn't a Use, ensure that this is a Use
                        if( !src_is_lvalue )
                        {
                            if( stmt2.is_Assign() && stmt2.as_Assign().src.is_Use() ) {
                                // Good
                            }
                            else {
                                // Bad, this has to stay a temporary
                                stop = true;
                                break;
                            }
                        }
                        found = true;
                        stop = true;
                        break;
                    }

                    // Determine if source is mutated.
                    // > Assume that any mutating access of the root value counts (over-cautious)
                    if( visit_mir_lvalues(stmt2, [&](const auto& lv, auto vu){ return /*vu == ValUsage::Write &&*/ is_lvalue_in_val(lv); }) )
                    {
                        stop = true;
                        break;
                    }
                }
                if( !stop )
                {
                    DEBUG("[find usage] " << block.terminator);
                    TU_MATCHA( (block.terminator), (e),
                    (Incomplete,
                        ),
                    (Return,
                        ),
                    (Diverge,
                        ),
                    (Goto,
                        DEBUG("TODO: Chain");
                        ),
                    (Panic,
                        ),
                    (If,
                        if( src_is_lvalue && visit_mir_lvalue(e.cond, ValUsage::Read, is_lvalue_usage) )
                            found = true;
                        stop = true;
                        ),
                    (Switch,
                        if( src_is_lvalue && visit_mir_lvalue(e.val, ValUsage::Read, is_lvalue_usage) )
                            found = true;
                        stop = true;
                        ),
                    (SwitchValue,
                        if( src_is_lvalue && visit_mir_lvalue(e.val, ValUsage::Read, is_lvalue_usage) )
                            found = true;
                        stop = true;
                        ),
                    (Call,
                        if( e.fcn.is_Value() )
                            if( src_is_lvalue && visit_mir_lvalue(e.fcn.as_Value(), ValUsage::Read, is_lvalue_usage) )
                                found = true;
                        for(const auto& v : e.args)
                        {
                            if( src_is_lvalue && visit_mir_lvalue(v, ValUsage::Read, is_lvalue_usage) )
                                found = true;
                        }
                        stop = true;
                        )
                    )
                }
                // Schedule a replacement in a future pass
                if( found )
                {
                    DEBUG("> Replace " << e.dst << " with " << e.src.as_Use());
                    replacements.insert( ::std::make_pair(e.dst.clone(), e.src.clone()) );
                }
                else
                {
                    DEBUG("- Single-write/read " << e.dst << " not replaced - couldn't find usage");
                }
            }   // for(stmt : block.statements)
        }

        // Apply replacements within replacements
        for(;;)
        {
            unsigned int inner_replaced_count = 0;
            for(auto& r : replacements)
            {
                visit_mir_lvalues_mut(r.second, [&](auto& lv, auto vu) {
                    if( vu == ValUsage::Read || vu == ValUsage::Move )
                    {
                        auto it = replacements.find(lv);
                        if( it != replacements.end() && it->second.is_Use() )
                        {
                            lv = it->second.as_Use().clone();
                            inner_replaced_count ++;
                        }
                    }
                    return false;
                    });
            }
            if( inner_replaced_count == 0 )
                break;
        }

        // Apply replacements
        unsigned int replaced = 0;
        while( replaced < replacements.size() )
        {
            auto old_replaced = replaced;
            auto cb = [&](auto& lv, auto vu){
                if( vu == ValUsage::Read || vu == ValUsage::Move )
                {
                    auto it = replacements.find(lv);
                    if( it != replacements.end() )
                    {
                        MIR_ASSERT(state, it->second.tag() != ::MIR::RValue::TAGDEAD, "Replacement of  " << lv << " fired twice");
                        MIR_ASSERT(state, it->second.is_Use(), "Replacing a lvalue with a rvalue - " << lv << " with " << it->second);
                        auto rval = ::std::move(it->second);
                        lv = ::std::move(rval.as_Use());
                        replaced += 1;
                    }
                }
                return false;
                };
            for(unsigned int block_idx = 0; block_idx < fcn.blocks.size(); block_idx ++)
            {
                auto& block = fcn.blocks[block_idx];
                if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                    continue ;
                for(auto& stmt : block.statements)
                {
                    state.set_cur_stmt(block_idx, (&stmt - &block.statements.front()));
                    if( stmt.is_Assign() && stmt.as_Assign().src.is_Use() )
                    {
                        auto& e = stmt.as_Assign();
                        auto it = replacements.find(e.src.as_Use());
                        if( it != replacements.end() )
                        {
                            MIR_ASSERT(state, it->second.tag() != ::MIR::RValue::TAGDEAD, "Replacement of  " << it->first << " fired twice");
                            e.src = mv$(it->second);
                            replaced += 1;
                        }
                    }
                    else
                    {
                        visit_mir_lvalues_mut(stmt, cb);
                    }
                }
                state.set_cur_stmt_term(block_idx);
                visit_mir_lvalues_mut(block.terminator, cb);
            }
            MIR_ASSERT(state, replaced > old_replaced, "Temporary eliminations didn't advance");
        }
        // Remove assignments of replaced values
        for(auto& block : fcn.blocks)
        {
            for(auto it = block.statements.begin(); it != block.statements.end(); )
            {
                state.set_cur_stmt(&block - &fcn.blocks.front(), (it - block.statements.begin()));
                // If the statement was an assign of a replaced temporary, remove it.
                if( it->is_Assign() && replacements.count( it->as_Assign().dst ) > 0 )
                    it = block.statements.erase(it);
                else {
                    MIR_ASSERT(state, !( it->is_Assign() && it->as_Assign().src.tag() == ::MIR::RValue::TAGDEAD ), "");
                    ++it;
                }
            }
        }
        replacement_happend = (replaced > 0);
    }
    // --- Eliminate `... = Use(tmp)` (propagate lvalues upwards)
    {
        DEBUG("- Move upwards");
        for(auto& block : fcn.blocks)
        {
            for(auto it = block.statements.begin(); it != block.statements.end(); ++it)
            {
                state.set_cur_stmt(&block - &fcn.blocks.front(), it - block.statements.begin());
                if( !it->is_Assign() )
                    continue;
                if( it->as_Assign().src.tag() == ::MIR::RValue::TAGDEAD )
                    continue ;
                auto& to_replace_lval = it->as_Assign().dst;
                if( const auto* e = to_replace_lval.opt_Local() ) {
                    const auto& vu = val_uses.local_uses[*e];
                    if( !( vu.read == 1 && vu.write == 1 && vu.borrow == 0 ) )
                        continue ;
                }
                else {
                    continue;
                }
                // ^^^  `tmp[1:1] = some_rvalue`

                // Find where it's used
                for(auto it2 = it+1; it2 != block.statements.end(); ++it2)
                {
                    if( !it2->is_Assign() )
                        continue ;
                    if( it2->as_Assign().src.tag() == ::MIR::RValue::TAGDEAD )
                        continue ;
                    if( !it2->as_Assign().src.is_Use() )
                        continue ;
                    if( it2->as_Assign().src.as_Use() != to_replace_lval )
                        continue ;
                    const auto& new_dst_lval = it2->as_Assign().dst;
                    // `... = Use(to_replace_lval)`

                    // TODO: Ensure that the target isn't borrowed.
                    if( const auto* e = new_dst_lval.opt_Local() ) {
                        const auto& vu = val_uses.local_uses[*e];
                        if( !( vu.read == 1 && vu.write == 1 && vu.borrow == 0 ) )
                            break ;
                    }
                    else if( new_dst_lval.is_Return() ) {
                        // Return, can't be borrowed?
                    }
                    else {
                        break;
                    }

                    // Ensure that the target doesn't change in the intervening time.
                    bool was_invalidated = false;
                    for(auto it3 = it+1; it3 != it2; it3++)
                    {
                        // Closure returns `true` if the passed lvalue is a component of `new_dst_lval`
                        auto is_lvalue_in_val = [&](const auto& lv) {
                            return visit_mir_lvalue(new_dst_lval, ValUsage::Write, [&](const auto& slv, auto ) { return lv == slv; });
                            };
                        if( visit_mir_lvalues(*it3, [&](const auto& lv, auto ){ return is_lvalue_in_val(lv); }) )
                        {
                            was_invalidated = true;
                            break;
                        }
                    }

                    // Replacement is valid.
                    if( ! was_invalidated )
                    {
                        DEBUG(state << "Replace assignment of " << to_replace_lval << " with " << new_dst_lval);
                        it->as_Assign().dst = mv$(it2->as_Assign().dst);
                        block.statements.erase(it2);
                        replacement_happend = true;
                        break;
                    }
                }
            }
        }
    }

    // --- Function returns (reverse propagate)
    // > Find `tmp = <function call>` where the temporary is used 1:1
    // > Search the following block for `<anything> = Use(this_tmp)`
    // > Ensure that the target of the above assignment isn't used in the intervening statements
    // > Replace function call result value with target of assignment
    {
        DEBUG("- Returns");
        for(auto& block : fcn.blocks)
        {
            if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;

            // If the terminator is a call that writes to a 1:1 value, replace the destination value with the eventual destination (if that value isn't used in the meantime)
            if( block.terminator.is_Call() )
            {
                // TODO: What if the destination located here is a 1:1 and its usage is listed to be replaced by the return value.
                auto& e = block.terminator.as_Call();
                if( !e.ret_val.is_Local() )
                    continue ;
                const auto& vu = val_uses.local_uses[e.ret_val.as_Local()];
                if( !( vu.read == 1 && vu.write == 1 && vu.borrow == 0 ) )
                    continue ;

                // Iterate the target block, looking for where this value is used.
                const ::MIR::LValue* new_dst = nullptr;
                auto& blk2 = fcn.blocks.at(e.ret_block);
                for(const auto& stmt : blk2.statements)
                { 
                    // Find `RValue::Use( this_lvalue )`
                    if( stmt.is_Assign() && stmt.as_Assign().src.is_Use() && stmt.as_Assign().src.as_Use() == e.ret_val ) {
                        new_dst = &stmt.as_Assign().dst;
                        break;
                    }
                }

                // Ensure that the new destination value isn't used before assignment
                if( new_dst )
                {
                    auto lvalue_impacts_dst = [&](const ::MIR::LValue& lv) {
                        return visit_mir_lvalue(*new_dst, ValUsage::Write, [&](const auto& slv, auto ) { return lv == slv; });
                        };
                    for(auto it = blk2.statements.begin(); it != blk2.statements.end(); ++ it)
                    {
                        const auto& stmt = *it;
                        if( stmt.is_Assign() && stmt.as_Assign().src.is_Use() && stmt.as_Assign().src.as_Use() == e.ret_val )
                        {
                            DEBUG("- Replace function return " << e.ret_val << " with " << *new_dst);
                            e.ret_val = new_dst->clone();
                            it = blk2.statements.erase(it);
                            replacement_happend = true;
                            break;
                        }
                        if( visit_mir_lvalues(stmt, [&](const auto& lv, ValUsage vu){ return lv == *new_dst || (vu == ValUsage::Write && lvalue_impacts_dst(lv)); }) )
                        {
                            break;
                        }
                    }
                }
            }
        }
    }

    // Locate values that are written, but not read or borrowed
    // - Current implementation requires a single write (to avoid issues with drop)
    // - if T: Drop (or T: !Copy) then the write should become a drop
    {
        DEBUG("- Write-only");
        for(auto& block : fcn.blocks)
        {
            for(auto it = block.statements.begin(); it != block.statements.end(); ++it)
            {
                state.set_cur_stmt(&block - &fcn.blocks.front(), it - block.statements.begin());
                if( const auto& se = it->opt_Assign() )
                {
                    // Remove No-op assignments (assignment from a lvalue to itself)
                    if( const auto* src_e = se->src.opt_Use() )
                    {
                        if( se->dst == *src_e )
                        {
                            DEBUG(state << se->dst << " set to itself, removing write");
                            it = block.statements.erase(it)-1;
                            continue ;
                        }
                    }

                    // Remove assignments of locals that are never read
                    TU_MATCH_DEF( ::MIR::LValue, (se->dst), (de),
                    (
                        ),
                    (Local,
                        const auto& vu = val_uses.local_uses[de];
                        if( vu.write == 1 && vu.read == 0 && vu.borrow == 0 ) {
                            DEBUG(state << se->dst << " only written, removing write");
                            it = block.statements.erase(it)-1;
                        }
                        )
                    )
                }
            }
            // NOTE: Calls can write values, but they also have side-effects
        }
    }

    // TODO: Run special case replacements for when there's `tmp/var = arg` and `rv = tmp/var`

    return replacement_happend;
}

// ----------------------------------------
// Clear all drop flags that are never read
// ----------------------------------------
bool MIR_Optimise_DeadDropFlags(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool removed_statement = false;
    TRACE_FUNCTION_FR("", removed_statement);
    ::std::vector<bool> read_drop_flags( fcn.drop_flags.size() );
    visit_blocks(state, fcn, [&read_drop_flags](auto , const ::MIR::BasicBlock& block) {
            for(const auto& stmt : block.statements)
            {
                if( const auto* e = stmt.opt_SetDropFlag() )
                {
                    if(e->other != ~0u) {
                        read_drop_flags[e->other] = true;
                    }
                }
                else if( const auto* e = stmt.opt_Drop() )
                {
                    if(e->flag_idx != ~0u) {
                        read_drop_flags[e->flag_idx] = true;
                    }
                }
            }
            });
    visit_blocks_mut(state, fcn, [&read_drop_flags,&removed_statement](auto _id, auto& block) {
            for(auto it = block.statements.begin(); it != block.statements.end(); )
            {
                if(it->is_SetDropFlag() && ! read_drop_flags[it->as_SetDropFlag().idx] ) {
                    removed_statement = true;
                    it = block.statements.erase(it);
                }
                else {
                    ++ it;
                }
            }
            });
    return removed_statement;
}

// --------------------------------------------------------------------
// Remove unread assignments of locals (and replaced assignments of anything?)
// --------------------------------------------------------------------
bool MIR_Optimise_DeadAssignments(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);

    // Find any locals that are never read, and delete their assignments.

    // Per-local flag indicating that the particular local is read.
    ::std::vector<bool> read_locals( fcn.locals.size() );
    for(const auto& bb : fcn.blocks)
    {
        auto cb = [&](const ::MIR::LValue& lv, ValUsage vu) {
            if( lv.is_Local() ) {
                read_locals[lv.as_Local()] = true;
            }
            return false;
            };
        for(const auto& stmt : bb.statements)
        {
            if( stmt.is_Assign() && stmt.as_Assign().dst.is_Local() )
            {
                visit_mir_lvalues(stmt.as_Assign().src, cb);
            }
            else
            {
                visit_mir_lvalues(stmt, cb);
            }
        }
        visit_mir_lvalues(bb.terminator, cb);
    }

    for(auto& bb : fcn.blocks)
    {
        for(auto it = bb.statements.begin(); it != bb.statements.end(); )
        {
            state.set_cur_stmt(&bb - &fcn.blocks.front(), it - bb.statements.begin());
            if( it->is_Assign() && it->as_Assign().dst.is_Local() && read_locals[it->as_Assign().dst.as_Local()] == false )
            {
                DEBUG(state << "Unread assignment, remove - " << *it);
                it = bb.statements.erase(it);
                changed = true;
                continue ;
            }
            ++ it;
        }
    }

    // Locate assignments of locals then find the next assignment or read.
    return changed;
}

// --------------------------------------------------------------------
// Eliminate no-operation assignments that may have appeared
// --------------------------------------------------------------------
bool MIR_Optimise_NoopRemoval(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);

    // Remove useless operations
    for(auto& bb : fcn.blocks)
    {
        for(auto it = bb.statements.begin(); it != bb.statements.end(); )
        {
            // `Value = Use(Value)`
            if( it->is_Assign()
                && it->as_Assign().src.is_Use()
                && it->as_Assign().src.as_Use() == it->as_Assign().dst
                )
            {
                DEBUG(state << "Useless assignment, remove - " << *it);
                it = bb.statements.erase(it);
                changed = true;

                continue ;
            }

            // `Value = Borrow(Deref(Value))`
            if( it->is_Assign()
                && it->as_Assign().src.is_Borrow()
                && it->as_Assign().src.as_Borrow().val.is_Deref()
                && *it->as_Assign().src.as_Borrow().val.as_Deref().val == it->as_Assign().dst
                )
            {
                DEBUG(state << "Useless assignment (v = &*v), remove - " << *it);
                it = bb.statements.erase(it);
                changed = true;

                continue ;
            }

            ++ it;
        }
    }

    return changed;
}


// --------------------------------------------------------------------
// Clear all unused blocks
// --------------------------------------------------------------------
bool MIR_Optimise_GarbageCollect_Partial(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    ::std::vector<bool> visited( fcn.blocks.size() );
    visit_blocks(state, fcn, [&visited](auto bb, const auto& _blokc) {
            assert( !visited[bb] );
            visited[bb] = true;
            });
    bool rv = false;
    for(unsigned int i = 0; i < visited.size(); i ++)
    {
        if( !visited[i] )
        {
            DEBUG("CLEAR bb" << i);
            fcn.blocks[i].statements.clear();
            fcn.blocks[i].terminator = ::MIR::Terminator::make_Incomplete({});
            rv = true;
        }
    }
    return rv;
}
// --------------------------------------------------------------------
// Remove all unused temporaries and blocks
// --------------------------------------------------------------------
bool MIR_Optimise_GarbageCollect(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    ::std::vector<bool> used_locals( fcn.locals.size() );
    ::std::vector<bool> used_dfs( fcn.drop_flags.size() );
    ::std::vector<bool> visited( fcn.blocks.size() );

    visit_blocks(state, fcn, [&](auto bb, const auto& block) {
        visited[bb] = true;

        auto assigned_lval = [&](const ::MIR::LValue& lv) {
            if(const auto* le = lv.opt_Local() )
                used_locals[*le] = true;
            };

        for(const auto& stmt : block.statements)
        {
            TU_IFLET( ::MIR::Statement, stmt, Assign, e,
                assigned_lval(e.dst);
            )
            //else if( const auto* e = stmt.opt_Drop() )
            //{
            //    //if( e->flag_idx != ~0u )
            //    //    used_dfs.at(e->flag_idx) = true;
            //}
            else if( const auto* e = stmt.opt_Asm() )
            {
                for(const auto& val : e->outputs)
                    assigned_lval(val.second);
            }
            else if( const auto* e = stmt.opt_SetDropFlag() )
            {
                if( e->other != ~0u )
                    used_dfs.at(e->other) = true;
                used_dfs.at(e->idx) = true;
            }
        }

        if( const auto* te = block.terminator.opt_Call() )
        {
            assigned_lval(te->ret_val);
        }
        });

    ::std::vector<unsigned int> block_rewrite_table;
    for(unsigned int i = 0, j = 0; i < fcn.blocks.size(); i ++)
    {
        block_rewrite_table.push_back( visited[i] ? j ++ : ~0u );
    }
    ::std::vector<unsigned int> local_rewrite_table;
    unsigned int n_locals = fcn.locals.size();
    for(unsigned int i = 0, j = 0; i < n_locals; i ++)
    {
        if( !used_locals[i] )
        {
            fcn.locals.erase(fcn.locals.begin() + j);
        }
        else {
            DEBUG("_" << i << " => _" << j);
        }
        local_rewrite_table.push_back( used_locals[i] ? j ++ : ~0u );
    }
    DEBUG("Deleted Locals:" << FMT_CB(ss,
                for(auto run : runs(used_locals))
                    if( !used_locals[run.first] )
                    {
                        ss << " " << run.first;
                        if(run.second != run.first)
                            ss << "-" << run.second;
                    }
                ));
    ::std::vector<unsigned int> df_rewrite_table;
    unsigned int n_df = fcn.drop_flags.size();
    for(unsigned int i = 0, j = 0; i < n_df; i ++)
    {
        if( !used_dfs[i] )
        {
            DEBUG("GC df" << i);
            // NOTE: Not erased until after rewriting
        }
        df_rewrite_table.push_back( used_dfs[i] ? j ++ : ~0u );
    }

    auto it = fcn.blocks.begin();
    for(unsigned int i = 0; i < visited.size(); i ++)
    {
        if( !visited[i] )
        {
            // Delete
            DEBUG("GC bb" << i);
            it = fcn.blocks.erase(it);
        }
        else
        {
            auto lvalue_cb = [&](auto& lv, auto ) {
                if(auto* e = lv.opt_Local() ) {
                    MIR_ASSERT(state, *e < local_rewrite_table.size(), "Variable out of range - " << lv);
                    // If the table entry for this variable is !0, it wasn't marked as used
                    MIR_ASSERT(state, local_rewrite_table.at(*e) != ~0u, "LValue " << lv << " incorrectly marked as unused");
                    *e = local_rewrite_table.at(*e);
                }
                return false;
                };
            ::std::vector<bool> to_remove_statements(it->statements.size());
            for(auto& stmt : it->statements)
            {
                auto stmt_idx = &stmt - &it->statements.front();
                state.set_cur_stmt(i, stmt_idx);
                if( auto* se = stmt.opt_Drop() )
                {
                    // If the drop flag was unset, either remove the drop or remove the drop flag reference
                    if( se->flag_idx != ~0u && df_rewrite_table[se->flag_idx] == ~0u)
                    {
                        if( fcn.drop_flags.at(se->flag_idx) ) {
                            DEBUG(state << "Remove flag from " << stmt << " - Flag never set and default true");
                            se->flag_idx = ~0u;
                        }
                        else {
                            DEBUG(state << "Remove " << stmt << " - Flag never set and default false");
                            to_remove_statements[stmt_idx] = true;
                            continue ;
                        }
                    }
                }

                visit_mir_lvalues_mut(stmt, lvalue_cb);
                if( auto* se = stmt.opt_Drop() )
                {
                    // Rewrite drop flag indexes
                    if( se->flag_idx != ~0u )
                        se->flag_idx = df_rewrite_table[se->flag_idx];
                }
                else if( auto* se = stmt.opt_SetDropFlag() )
                {
                    // Rewrite drop flag indexes OR delete
                    if( df_rewrite_table[se->idx] == ~0u ) {
                        to_remove_statements[stmt_idx] = true;
                        continue ;
                    }
                    se->idx = df_rewrite_table[se->idx];
                    if( se->other != ~0u )
                        se->other = df_rewrite_table[se->other];
                }
                else if( auto* se = stmt.opt_ScopeEnd() )
                {
                    for(auto it = se->slots.begin(); it != se->slots.end(); )
                    {
                        if( local_rewrite_table.at(*it) == ~0u ) {
                            it = se->slots.erase(it);
                        }
                        else {
                            *it = local_rewrite_table.at(*it);
                            ++ it;
                        }
                    }

                    if( se->slots.empty() ) {
                        DEBUG(state << "Delete ScopeEnd (now empty)");
                        to_remove_statements[stmt_idx] = true;
                        continue ;
                    }
                }
            }
            state.set_cur_stmt_term(i);
            // Rewrite and advance
            TU_MATCHA( (it->terminator), (e),
            (Incomplete,
                ),
            (Return,
                ),
            (Diverge,
                ),
            (Goto,
                e = block_rewrite_table[e];
                ),
            (Panic,
                ),
            (If,
                visit_mir_lvalue_mut(e.cond, ValUsage::Read, lvalue_cb);
                e.bb0 = block_rewrite_table[e.bb0];
                e.bb1 = block_rewrite_table[e.bb1];
                ),
            (Switch,
                visit_mir_lvalue_mut(e.val, ValUsage::Read, lvalue_cb);
                for(auto& target : e.targets)
                    target = block_rewrite_table[target];
                ),
            (SwitchValue,
                visit_mir_lvalue_mut(e.val, ValUsage::Read, lvalue_cb);
                for(auto& target : e.targets)
                    target = block_rewrite_table[target];
                e.def_target = block_rewrite_table[e.def_target];
                ),
            (Call,
                if( e.fcn.is_Value() ) {
                    visit_mir_lvalue_mut(e.fcn.as_Value(), ValUsage::Read, lvalue_cb);
                }
                for(auto& v : e.args)
                    visit_mir_lvalue_mut(v, ValUsage::Read, lvalue_cb);
                visit_mir_lvalue_mut(e.ret_val, ValUsage::Write, lvalue_cb);
                e.ret_block   = block_rewrite_table[e.ret_block];
                e.panic_block = block_rewrite_table[e.panic_block];
                )
            )

            // Delete all statements flagged in a bitmap for deletion
            auto stmt_it = it->statements.begin();
            for(auto flag : to_remove_statements)
            {
                if(flag) {
                    stmt_it = it->statements.erase(stmt_it);
                }
                else {
                    ++ stmt_it;
                }
            }

            ++it;
        }
    }

    for(unsigned int i = 0, j = 0; i < n_df; i ++)
    {
        if( !used_dfs[i] )
        {
            fcn.drop_flags.erase(fcn.drop_flags.begin() + j);
        }
        else
        {
            j ++;
        }
    }

    // TODO: Detect if any optimisations happened, and return true in that case
    return false;
}


/// Sort basic blocks to approximate program flow (helps when reading MIR)
void MIR_SortBlocks(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn)
{
    ::std::vector<bool> visited( fcn.blocks.size() );
    ::std::vector<::std::pair<unsigned,unsigned>> depths( fcn.blocks.size() );

    struct Todo {
        size_t  bb_idx;
        unsigned    branch_count;
        unsigned    level;
    };
    unsigned int branches = 0;
    ::std::vector<Todo> todo;
    todo.push_back( Todo { 0, 0, 0 } );

    while(!todo.empty())
    {
        auto info = todo.back();
        todo.pop_back();
        if( visited[info.bb_idx] )
            continue ;

        visited[info.bb_idx] = true;
        depths[info.bb_idx] = ::std::make_pair( info.branch_count, info.level );
        const auto& bb = fcn.blocks[info.bb_idx];

        TU_MATCHA( (bb.terminator), (te),
        (Incomplete,
            ),
        (Return,
            ),
        (Diverge,
            ),
        (Goto,
            todo.push_back(Todo { te, info.branch_count, info.level + 1 });
            ),
        (Panic,
            todo.push_back(Todo { te.dst, info.branch_count, info.level + 1 });
            ),
        (If,
            todo.push_back(Todo { te.bb0, ++branches, info.level + 1 });
            todo.push_back(Todo { te.bb1, ++branches, info.level + 1 });
            ),
        (Switch,
            for(auto dst : te.targets)
                todo.push_back(Todo { dst, ++branches, info.level + 1 });
            ),
        (SwitchValue,
            for(auto dst : te.targets)
                todo.push_back(Todo { dst, ++branches, info.level + 1 });
            todo.push_back(Todo { te.def_target, info.branch_count, info.level + 1 });
            ),
        (Call,
            todo.push_back(Todo { te.ret_block, info.branch_count, info.level + 1 });
            todo.push_back(Todo { te.panic_block, ++branches, info.level + 1 });
            )
        )
    }

    // Sort a list of block indexes by `depths`
    ::std::vector<size_t>   idxes;
    idxes.reserve(fcn.blocks.size());
    for(size_t i = 0; i < fcn.blocks.size(); i++)
        idxes.push_back(i);
    ::std::sort( idxes.begin(), idxes.end(), [&](auto a, auto b){
            return depths.at(a) < depths.at(b);
            });

    DEBUG(idxes);

    decltype(fcn.blocks)    new_block_list;
    new_block_list.reserve( fcn.blocks.size() );
    for(auto idx : idxes)
    {
        auto fix_bb_idx = [&](auto idx){ return ::std::find(idxes.begin(), idxes.end(), idx) - idxes.begin(); };
        new_block_list.push_back( mv$(fcn.blocks[idx]) );
        visit_terminator_target_mut(new_block_list.back().terminator, [&](auto& te){
            te = fix_bb_idx(te);
            });
    }
    fcn.blocks = mv$(new_block_list);
}


void MIR_OptimiseCrate(::HIR::Crate& crate, bool do_minimal_optimisation)
{
    ::MIR::OuterVisitor ov { crate, [do_minimal_optimisation](const auto& res, const auto& p, auto& expr, const auto& args, const auto& ty)
        {
            if( ! dynamic_cast<::HIR::ExprNode_Block*>(expr.get()) ) {
                return ;
            }
            if( do_minimal_optimisation ) {
                MIR_OptimiseMin(res, p, *expr.m_mir, args, ty);
            }
            else {
                MIR_Optimise(res, p, *expr.m_mir, args, ty);
            }
        }
        };
    ov.visit_crate(crate);
}

void MIR_OptimiseCrate_Inlining(const ::HIR::Crate& crate, TransList& list)
{
    ::StaticTraitResolve    resolve { crate };

    bool did_inline_on_pass;

    size_t  MAX_ITERATIONS = 5; // TODO: Tune this.
    size_t  num_iterations = 0;
    do
    {
        did_inline_on_pass = false;

        for(auto& fcn_ent : list.m_functions)
        {
            const auto& path = fcn_ent.first;
            //const auto& pp = fcn_ent.second->pp;
            auto& hir_fcn = *const_cast<::HIR::Function*>(fcn_ent.second->ptr);
            auto& mono_fcn = fcn_ent.second->monomorphised;

            ::std::string s = FMT(path);
            ::HIR::ItemPath ip(s);

            if( mono_fcn.code )
            {
                did_inline_on_pass |= MIR_OptimiseInline(resolve, ip, *mono_fcn.code, mono_fcn.arg_tys, mono_fcn.ret_ty, list);
            }
            else if( hir_fcn.m_code.m_mir)
            {
                did_inline_on_pass |= MIR_OptimiseInline(resolve, ip, *hir_fcn.m_code.m_mir, hir_fcn.m_args, hir_fcn.m_return, list);
            }
            else
            {
                // Extern, no optimisations
            }
        }
    } while( did_inline_on_pass && num_iterations < MAX_ITERATIONS );

    if( did_inline_on_pass )
    {
        DEBUG("Ran inlining optimise pass to exhaustion (maximum of " << MAX_ITERATIONS << " hit");
    }
}
