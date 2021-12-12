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
#include <cmath>
#include <iomanip>
#include <trans/target.hpp>
#include <trans/trans_list.hpp> // Note: This is included for inlining after enumeration and monomorph

#include <hir/expr.hpp> // HACK


#define DUMP_BEFORE_ALL 1
#define DUMP_BEFORE_CONSTPROPAGATE 0
#define DUMP_AFTER_PASS 1
#define DUMP_AFTER_ALL  0

#define DUMP_AFTER_DONE     1
#define CHECK_AFTER_DONE    2   // 1 = Check before GC, 2 = check before and after GC

// ----
// List of optimisations avaliable
// ----
bool MIR_Optimise_BlockSimplify(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_Inlining(::MIR::TypeResolve& state, ::MIR::Function& fcn, bool minimal, const TransList* list=nullptr);
bool MIR_Optimise_SplitAggregates(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_PropagateSingleAssignments(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_PropagateKnownValues(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_DeTemporary(::MIR::TypeResolve& state, ::MIR::Function& fcn); // Eliminate useless temporaries
bool MIR_Optimise_UnifyTemporaries(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_CommonStatements(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_UnifyBlocks(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_ConstPropagate(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_DeadDropFlags(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_DeadAssignments(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_NoopRemoval(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_GotoAssign(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_UselessReborrows(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_GarbageCollect_Partial(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_GarbageCollect(::MIR::TypeResolve& state, ::MIR::Function& fcn);

enum {
    CHECKMODE_UNKNOWN,
    CHECKMODE_NONE,
    CHECKMODE_FINAL,
    CHECKMODE_PASS,
    CHECKMODE_ALL,
};
static int check_mode() {
    static int mode = CHECKMODE_UNKNOWN;
    if( mode == CHECKMODE_UNKNOWN ) {
        const auto* n = getenv("MRUSTC_MIR_CHECK");
        if(n)
        {
            if( strcmp(n, "none") == 0 ) {
                mode = CHECKMODE_NONE;
            }
            else if( strcmp(n, "final") == 0 ) {
                mode = CHECKMODE_FINAL;
            }
            else if( strcmp(n, "pass") == 0 ) {
                mode = CHECKMODE_PASS;
            }
            else if( strcmp(n, "all") == 0 ) {
                mode = CHECKMODE_ALL;
            }
            else {
                WARNING(Span(), W0000,
                    "Unknown value for $MRUSTC_MIR_CHECK - '" << n << "'"
                    << ": options are 'none','final','pass','all'"
                    );
            }
        }

        if( mode == CHECKMODE_UNKNOWN ) {
            mode = CHECKMODE_FINAL;
        }
    }
    return mode;
}
static bool check_after_all() {
    return check_mode() >= CHECKMODE_ALL;
}

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
        if(check_after_all()) {
            MIR_Validate(resolve, path, fcn, args, ret_type);
        }
    }

    MIR_Optimise_BlockSimplify(state, fcn);
    MIR_Optimise_UnifyBlocks(state, fcn);

    //MIR_Optimise_GarbageCollect_Partial(state, fcn);

    // NOTE: No check here, this version of optimise is pretty reliable
    //if( check_mode() >= CHECKMODE_FINAL ) {
    //    MIR_Validate(resolve, path, fcn, args, ret_type);
    //}
    MIR_Optimise_GarbageCollect(state, fcn);
    //MIR_Validate_Full(resolve, path, fcn, args, ret_type);
    MIR_SortBlocks(resolve, path, fcn);

#if CHECK_AFTER_DONE > 1
    if( check_mode() >= CHECKMODE_FINAL ) {
        MIR_Validate(resolve, path, fcn, args, ret_type);
    }
#endif
    return ;
}
/// Perfom inlining only, using a list of monomorphised functions, then cleans up the flow graph
///
/// Returns true if any optimisation was performed
bool MIR_OptimiseInline(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type, const TransList& list)
{
    static Span sp;
    bool rv = false;
    TRACE_FUNCTION_FR(path, rv);
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };

    while( MIR_Optimise_Inlining(state, fcn, false, &list) )
    {
        MIR_Cleanup(resolve, path, fcn, args, ret_type);
        if( check_after_all() ) {
            MIR_Validate(resolve, path, fcn, args, ret_type);
        }
        rv = true;
    }

    if( rv )
    {
        MIR_Optimise(resolve, path, fcn, args, ret_type, /*do_inline=*/false);
    }

    return rv;
}
void MIR_Optimise(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type, bool do_inline/*=true*/)
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
        if( MIR_Optimise_BlockSimplify(state, fcn) )
        {
#if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
            // NOTE: Don't set `change_happened`, as this is the first pass
        }
        //else { MIR_Validate(resolve, path, fcn, args, ret_type); }

        // >> Apply known constants
        if( MIR_Optimise_ConstPropagate(state, fcn) )
        {
#if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
            change_happened = true;
        }

        // >> Attempt to remove useless temporaries
        if( MIR_Optimise_DeTemporary(state, fcn) )
        {
            // - Run until no changes
            while( MIR_Optimise_DeTemporary(state, fcn) )
            {
            }
#if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
            change_happened = true;
        }
        //else { MIR_Validate(resolve, path, fcn, args, ret_type); }

        // >> Split apart aggregates that are never used such (Written once, never used directly)
        if( MIR_Optimise_SplitAggregates(state, fcn) )
        {
#if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
            change_happened = true;
        }
        //else { MIR_Validate(resolve, path, fcn, args, ret_type); }

        // >> Replace values from composites if they're known
        //   - Undoes the inefficiencies from the `match (a, b) { ... }` pattern
        if( MIR_Optimise_PropagateKnownValues(state, fcn) )
        {
#if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
            change_happened = true;
        }
        //else { MIR_Validate(resolve, path, fcn, args, ret_type); }

        // TODO: Convert `&mut *mut_foo` into `mut_foo` if the source is movable and not used afterwards

        // >> Propagate/remove dead assignments
        if( MIR_Optimise_PropagateSingleAssignments(state, fcn) )
        {
            // - Run until no changes
            while( MIR_Optimise_PropagateSingleAssignments(state, fcn) )
            {
            }
#if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
            change_happened = true;
        }
        //else { MIR_Validate(resolve, path, fcn, args, ret_type); }

        // >> Move common statements (assignments) across gotos.
        //if( MIR_Optimise_CommonStatements(state, fcn) )
        //{
        //    if( check_after_all() ) {
        //        MIR_Validate(resolve, path, fcn, args, ret_type);
        //    }
        //    change_happened = true;
        //}

        // >> Combine Duplicate Blocks
        if( MIR_Optimise_UnifyBlocks(state, fcn) )
        {
#if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
            change_happened = true;
        }
        // >> Remove assignments of unsed drop flags
        if( MIR_Optimise_DeadDropFlags(state, fcn) )
        {
#if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
            change_happened = true;
        }
        // >> Remove assignments that are never read
        if( MIR_Optimise_DeadAssignments(state, fcn) )
        {
#if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
            change_happened = true;
        }
        // >> Remove no-op assignments
        if( MIR_Optimise_NoopRemoval(state, fcn) )
        {
#if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
            change_happened = true;
        }

        // >> Remove re-borrow operations that don't need to exist
        if( MIR_Optimise_UselessReborrows(state, fcn) )
        {
            #if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
            #endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
            change_happened = true;
        }

        // >> If the first statement of a block is an assignment, and the last op of the previous is to that assignment's source, move up.
        if( MIR_Optimise_GotoAssign(state, fcn) )
        {
            #if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
            #endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
            change_happened = true;
        }

        // >> Inline short functions
        if( do_inline && !change_happened )
        {
            if( MIR_Optimise_Inlining(state, fcn, /*minimal=*/false) )
            {
                // Apply cleanup again (as monomorpisation in inlining may have exposed a vtable call)
                MIR_Cleanup(resolve, path, fcn, args, ret_type);
                //MIR_Dump_Fcn(::std::cout, fcn);
#if DUMP_AFTER_ALL
                if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
                if( check_after_all() ) {
                    MIR_Validate(resolve, path, fcn, args, ret_type);
                }
                change_happened = true;
            }
        }

        if( change_happened )
        {
            #if DUMP_AFTER_PASS
            if( debug_enabled() ) {
                MIR_Dump_Fcn(::std::cout, fcn);
            }
            #endif
            if( check_mode() == CHECKMODE_PASS ) {  // NOTE: Skipped if CHECKMODE_ALL
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
        }
        //else { MIR_Validate(resolve, path, fcn, args, ret_type); }

        if( MIR_Optimise_GarbageCollect_Partial(state, fcn) )
        {
            change_happened = true;
#if DUMP_AFTER_ALL
            if( debug_enabled() ) MIR_Dump_Fcn(::std::cout, fcn);
#endif
            if( check_after_all() ) {
                MIR_Validate(resolve, path, fcn, args, ret_type);
            }
        }
        //else { MIR_Validate(resolve, path, fcn, args, ret_type); }

#if 0
        if(change_happened)
        {
            MIR_Validate_Full(resolve, path, fcn, args, ret_type);
        }
#endif
        pass_num += 1;
    } while( change_happened );

    // Run UnifyTemporaries last, then unify blocks, then run some
    // optimisations that might be affected
#if 0
    if(MIR_Optimise_UnifyTemporaries(state, fcn))
    {
        if( check_after_all() ) {
            MIR_Validate(resolve, path, fcn, args, ret_type);
        }
        MIR_Optimise_UnifyBlocks(state, fcn);
        //MIR_Optimise_ConstPropagate(state, fcn);
        MIR_Optimise_NoopRemoval(state, fcn);
    }
#endif


    #if DUMP_AFTER_DONE
    if( debug_enabled() ) {
        MIR_Dump_Fcn(::std::cout, fcn);
    }
    #endif
    if( check_mode() >= CHECKMODE_FINAL )
    {
        // DEFENCE: Run validation _before_ GC (so validation errors refer to the pre-gc numbers)
        MIR_Validate(resolve, path, fcn, args, ret_type);
    }
    // GC pass on blocks and variables
    // - Find unused blocks, then delete and rewrite all references.
    MIR_Optimise_GarbageCollect(state, fcn);

    //MIR_Validate_Full(resolve, path, fcn, args, ret_type);

    MIR_SortBlocks(resolve, path, fcn);
    if( check_mode() >= CHECKMODE_FINAL )
    {
        MIR_Validate(resolve, path, fcn, args, ret_type);
    }
}

namespace
{
    enum class ValUsage {
        Move,   // Moving read (even if T: Copy)
        Read,   // Non-moving read (e.g. indexing or deref, TODO: &move pointers?)
        Write,  // Mutation
        Borrow, // Any borrow
    };

    bool visit_mir_lvalues_inner(const ::MIR::LValue& lv, ValUsage u, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        for(const auto& w : lv.m_wrappers)
        {
            if(w.is_Index())
            {
                if( cb(::MIR::LValue::new_Local(w.as_Index()), ValUsage::Read) )
                    return true;
            }
            else if(w.is_Deref())
            {
                //u = ValUsage::Read;
            }
        }
        return cb(lv, u);
    }
    bool visit_mir_lvalue_mut(::MIR::LValue& lv, ValUsage u, ::std::function<bool(::MIR::LValue::MRef& , ValUsage)> cb)
    {
        auto lvr = ::MIR::LValue::MRef(lv);
        do
        {
            if( cb(lvr, u) )
                return true;
            // TODO: Use a TU_MATCH?
            if( lvr.is_Index() )
            {
                auto ilv = ::MIR::LValue::new_Local(lvr.as_Index());
                auto ilv_r = ::MIR::LValue::MRef(ilv);
                bool rv = cb(ilv_r, ValUsage::Read);
                assert(ilv.is_Local() && ilv.as_Local() == lvr.as_Index());
                if( rv )
                    return true;
            }
            else if( lvr.is_Field() )
            {
                // HACK: If "moving", use a "Read" value usage (covers some quirks)
                if( u == ValUsage::Move ) {
                    u = ValUsage::Read;
                }
            }
            else if( lvr.is_Deref() )
            {
                // TODO: Is this right?
                if( u == ValUsage::Borrow ) {
                    u = ValUsage::Read;
                }
            }
            else
            {
                // No change
            }
        } while( lvr.try_unwrap() );
        return false;
    }
    bool visit_mir_lvalue_raw_mut(::MIR::LValue& lv, ValUsage u, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        return cb(lv, u);
    }

    bool visit_mir_lvalue_mut(::MIR::Param& p, ValUsage u, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        if( auto* e = p.opt_LValue() )
        {
            return visit_mir_lvalue_raw_mut(*e, u, cb);
        }
        else
        {
            return false;
        }
    }

    bool visit_mir_lvalues_mut(::MIR::RValue& rval, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        bool rv = false;
        TU_MATCH_HDRA( (rval), {)
        TU_ARMA(Use, se) {
            rv |= visit_mir_lvalue_raw_mut(se, ValUsage::Move, cb); // Can move
            }
        TU_ARMA(Constant, se) {
            }
        TU_ARMA(SizedArray, se) {
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Read, cb); // Has to be Read
            }
        TU_ARMA(Borrow, se) {
            rv |= visit_mir_lvalue_raw_mut(se.val, ValUsage::Borrow, cb);
            }
        TU_ARMA(Cast, se) {
            rv |= visit_mir_lvalue_raw_mut(se.val, ValUsage::Read, cb); // Also has to be read
            }
        TU_ARMA(BinOp, se) {
            rv |= visit_mir_lvalue_mut(se.val_l, ValUsage::Read, cb);   // Same
            rv |= visit_mir_lvalue_mut(se.val_r, ValUsage::Read, cb);
            }
        TU_ARMA(UniOp, se) {
            rv |= visit_mir_lvalue_raw_mut(se.val, ValUsage::Read, cb);
            }
        TU_ARMA(DstMeta, se) {
            rv |= visit_mir_lvalue_raw_mut(se.val, ValUsage::Read, cb); // Reads
            }
        TU_ARMA(DstPtr, se) {
            rv |= visit_mir_lvalue_raw_mut(se.val, ValUsage::Read, cb);
            }
        TU_ARMA(MakeDst, se) {
            rv |= visit_mir_lvalue_mut(se.ptr_val, ValUsage::Move, cb);
            rv |= visit_mir_lvalue_mut(se.meta_val, ValUsage::Read, cb);    // Note, metadata has to be Copy
            }
        TU_ARMA(Tuple, se) {
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, ValUsage::Move, cb);
            }
        TU_ARMA(Array, se) {
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, ValUsage::Move, cb);
            }
        TU_ARMA(UnionVariant, se) {
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Move, cb);
            }
        TU_ARMA(EnumVariant, se) {
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, ValUsage::Move, cb);
            }
        TU_ARMA(Struct, se) {
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, ValUsage::Move, cb);
            }
        }
        return rv;
    }
    bool visit_mir_lvalues(const ::MIR::RValue& rval, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        return visit_mir_lvalues_mut(const_cast<::MIR::RValue&>(rval), [&](auto& lv, auto u){ return cb(lv, u); });
    }

    bool visit_mir_lvalues_mut(::MIR::Statement& stmt, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        bool rv = false;
        TU_MATCH_HDRA( (stmt), {)
        TU_ARMA(Assign, e) {
            rv |= visit_mir_lvalues_mut(e.src, cb);
            rv |= visit_mir_lvalue_raw_mut(e.dst, ValUsage::Write, cb);
            }
        TU_ARMA(Asm, e) {
            for(auto& v : e.inputs)
                rv |= visit_mir_lvalue_raw_mut(v.second, ValUsage::Read, cb);
            for(auto& v : e.outputs)
                rv |= visit_mir_lvalue_raw_mut(v.second, ValUsage::Write, cb);
            }
        TU_ARMA(Asm2, e) {
            for(auto& p : e.params)
            {
                TU_MATCH_HDRA( (p), { )
                TU_ARMA(Const, v) {}
                TU_ARMA(Sym, v) {}
                TU_ARMA(Reg, v) {
                    if(v.input)
                        rv |= visit_mir_lvalue_mut(*v.input, ValUsage::Read, cb);
                    if(v.output)
                        rv |= visit_mir_lvalue_raw_mut(*v.output, ValUsage::Write, cb);
                    }
                }
            }
            }
        TU_ARMA(SetDropFlag, e) {
            }
        TU_ARMA(Drop, e) {
            // Well, it mutates...
            rv |= visit_mir_lvalue_raw_mut(e.slot, ValUsage::Write, cb);
            }
        TU_ARMA(ScopeEnd, e) {
            }
        }
        return rv;
    }
    bool visit_mir_lvalues(const ::MIR::Statement& stmt, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        return visit_mir_lvalues_mut(const_cast<::MIR::Statement&>(stmt), [&](auto& lv, auto im){ return cb(lv, im); });
    }

    bool visit_mir_lvalues_mut(::MIR::Terminator& term, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        bool rv = false;
        TU_MATCH_HDRA( (term), {)
        TU_ARMA(Incomplete, e) {
            }
        TU_ARMA(Return, e) {
            }
        TU_ARMA(Diverge, e) {
            }
        TU_ARMA(Goto, e) {
            }
        TU_ARMA(Panic, e) {
            }
        TU_ARMA(If, e) {
            rv |= visit_mir_lvalue_raw_mut(e.cond, ValUsage::Read, cb);
            }
        TU_ARMA(Switch, e) {
            rv |= visit_mir_lvalue_raw_mut(e.val, ValUsage::Read, cb);
            }
        TU_ARMA(SwitchValue, e) {
            rv |= visit_mir_lvalue_raw_mut(e.val, ValUsage::Read, cb);
            }
        TU_ARMA(Call, e) {
            if( e.fcn.is_Value() ) {
                rv |= visit_mir_lvalue_raw_mut(e.fcn.as_Value(), ValUsage::Read, cb);
            }
            for(auto& v : e.args)
                rv |= visit_mir_lvalue_mut(v, ValUsage::Move, cb);
            rv |= visit_mir_lvalue_raw_mut(e.ret_val, ValUsage::Write, cb);
            }
        }
        return rv;
    }
    bool visit_mir_lvalues(const ::MIR::Terminator& term, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        return visit_mir_lvalues_mut(const_cast<::MIR::Terminator&>(term), [&](auto& lv, auto im){ return cb(lv, im); });
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

    struct ParamsSet:
        public MonomorphiserPP
    {
        ::HIR::PathParams   impl_params;
        const ::HIR::PathParams*  fcn_params;
        const ::HIR::TypeRef*   self_ty;
        const ::HIR::GenericParams* impl_params_def;
        const ::HIR::GenericParams* fcn_params_def;

        ParamsSet():
            fcn_params(nullptr),
            self_ty(nullptr)
            , impl_params_def(nullptr)
            , fcn_params_def(nullptr)
        {}

        const ::HIR::TypeRef* get_self_type() const override {
            return self_ty;
        }
        const ::HIR::PathParams* get_impl_params() const override {
            return &impl_params;
        }
        const ::HIR::PathParams* get_method_params() const override {
            return fcn_params;
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
            else if( const auto* mir = hir_fcn.m_code.get_mir_opt() ) {
                MIR_ASSERT(state, hir_fcn.m_params.m_types.empty(), "Enumeration failure - Function had params, but wasn't monomorphised - " << path);
                // TODO: Check for trait methods too?
                return mir;
            }
            else {
                MIR_ASSERT(state, !hir_fcn.m_code, "LowerMIR failure - No MIR but HIR is present?! - " << path);
                // External function (no MIR present)
                return nullptr;
            }
        }

        MonomorphState  out_params;
        auto e = state.m_resolve.get_value(state.sp, path, out_params, /*sig_only*/false, &params.impl_params_def);
        DEBUG(e.tag_str() << " " << out_params);
        params.fcn_params = out_params.get_method_params();
        params.impl_params = out_params.pp_impl == nullptr ? ::HIR::PathParams()
            : out_params.pp_impl == &out_params.pp_impl_data ? std::move(out_params.pp_impl_data)
            : out_params.pp_impl->clone()
            ;
        TU_MATCH_HDRA( (path.m_data), {)
        TU_ARMA(Generic, pe) {
            params.self_ty = nullptr;
            }
        TU_ARMA(UfcsKnown, pe) {
            params.self_ty = &pe.type;
            }
        TU_ARMA(UfcsInherent, pe) {
            params.self_ty = &pe.type;
            }
        TU_ARMA(UfcsUnknown, pe) {
            MIR_BUG(state, "UfcsUnknown hit - " << path);
            }
        }

        TU_MATCH_HDRA( (e), { )
        default:
            MIR_BUG(state, "MIR Call of " << e.tag_str() << " - " << path);
        TU_ARMA(NotFound, _) {
            return nullptr;
            }
        TU_ARMA(NotYetKnown, _) {
            return nullptr;
            }
        TU_ARMA(Function, f) {
            params.fcn_params_def = &f->m_params;
            return f->m_code.get_mir_opt();
            }
        }
        return nullptr;
    }


    void visit_terminator_target_mut(::MIR::Terminator& term, ::std::function<void(::MIR::BasicBlockId&)> cb) {
        TU_MATCH_HDRA( (term), {)
        TU_ARMA(Incomplete, e) {
            }
        TU_ARMA(Return, e) {
            }
        TU_ARMA(Diverge, e) {
            }
        TU_ARMA(Goto, e) {
            cb(e);
            }
        TU_ARMA(Panic, e) {
            cb(e.dst);
            }
        TU_ARMA(If, e) {
            cb(e.bb0);
            cb(e.bb1);
            }
        TU_ARMA(Switch, e) {
            for(auto& target : e.targets)
                cb(target);
            }
        TU_ARMA(SwitchValue, e) {
            for(auto& target : e.targets)
                cb(target);
            cb(e.def_target);
            }
        TU_ARMA(Call, e) {
            cb(e.ret_block);
            cb(e.panic_block);
            }
        }
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


    /// Convert a MIR::Param into a MIR::RValue
    MIR::RValue param_to_rvalue(MIR::Param param)
    {
        TU_MATCH_HDRA( (param), { )
        TU_ARMA(LValue, lv) {
            return mv$(lv);
            }
        TU_ARMA(Borrow, e) {
            return ::MIR::RValue::make_Borrow({ e.type, mv$(e.val) });
            }
        TU_ARMA(Constant, c) {
            return mv$(c);
            }
        }
        throw std::runtime_error("Corrupted MIR::Param");
    }
} // namespace ""


// --------------------------------------------------------------------
// Performs basic simplications on the call graph (merging/removing blocks)
// --------------------------------------------------------------------
bool MIR_Optimise_BlockSimplify(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);

    struct H {
        static ::MIR::BasicBlockId get_new_target(const ::MIR::TypeResolve& state, ::MIR::BasicBlockId bb)
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
                // Make sure we don't infinite loop (TODO: What about mutual recursion?)
                if( bb == target.terminator.as_Goto() )
                    return bb;

                return get_new_target(state, target.terminator.as_Goto());
            }
        }
    };

    // >> Replace targets that point to a block that is just a goto
    for(auto& block : fcn.blocks)
    {
        visit_terminator_target_mut(block.terminator, [&](auto& e) {
            if( &fcn.blocks[e] != &block )
            {
                auto new_bb = H::get_new_target(state, e);
                if( new_bb != e )
                {
                    DEBUG("BB" << &block - fcn.blocks.data() << "/TERM: Rewrite bb reference " << e << " => " << new_bb);
                    e = new_bb;
                    changed = true;
                }
            }
            });

        // Handle chained switches of the same value
        // - Happens in libcore's atomics
        if( auto* te = block.terminator.opt_Switch() )
        {
            for(auto& t : te->targets)
            {
                auto idx = &t - &te->targets.front();
                // The block must be a terminator only, and be a switch over the same value.
                if( fcn.blocks[t].statements.empty() && fcn.blocks[t].terminator.is_Switch() )
                {
                    const auto& n_te = fcn.blocks[t].terminator.as_Switch();
                    if( n_te.val == te->val )
                    {
                        // If that's the case, then update this target with the equivalent from the new switch.
                        DEBUG("BB" << &block - fcn.blocks.data() << "/TERM: Update switch from BB" << t << " to BB" << n_te.targets[idx]);
                        t = n_te.targets[idx];
                        changed = true;
                    }
                }
            }
        }
    }

    // >> Unify sequential `ScopeEnd` statements
    for(auto& block : fcn.blocks)
    {
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
                    changed = true;
                }
                else
                {
                    ++ it;
                }
            }
        }
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
                    changed = true;
                }
            }
            i ++;
        }
    }

    // >> If a block GOTOs a block that is just a `RETURN` or `DIVERGE`, then change terminator
    for(auto& block : fcn.blocks)
    {
        state.set_cur_stmt_term(&block - &fcn.blocks.front());
        if(block.terminator.is_Goto())
        {
            auto tgt = block.terminator.as_Goto();
            if( !fcn.blocks[tgt].statements.empty() ) {
            }
            else if( fcn.blocks[tgt].terminator.is_Return() ) {
                DEBUG(state << " -> Return");
                block.terminator = MIR::Terminator::make_Return({});
                changed = true;
            }
            else if( fcn.blocks[tgt].terminator.is_Diverge() ) {
                DEBUG(state << " -> Diverge");
                block.terminator = MIR::Terminator::make_Diverge({});
                changed = true;
            }
            else {
                // No replace
            }
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
    struct InlineEvent {
        ::HIR::Path path;
        ::std::vector<size_t>   bb_list;
        InlineEvent(::HIR::Path p)
            :path(::std::move(p))
        {
        }
        bool has_bb(size_t i) const {
            return ::std::find(this->bb_list.begin(), this->bb_list.end(), i) != this->bb_list.end();
        }
        void add_range(size_t start, size_t count) {
            for(size_t j = 0; j < count; j++)
            {
                this->bb_list.push_back(start + j);
            }
        }
    };
    ::std::vector<InlineEvent>  inlined_functions;

    struct H
    {
        struct Source {
            unsigned    bb_idx;
            unsigned    stmt_idx;
            const ::MIR::Statement* stmt;

            Source( unsigned bb_idx, unsigned stmt_idx, const ::MIR::Statement* stmt = nullptr)
                : bb_idx(bb_idx)
                , stmt_idx(stmt_idx)
                , stmt(stmt)
            {
            }
        };
        static Source find_source(const ::MIR::Function& fcn, unsigned bb_idx, unsigned stmt_idx, const ::MIR::LValue& val)
        {
            if(!val.m_wrappers.empty())
                return Source(bb_idx, stmt_idx);
            const auto& bb = fcn.blocks.at(bb_idx);
            while(stmt_idx --)
            {
                const auto& stmt = bb.statements[stmt_idx];
                if( stmt.is_Asm() )
                    return Source(bb_idx, stmt_idx);
                if( stmt.is_Assign() )
                {
                    const auto& se = stmt.as_Assign();
                    if( se.dst == val )
                    {
                        return Source(bb_idx, stmt_idx, &stmt);
                    }
                }
            }
            return Source(bb_idx, 0);
        }
        /// Checks if the passed lvalue would optimise/expand to a constant value
        static bool value_is_const(const ::MIR::Function& fcn, unsigned bb_idx, unsigned stmt_idx, const ::MIR::LValue& val, const std::vector<::MIR::Param>& params)
        {
            if(val.m_root.is_Argument())
            {
                auto a = val.m_root.as_Argument();
                return params[a].is_Constant() && !params[a].as_Constant().is_Const();
            }

            // Find the source of this lvalue, chase it backwards
            auto src = H::find_source(fcn, bb_idx, stmt_idx, val);
            if(src.stmt)
            {
                if( const auto* se = src.stmt->opt_Assign() )
                {
                    if( se->src.is_Use() ) {
                        return value_is_const(fcn, src.bb_idx, src.stmt_idx, se->src.as_Use(), params);
                    }
                    if(const auto* rve = se->src.opt_BinOp())
                    {
                        return value_is_const(fcn, src.bb_idx, src.stmt_idx, rve->val_l, params)
                            && value_is_const(fcn, src.bb_idx, src.stmt_idx, rve->val_r, params);
                    }
                }
            }

            return false;
        }
        static bool value_is_const(const ::MIR::Function& fcn, unsigned bb_idx, unsigned stmt_idx, const ::MIR::Param& val, const std::vector<::MIR::Param>& params)
        {
            if( val.is_LValue() ) {
                return value_is_const(fcn, bb_idx, stmt_idx, val.as_LValue(), params);
            }
            else {
                return val.is_Constant() && !val.as_Constant().is_Const();
            }
        }
        static bool can_inline(const ::HIR::Path& path, const ::MIR::Function& fcn, const std::vector<::MIR::Param>& params, bool minimal)
        {
            // TODO: If the function is marked as `inline(always)`, then inline it regardless of the contents
            // TODO: If the function is marked as `inline(never)`, then don't inline
            // TODO: Take a monomorph helper so recursion can be detected

            if( minimal ) {
                return false;
            }


            // TODO: Allow functions that are just a switch on an input.
            if( fcn.blocks.size() == 1 )
            {
                return fcn.blocks[0].statements.size() < 10 && ! fcn.blocks[0].terminator.is_Goto();
            }
            else if( fcn.blocks.size() == 2 && fcn.blocks[0].terminator.is_Call() )
            {
                const auto& blk0_te = fcn.blocks[0].terminator.as_Call();
                if( !fcn.blocks[1].terminator.is_Diverge() )
                    return false;
                if( fcn.blocks[0].statements.size() + fcn.blocks[1].statements.size() > 10 )
                    return false;
                // Detect and avoid simple recursion.
                // - This won't detect mutual recursion - that also needs prevention.
                // TODO: This is the pre-monomorph path, but we're comparing with the post-monomorph path
                if( blk0_te.fcn.is_Path() && blk0_te.fcn.as_Path() == path )
                    return false;
                return true;
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
                // TODO: This is the pre-monomorph path, but we're comparing with the post-monomorph path
                if( blk0_te.fcn.is_Path() && blk0_te.fcn.as_Path() == path )
                    return false;
                return true;
            }
            else
            {
            }

            if( can_inline_Switch_wrapper(path, fcn, params) )
                return true;
            if( can_inline_SwitchValue_wrapper(path, fcn, params) )
                return true;
            return false;
        }

        /// Case: A Switch that has all distinct arms that just call a function AND the value is over (effectively) a literal
        static bool can_inline_Switch_wrapper(const ::HIR::Path& path, const ::MIR::Function& fcn, const std::vector<::MIR::Param>& params)
        {
            if( fcn.blocks.size() <= 1)
                return false;
            if( !fcn.blocks[0].terminator.is_Switch() )
                return false;
            const auto& te_switch = fcn.blocks[0].terminator.as_Switch();
            // Setup + Arms + Return + Panic
            // - Handles the atomic wrappers
            if( fcn.blocks.size() != te_switch.targets.size()+3 )
                return false;
            // Check for the switch value being an argument that is also a constant parameter being a Constant
            if( !value_is_const(fcn, 0, fcn.blocks[0].statements.size(), te_switch.val, params) )
                return false;
            // Check all arms of the switch are distinct
            for(const auto& tgt : te_switch.targets)
                if( std::find(te_switch.targets.begin() + (1 + &tgt - te_switch.targets.data()), te_switch.targets.end(), tgt) != te_switch.targets.end() )
                    return false;
            // Check for recursion
            for(size_t i = 1; i < fcn.blocks.size(); i ++)
            {
                if( fcn.blocks[i].terminator.is_Call() )
                {
                    const auto& te = fcn.blocks[i].terminator.as_Call();
                    // Recursion, don't inline.
                    if( te.fcn.is_Path() && te.fcn.as_Path() == path )
                        return false;
                    // HACK: Only allow if the wrapped function is an intrinsic
                    // - Works around the TODO about monomorphed paths above
                    if(!te.fcn.is_Intrinsic())
                        return false;
                }
            }
            return true;
        }

        /// Case: A SwitchValue that has all distinct arms that just call a function AND the value is over (effectively) a literal
        static bool can_inline_SwitchValue_wrapper(const ::HIR::Path& path, const ::MIR::Function& fcn, const std::vector<::MIR::Param>& params)
        {
            if( fcn.blocks.size() <= 1)
                return false;
            if( !fcn.blocks[0].terminator.is_SwitchValue() )
                return false;
            const auto& te_switch = fcn.blocks[0].terminator.as_SwitchValue();
            // Setup + Arms(+default) + Return + Panic
            // - Handles some code in crc32-fast that emits a 256-arm SwitchValue
            if( fcn.blocks.size() != te_switch.targets.size()+1+3 )
                return false;
            // Check for the switch value being an argument that is also a constant parameter being a Constant
            if( !value_is_const(fcn, 0, fcn.blocks[0].statements.size(), te_switch.val, params) )
                return false;

            // Check all arms of the switch are distinct
            if( std::find(te_switch.targets.begin(), te_switch.targets.end(), te_switch.def_target) != te_switch.targets.end() )
                return false;
            for(const auto& tgt : te_switch.targets)
                if( std::find(te_switch.targets.begin() + (1 + &tgt - te_switch.targets.data()), te_switch.targets.end(), tgt) != te_switch.targets.end() )
                    return false;

            // Check for recursion
            for(size_t i = 1; i < fcn.blocks.size(); i ++)
            {
                if( fcn.blocks[i].terminator.is_Call() )
                {
                    const auto& te = fcn.blocks[i].terminator.as_Call();
                    // Recursion, don't inline.
                    if( te.fcn.is_Path() && te.fcn.as_Path() == path )
                        return false;
                    // HACK: Only allow if the wrapped function is an intrinsic
                    // - Works around the TODO about monomorphed paths above
                    if(!te.fcn.is_Intrinsic())
                        return false;
                }
            }
            return true;
        }
    };
    // TODO: Can this use the code in `monomorphise.cpp`?
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
            auto rv = params.monomorph_type(sp, ty);
            resolve.expand_associated_types(sp, rv);
            return rv;
        }
        ::HIR::GenericPath monomorph(const ::HIR::GenericPath& ty) const {
            TRACE_FUNCTION_F(ty);
            auto rv = params.monomorph_genericpath(sp, ty, false);
            for(auto& arg : rv.m_params.m_types)
                resolve.expand_associated_types(sp, arg);
            return rv;
        }
        ::HIR::Path monomorph(const ::HIR::Path& ty) const {
            TRACE_FUNCTION_F(ty);
            auto rv = params.monomorph_path(sp, ty, false);
            TU_MATCH(::HIR::Path::Data, (rv.m_data), (e2),
            (Generic,
                for(auto& arg : e2.m_params.m_types)
                    resolve.expand_associated_types(sp, arg);
                ),
            (UfcsInherent,
                resolve.expand_associated_types(sp, e2.type);
                for(auto& arg : e2.params.m_types)
                    resolve.expand_associated_types(sp, arg);
                // TODO: impl params too?
                for(auto& arg : e2.impl_params.m_types)
                    resolve.expand_associated_types(sp, arg);
                ),
            (UfcsKnown,
                resolve.expand_associated_types(sp, e2.type);
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
            auto rv = params.monomorph_path_params(sp, ty, false);
            for(auto& arg : rv.m_types)
                resolve.expand_associated_types(sp, arg);
            return rv;
        }

        ::std::vector<MIR::AsmParam>    clone_asm_params(const ::std::vector<MIR::AsmParam>& params) const
        {
            ::std::vector<MIR::AsmParam>    rv;
            for(const auto& p : params)
            {
                TU_MATCH_HDRA((p), {)
                TU_ARMA(Const, v)
                    rv.push_back( this->clone_constant(v) );
                TU_ARMA(Sym, v)
                    rv.push_back( this->monomorph(v) );
                TU_ARMA(Reg, v)
                    rv.push_back(::MIR::AsmParam::make_Reg({
                        v.dir,
                        v.spec.clone(),
                        v.input  ? box$(this->clone_param(*v.input)) : std::unique_ptr<MIR::Param>(),
                        v.output ? box$(this->clone_lval(*v.output)) : std::unique_ptr<MIR::LValue>()
                        }));
                }
            }
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
                (Asm2,
                    rv.statements.push_back( ::MIR::Statement::make_Asm2({
                        se.options,
                        se.lines,
                        this->clone_asm_params(se.params)
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
            auto wrappers = src.m_wrappers;
            for(auto& w : wrappers)
            {
                if( w.is_Index() ) {
                    w = ::MIR::LValue::Wrapper::new_Index( this->var_base + w.as_Index() );
                }
            }
            TU_MATCHA( (src.m_root), (se),
            (Return,
                return this->retval.clone_wrapped( mv$(wrappers) );
                ),
            (Argument,
                const auto& arg = this->te.args.at(se);
                if( this->copy_args[se] != ~0u )
                {
                    return ::MIR::LValue( ::MIR::LValue::Storage::new_Local(this->copy_args[se]), mv$(wrappers) );
                }
                else
                {
                    assert( !arg.is_Constant() );   // Should have been handled in the above
                    return arg.as_LValue().clone_wrapped( mv$(wrappers) );
                }
                ),
            (Local,
                return ::MIR::LValue( ::MIR::LValue::Storage::new_Local(this->var_base + se), mv$(wrappers) );
                ),
            (Static,
                return ::MIR::LValue( ::MIR::LValue::Storage::new_Static(this->monomorph(se)), mv$(wrappers) );
                )
            )
            throw "";
        }
        ::MIR::Constant clone_constant(const ::MIR::Constant& src) const
        {
            TU_MATCH_HDRA( (src), {)
            TU_ARMA(Int  , ce) return ::MIR::Constant(ce);
            TU_ARMA(Uint , ce) return ::MIR::Constant(ce);
            TU_ARMA(Float, ce) return ::MIR::Constant(ce);
            TU_ARMA(Bool , ce) return ::MIR::Constant(ce);
            TU_ARMA(Bytes, ce) return ::MIR::Constant(ce);
            TU_ARMA(StaticString, ce) return ::MIR::Constant(ce);
            TU_ARMA(Const, ce) {
                return ::MIR::Constant::make_Const({ box$(this->monomorph(*ce.p)) });
                }
            TU_ARMA(Generic, ce) {
                const HIR::GenericParams* p;
                switch(ce.group())
                {
                case 0: // impl level
                    p = params.impl_params_def;
                    break;
                case 1: // method level
                    p = params.fcn_params_def;
                    break;
                default:
                    TODO(sp, "Typecheck const generics - look up the type");
                }
                ASSERT_BUG(sp, p, "No generic list for " << ce);
                ASSERT_BUG(sp, ce.idx() < p->m_values.size(), "Generic param index out of range");
                const auto& ty = p->m_values.at(ce.idx()).m_type;

                auto val = params.get_value(sp, ce);
                TU_MATCH_HDRA( (val), {)
                default:
                    TODO(sp, "Monomorphise MIR generic constant " << ce << " = " << val);
                TU_ARMA(Generic, ve) {
                    return ve;
                    }
                TU_ARMA(Evaluated, ve) {
                    auto v = EncodedLiteralSlice(*ve);
                    ASSERT_BUG(sp, ty.data().is_Primitive(), "Handle non-primitive const generic: " << ty);
                    // TODO: This is duplicated in `mir/from_hir_match.cpp` - De-duplicate?
                    switch(ty.data().as_Primitive())
                    {
                    case ::HIR::CoreType::Bool: return ::MIR::Constant::make_Bool({ v.read_uint(1) == 0 });
                    case ::HIR::CoreType::U8:   return ::MIR::Constant::make_Uint({ v.read_uint(1), ty.data().as_Primitive() });
                    case ::HIR::CoreType::U16:  return ::MIR::Constant::make_Uint({ v.read_uint(2), ty.data().as_Primitive() });
                    case ::HIR::CoreType::U32:  return ::MIR::Constant::make_Uint({ v.read_uint(4), ty.data().as_Primitive() });
                    case ::HIR::CoreType::U64:  return ::MIR::Constant::make_Uint({ v.read_uint(8), ty.data().as_Primitive() });
                    case ::HIR::CoreType::Usize:  return ::MIR::Constant::make_Uint({ v.read_uint(Target_GetPointerBits() / 8), ty.data().as_Primitive() });
                    case ::HIR::CoreType::U128:  TODO(sp, "u128 const generic");
                    case ::HIR::CoreType::I8:   return ::MIR::Constant::make_Int({ v.read_sint(1), ty.data().as_Primitive() });
                    case ::HIR::CoreType::I16:  return ::MIR::Constant::make_Int({ v.read_sint(2), ty.data().as_Primitive() });
                    case ::HIR::CoreType::I32:  return ::MIR::Constant::make_Int({ v.read_sint(4), ty.data().as_Primitive() });
                    case ::HIR::CoreType::I64:  return ::MIR::Constant::make_Int({ v.read_sint(8), ty.data().as_Primitive() });
                    case ::HIR::CoreType::Isize:  return ::MIR::Constant::make_Int({ v.read_sint(Target_GetPointerBits() / 8), ty.data().as_Primitive() });
                    case ::HIR::CoreType::I128:  TODO(sp, "i128 const generic");
                    case ::HIR::CoreType::F32:  return ::MIR::Constant::make_Float({ v.read_float(4), ty.data().as_Primitive() });
                    case ::HIR::CoreType::F64:  return ::MIR::Constant::make_Float({ v.read_float(8), ty.data().as_Primitive() });
                    case ::HIR::CoreType::Char: return ::MIR::Constant::make_Uint({ v.read_uint(4), ty.data().as_Primitive() });
                    case ::HIR::CoreType::Str:  BUG(sp, "`str` const generic");
                    }
                    }
                }
                }
            TU_ARMA(ItemAddr, ce) {
                if(!ce)
                    return ::MIR::Constant::make_ItemAddr({});
                return ::MIR::Constant::make_ItemAddr(box$(this->monomorph(*ce)));
                }
            }
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
            (Borrow,
                return ::MIR::Param::make_Borrow({ se.type, this->clone_lval(se.val) });
                ),
            (Constant,
                return clone_constant(se);
                )
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
                return ::MIR::RValue::make_SizedArray({ this->clone_param(se.val), params.monomorph_arraysize(sp, se.count) });
                ),
            (Borrow,
                // TODO: Region IDs
                return ::MIR::RValue::make_Borrow({ se.type, this->clone_lval(se.val) });
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
            (UnionVariant,
                return ::MIR::RValue::make_UnionVariant({ this->monomorph(se.path), se.index, this->clone_param(se.val) });
                ),
            (EnumVariant,
                return ::MIR::RValue::make_EnumVariant({ this->monomorph(se.path), se.index, this->clone_param_vec(se.vals) });
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
            DEBUG(state << fcn.blocks[i].terminator);

            for(const auto& e : inlined_functions)
            {
                if( path == e.path && e.has_bb(i) )
                {
                    MIR_BUG(state, "Recursive inline of " << path);
                }
            }

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
            if( ! H::can_inline(path, *called_mir, te->args, minimal) )
            {
                DEBUG("Can't inline " << path);
                continue ;
            }
            TRACE_FUNCTION_F("Inline " << path);

            // Allocate a temporary for the return value
            {
                cloner.retval = ::MIR::LValue::new_Local( fcn.locals.size() );
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
                auto lv = ::MIR::LValue::new_Local( static_cast<unsigned>(fcn.locals.size()) );
                fcn.locals.push_back( mv$(ty) );
                auto rval = val.is_Constant() ? ::MIR::RValue(mv$(val.as_Constant())) : ::MIR::RValue( mv$(val.as_LValue()) );
                auto stmt = ::MIR::Statement::make_Assign({ mv$(lv), mv$(rval) });
                DEBUG("++ " << stmt);
                new_blocks[0].statements.insert( new_blocks[0].statements.begin(), mv$(stmt) );
            }
            cloner.const_assignments.clear();

            // Record the inline event
            for(auto& e : inlined_functions)
            {
                if( e.has_bb(i) )
                {
                    e.add_range(cloner.bb_base, new_blocks.size());
                }
            }
            inlined_functions.push_back(InlineEvent(path.clone()));
            inlined_functions.back().add_range(cloner.bb_base, new_blocks.size());

            // Apply
            DEBUG("- Append new blocks");
            fcn.blocks.reserve( fcn.blocks.size() + new_blocks.size() );
            for(auto& b : new_blocks)
            {
                fcn.blocks.push_back( mv$(b) );
            }
            fcn.blocks[i].terminator = ::MIR::Terminator::make_Goto( cloner.bb_base );
            inline_happened = true;

            // TODO: Store the inlined path along with the start and end BBs, and then use that to detect recursive
            // inlining
            // - Recursive inlining should be an immediate panic.
        }
    }
    return inline_happened;
}

namespace {
    struct StmtRef {
        unsigned    bb_idx;
        unsigned    stmt_idx;
        StmtRef(): bb_idx(~0u), stmt_idx(0) {}
        StmtRef(unsigned b, unsigned s): bb_idx(b), stmt_idx(s) {}
    };
    ::std::ostream& operator<<(::std::ostream& os, const StmtRef& x) {
        return os << "BB" << x.bb_idx << "/" << x.stmt_idx;
    }

    // Iterates the path between two positions, NOT visiting entry specified by `end`
    enum class IterPathRes {
        Abort,
        EarlyTrue,
        Complete,
    };
    IterPathRes iter_path(
            const ::MIR::Function& fcn, const StmtRef& start, const StmtRef& end,
            ::std::function<bool(StmtRef, const ::MIR::Statement&)> cb_stmt,
            ::std::function<bool(StmtRef, const ::MIR::Terminator&)> cb_term
            )
    {
        if( start.bb_idx == end.bb_idx ) {
            assert(start.stmt_idx <= end.stmt_idx);
        }

        auto visted_bbs = ::std::set<unsigned>();
        // Loop while not equal (either not in the right block, or before the statement) to the end point
        for(auto ref = start; ref.bb_idx != end.bb_idx || ref.stmt_idx < end.stmt_idx; )
        {
            const auto& bb = fcn.blocks.at(ref.bb_idx);
            if( ref.stmt_idx < bb.statements.size() )
            {
                DEBUG(ref << " " << bb.statements.at(ref.stmt_idx));
                if( cb_stmt(ref, bb.statements.at(ref.stmt_idx)) )
                {
                    DEBUG("> Early true");
                    return IterPathRes::EarlyTrue;
                }

                ref.stmt_idx ++;
            }
            else
            {
                DEBUG(ref << " " << bb.terminator);
                if( cb_term(ref, bb.terminator) )
                {
                    DEBUG("> Early true");
                    return IterPathRes::EarlyTrue;
                }

                // If this is the end point, break out before checking the terminator for looping
                if( ref.bb_idx == end.bb_idx )
                {
                    // ^ don't need to check the statment index, this is the last "statement"
                    break;
                }

                // If this terminator is a Goto, follow it (tracking for loops)
                if( const auto* te = bb.terminator.opt_Goto() )
                {
                    // Possibly loop into the next block
                    if( !visted_bbs.insert(*te).second ) {
                        DEBUG("> Loop abort");
                        return IterPathRes::Abort;
                    }
                    ref.stmt_idx = 0;
                    ref.bb_idx = *te;
                }
                // If it's a call, check that the target block ends with Diverge, and iterate that in-place
                // - Then follow the success path as usual
                else if( const auto* te = bb.terminator.opt_Call() )
                {
                    // Check the panic arm (should just be a list of destructor calls follwed by a Diverge terminator)
                    const auto& panic_bb = fcn.blocks[te->panic_block];
                    ASSERT_BUG(Span(), panic_bb.terminator.is_Diverge(), "Panic arm of call does not end with Diverge");
                    if( !panic_bb.statements.empty() )
                    {
                        TODO(Span(), "Visit call panic block");
                    }
                    // Possibly loop into the next block
                    if( !visted_bbs.insert(te->ret_block).second ) {
                        DEBUG("> Loop abort");
                        return IterPathRes::Abort;
                    }
                    ref.stmt_idx = 0;
                    ref.bb_idx = te->ret_block;
                }
                else
                {
                    DEBUG("> Terminator abort");
                    return IterPathRes::Abort;
                }
            }
        }
        return IterPathRes::Complete;
    }

    ::std::function<bool(const ::MIR::LValue& , ValUsage)> check_invalidates_lvalue_cb(const ::MIR::LValue& val, bool also_read=false)
    {
        bool has_index = ::std::any_of(val.m_wrappers.begin(), val.m_wrappers.end(), [](const auto& w){ return w.is_Index(); });
        // Value is invalidated if it's used with ValUsage::Write or ValUsage::Borrow
        // - Same applies to any component of the lvalue
        return [&val,has_index,also_read](const ::MIR::LValue& lv, ValUsage vu) {
            switch(vu)
            {
            case ValUsage::Move:    // A move can invalidate
                // - Ideally this would check if it DOES invalidate
            case ValUsage::Write:
            case ValUsage::Borrow:
                // (Possibly) mutating use, check if it impacts the root or one of the indexes
                if( lv.m_root == val.m_root ) {
                    return true;
                }
                // If the desired lvalue has an index in it's wrappers, AND the current lvalue is a local
                if( has_index && lv.m_root.is_Local() )
                {
                    // Search for any wrapper on `val` that Index(lv)
                    for(const auto& w : val.m_wrappers)
                    {
                        if( w.is_Index() && w.as_Index() == lv.m_root.as_Local() )
                        {
                            // This lvalue is changed, so the index is invalidated
                            return true;
                        }
                    }
                }
                break;
            case ValUsage::Read:
                if( also_read )
                {
                    // NOTE: A read of the same root is a read of this value (what if they're disjoint fields?)
                    if( lv.m_root == val.m_root ) {
                        return true;
                    }
                }
                break;
            }
            return false;
            };
    }
    bool check_invalidates_lvalue(const ::MIR::Statement& stmt, const ::MIR::LValue& val, bool also_read=false)
    {
        return visit_mir_lvalues(stmt, check_invalidates_lvalue_cb(val, also_read));
    }
    bool check_invalidates_lvalue(const ::MIR::Terminator& term, const ::MIR::LValue& val, bool also_read=false)
    {
        return visit_mir_lvalues(term, check_invalidates_lvalue_cb(val, also_read));
    }
}

// --------------------------------------------------------------------
// Locates locals that are only set/used once, and replaces them with
//  their source IF the source isn't invalidated
// --------------------------------------------------------------------
bool MIR_Optimise_DeTemporary_SingleSetAndUse(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);

    // Find all single-use/single-write locals
    // - IF the usage is a RValue::Use, AND the usage destination is not invalidated between set/use
    //  - Replace initialisation destination with usage destination (delete usage statement)
    // - IF the source a Use/Constant, AND is not invalidated between set/use
    //  - Replace usage with the original source
    struct LocalUsage {
        unsigned    n_write;
        unsigned    n_read;
        unsigned    n_borrow;
        StmtRef set_loc;
        StmtRef use_loc;
        LocalUsage():
            n_write(0),
            n_read(0),
            n_borrow(0)
        {
        }
    };
    auto usage_info = ::std::vector<LocalUsage>(fcn.locals.size());

    // 1. Enumrate usage
    {
        auto get_cur_loc = [&state]() {
            return StmtRef(state.get_cur_block(), state.get_cur_stmt_ofs());
            };
        auto visit_cb = [&](const ::MIR::LValue& lv, auto vu) {
            if( !lv.m_wrappers.empty() ) {
                vu = ValUsage::Read;
            }
            for(const auto& w : lv.m_wrappers)
            {
                if(w.is_Index())
                {
                    auto& slot = usage_info[w.as_Index()];
                    slot.n_read += 1;
                    slot.use_loc = get_cur_loc();
                    //DEBUG(lv << " index use");
                }
            }
            if( lv.m_root.is_Local() )
            {
                auto& slot = usage_info[lv.m_root.as_Local()];
                switch(vu)
                {
                case ValUsage::Write:
                    slot.n_write += 1;
                    slot.set_loc = get_cur_loc();
                    //DEBUG(lv << " set");
                    break;
                case ValUsage::Move:
                    slot.n_read += 1;
                    slot.use_loc = get_cur_loc();
                    //DEBUG(lv << " use");
                    break;
                case ValUsage::Read:
                case ValUsage::Borrow:
                    slot.n_borrow += 1;
                    //DEBUG(lv << " borrow");
                    break;
                }
            }
            return false;
            };
        visit_mir_lvalues(state, fcn, visit_cb);
    }

    // 2. Find any local with 1 write, 1 read, and no borrows
    for(size_t var_idx = 0; var_idx < fcn.locals.size(); var_idx ++)
    {
        const auto& slot = usage_info[var_idx];
        auto this_var = ::MIR::LValue::new_Local(var_idx);
        //ASSERT_BUG(Span(), slot.n_write > 0, "Variable " << var_idx << " not written?");
        if( slot.n_write == 1 && slot.n_read == 1 && slot.n_borrow == 0 )
        {
            // Single-use variable, now check how we can eliminate it
            DEBUG("Single-use: _" << var_idx << " - Set " << slot.set_loc << ", Use " << slot.use_loc);

            auto& use_bb = fcn.blocks[slot.use_loc.bb_idx];
            auto& set_bb = fcn.blocks[slot.set_loc.bb_idx];

            auto set_loc_next = slot.set_loc;
            if( slot.set_loc.stmt_idx < set_bb.statements.size() )
            {
                set_loc_next.stmt_idx += 1;
            }
            else
            {
                set_loc_next.bb_idx = set_bb.terminator.as_Call().ret_block;
                set_loc_next.stmt_idx = 0;
            }

            // If usage is direct assignment of the original value.
            // - In this case, we can move the usage upwards
            if( slot.use_loc.stmt_idx < use_bb.statements.size() && TU_TEST2(use_bb.statements[slot.use_loc.stmt_idx], Assign, .src, Use, == this_var) )
            {
                // Move the usage up to original assignment (if destination isn't invalidated)
                const auto& dst = use_bb.statements[slot.use_loc.stmt_idx].as_Assign().dst;

                // TODO: If the destination slot was ever borrowed mutably, don't move.
                // - Maybe, if there's a drop skip? (as the drop could be &mut to the target value)

                // - Iterate the path(s) between the two statements to check if the destination would be invalidated
                //  > The iterate function doesn't (yet) support following BB chains, so assume invalidated if over a jump.
                // TODO: What if the set location is a call?
                bool invalidated = IterPathRes::Complete != iter_path(fcn, set_loc_next, slot.use_loc,
                        // TODO: What about a mutable borrow?
                        [&](auto loc, const auto& stmt)->bool{ return stmt.is_Drop() || check_invalidates_lvalue(stmt, dst, /*also_read=*/true); },
                        [&](auto loc, const auto& term)->bool{ return check_invalidates_lvalue(term, dst, /*also_read=*/true); }
                        );
                if( !invalidated )
                {
                    // destination not dependent on any statements between the two, move.
                    if( slot.set_loc.stmt_idx < set_bb.statements.size() )
                    {
                        auto& set_stmt = set_bb.statements[slot.set_loc.stmt_idx];
                        TU_MATCH_HDRA( (set_stmt), {)
                        TU_ARMA(Assign, se) {
                            MIR_ASSERT(state, se.dst == ::MIR::LValue::new_Local(var_idx), "Impossibility: Value set but isn't destination in " << set_stmt);
                            DEBUG("Move destination " << dst << " from " << use_bb.statements[slot.use_loc.stmt_idx] << " to " << set_stmt);
                            se.dst = dst.clone();
                            use_bb.statements[slot.use_loc.stmt_idx] = ::MIR::Statement();
                            changed = true;
                            }
                        TU_ARMA(Asm, se) {
                            // Initialised from an ASM statement, find the variable in the output parameters
                            }
                        TU_ARMA(Asm2, se) {
                            // Initialised from an ASM statement, find the variable in the output parameters
                            // TODO: Replace the output variable
                            for(auto& e : se.params)
                            {
                                if(const auto* ep = e.opt_Reg())
                                {
                                    if( ep->output ) {
                                        if( *ep->output == ::MIR::LValue::new_Local(var_idx) ) {
                                            DEBUG("Move destination " << dst << " from " << use_bb.statements[slot.use_loc.stmt_idx] << " to " << set_stmt);
                                            *ep->output = dst.clone();
                                            use_bb.statements[slot.use_loc.stmt_idx] = ::MIR::Statement();
                                            changed = true;
                                            break;
                                        }
                                    }
                                }
                            }
                            if( !changed ) {
                                MIR_BUG(state, "Failed to find usage of _" << var_idx << " in asm! statement");
                            }
                            }
                            break;
                        default:
                            MIR_BUG(state, "Impossibility: Value set in " << set_stmt);
                        }
                    }
                    else
                    {
                        auto& set_term = set_bb.terminator;
                        MIR_ASSERT(state, set_term.is_Call(), "Impossibility: Value set using non-call");
                        auto& te = set_term.as_Call();
                        DEBUG("Move destination " << dst << " from " << use_bb.statements[slot.use_loc.stmt_idx] << " to " << set_term);
                        te.ret_val = dst.clone();
                        use_bb.statements[slot.use_loc.stmt_idx] = ::MIR::Statement();
                        changed = true;
                    }
                }
                else
                {
                    DEBUG("Destination invalidated");
                }
                continue ;
            }

            // Can't move up, can we move down?
            // - If the source is an Assign(Use) then we can move down
            if( slot.set_loc.stmt_idx < set_bb.statements.size() && TU_TEST1(set_bb.statements[slot.set_loc.stmt_idx], Assign, .src.is_Use()) )
            {
                auto& set_stmt = set_bb.statements[slot.set_loc.stmt_idx];
                const auto& src = set_stmt.as_Assign().src.as_Use();

                // Check if the source of initial assignment is invalidated in the meantime.
                auto use_loc_inc = slot.use_loc;
                use_loc_inc.stmt_idx += 1;
                bool invalidated = IterPathRes::Complete != iter_path(fcn, set_loc_next, use_loc_inc,
                        // NOTE: If a mutable borrow happens, assume it invalidates the source
                        [&](auto loc, const auto& stmt)->bool{ return check_invalidates_lvalue(stmt, src) || TU_TEST2(stmt, Assign, .src, Borrow, .type != HIR::BorrowType::Shared); },
                        [&](auto loc, const auto& term)->bool{ return check_invalidates_lvalue(term, src); }
                        );
                // If this is a deref, and there are move ops between definition and use - then invalidate
                if( !invalidated && std::any_of(src.m_wrappers.begin(), src.m_wrappers.end(), [](const MIR::LValue::Wrapper& w){ return w.is_Deref(); }) )
                {
                    // If there are any move ops between the set and the usage, invalidate
                    bool stop = false;
                    auto check_cb = [&](const MIR::LValue& lv, ValUsage vu){
                        if( lv == this_var ) {
                            stop = true;
                            return false;
                        }
                        if( stop ) {
                            // Once the value is seen, ignore anything else
                            return false;
                        }
                        // If a move is seen, check if it's a move (and not a copy)
                        if( vu == ValUsage::Move ) {
                            return !state.lvalue_is_copy(lv);
                        }
                        return false;
                        };
                    invalidated = IterPathRes::Complete != iter_path(fcn, set_loc_next, use_loc_inc,
                        [&](auto loc, const auto& stmt)->bool{ return visit_mir_lvalues(stmt, check_cb); },
                        [&](auto loc, const auto& term)->bool{ return visit_mir_lvalues(term, check_cb); }
                        );
                }
                if( !invalidated )
                {
                    // Update the usage site and replace.
                    auto replace_cb = [&](::MIR::LValue& slot, ValUsage vu)->bool {
                        if( slot.m_root == this_var.m_root )
                        {
                            if( src.m_wrappers.empty() ) {
                                slot.m_root = src.m_root.clone();
                            }
                            else if( slot.m_wrappers.empty() ) {
                                slot = src.clone();
                            }
                            else {
                                MIR_TODO(state, "Replace inner of " << slot << " with " << src);
                            }
                            return true;
                        }
                        return false;
                        };
                    if( slot.use_loc.stmt_idx < use_bb.statements.size() )
                    {
                        auto& use_stmt = use_bb.statements[slot.use_loc.stmt_idx];
                        DEBUG("Replace " << this_var << " with " << src << " in BB" << slot.use_loc.bb_idx << "/" << slot.use_loc.stmt_idx  << " " << use_stmt);
                        bool found = visit_mir_lvalues_mut(use_stmt, replace_cb);
                        if( !found )
                        {
                            DEBUG("Can't find use of " << this_var << " in " << use_stmt);
                        }
                        else
                        {
                            set_stmt = ::MIR::Statement();
                            changed = true;
                        }
                    }
                    else
                    {
                        auto& use_term = use_bb.terminator;
                        DEBUG("Replace " << this_var << " with " << src << " in " << use_term);
                        bool found = visit_mir_lvalues_mut(use_term, replace_cb);
                        if( !found )
                        {
                            DEBUG("Can't find use of " << this_var << " in " << use_term);
                        }
                        else
                        {
                            set_stmt = ::MIR::Statement();
                            changed = true;
                        }
                    }
                }
                else
                {
                    DEBUG("Source invalidated");
                }
                continue;
            }

            // TODO: If the source is a Borrow and the use is a Deref, then propagate forwards
            // - This would be a simpler version of a var more compliciated algorithm

            DEBUG("Can't replace:");
            if( slot.set_loc.stmt_idx < set_bb.statements.size() )
            {
                DEBUG("Set: " << set_bb.statements[slot.set_loc.stmt_idx]);
            }
            else
            {
                DEBUG("Set: " << set_bb.terminator);
            }
            if( slot.use_loc.stmt_idx < use_bb.statements.size() )
            {
                DEBUG("Use: " << use_bb.statements[slot.use_loc.stmt_idx]);
            }
            else
            {
                DEBUG("Use: " << use_bb.terminator);
            }
        }
    }

    return changed;
}

// Remove useless borrows (locals assigned with a borrow, and never used by value)
// ```
// _$1 = & _$0;
// (*_$1).1 = 0x0;
// ```
bool MIR_Optimise_DeTemporary_Borrows(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
#if 1
    TRACE_FUNCTION_FR("", changed);

    // Find all single-assign borrows that are only ever used via Deref
    // - Direct drop is ignored for this purpose
    struct LocalUsage {
        unsigned    n_write;
        unsigned    n_other_read;
        unsigned    n_deref_read;
        StmtRef set_loc;
        ::std::vector<StmtRef> drop_locs;
        LocalUsage():
            n_write(0),
            n_other_read(0),
            n_deref_read(0)
        {
        }
    };
    auto usage_info = ::std::vector<LocalUsage>(fcn.locals.size());
    for(const auto& bb : fcn.blocks)
    {
        StmtRef cur_loc;
        auto visit_cb = [&](const ::MIR::LValue& lv, auto vu) {
            if( lv.m_root.is_Local() )
            {
                auto& slot = usage_info[lv.m_root.as_Local()];
                // NOTE: This pass doesn't care about indexing, as we're looking for values that are borrows (which aren't valid indexes)
                // > Inner-most wrapper is Deref - it's a deref of this variable
                if( !lv.m_wrappers.empty() && lv.m_wrappers.front().is_Deref() ) {
                    slot.n_deref_read ++;
                    if( fcn.locals[lv.m_root.as_Local()].data().is_Borrow() ) {
                        DEBUG(lv << " deref use " << cur_loc);
                    }
                }
                // > Write with no wrappers - Assignment
                else if( lv.m_wrappers.empty() && vu == ValUsage::Write ) {
                    slot.n_write ++;
                    slot.set_loc = cur_loc;
                    //DEBUG(lv << " set");
                }
                // Anything else, count as a read
                else {
                    slot.n_other_read ++;
                }
            }
            return false;
            };
        for(const auto& stmt : bb.statements)
        {
            cur_loc = StmtRef(&bb - &fcn.blocks.front(), &stmt - &bb.statements.front());

            // If the statement is a drop of a local, then don't count that as a read
            // - But do record the location of the drop, so it can be deleted later on?
            if( stmt.is_Drop() )
            {
                const auto& drop_lv = stmt.as_Drop().slot;
                if( drop_lv.m_root.is_Local() && drop_lv.m_wrappers.empty() )
                {
                    auto& slot = usage_info[drop_lv.m_root.as_Local()];
                    slot.drop_locs.push_back(cur_loc);
                    continue ;
                }
            }

            //DEBUG(cur_loc << ":" << stmt);
            visit_mir_lvalues(stmt, visit_cb);
        }
        cur_loc = StmtRef(&bb - &fcn.blocks.front(), bb.statements.size());
        //DEBUG(cur_loc << ":" << bb.terminator);
        visit_mir_lvalues(bb.terminator, visit_cb);
    }

    // Look single-write/deref-only locals assigned with `_0 = Borrow`
    for(size_t var_idx = 0; var_idx < fcn.locals.size(); var_idx ++)
    {
        const auto& slot = usage_info[var_idx];
        auto this_var = ::MIR::LValue::new_Local(var_idx);

        // This rule only applies to single-write variables, with no use other than via derefs
        if( !(slot.n_write == 1 && slot.n_other_read == 0) )
        {
            //DEBUG(this_var << " - Multi-assign, or use-by-value");
            continue ;
        }
        if( slot.n_deref_read == 0 )
        {
            //DEBUG(this_var << " - Not used");
            continue ;
        }

        // Check that the source was a borrow statement
        auto& src_bb = fcn.blocks[slot.set_loc.bb_idx];
        if( !(slot.set_loc.stmt_idx < src_bb.statements.size() && TU_TEST1(src_bb.statements[slot.set_loc.stmt_idx], Assign, .src.is_Borrow())) )
        {
            DEBUG(this_var << " - Source is not a borrow op");
            continue;
        }
        const auto& src_lv = src_bb.statements[slot.set_loc.stmt_idx].as_Assign().src.as_Borrow().val;
        // Check that the borrow isn't too complex (if it's used multiple times)
        if( slot.n_deref_read > 1 && src_lv.m_wrappers.size() >= 2 )
        {
            DEBUG(this_var << " - Source is too complex - " << src_lv);
            continue;
        }
        if( slot.n_deref_read > 1 && fcn.locals[var_idx].data().as_Borrow().type != ::HIR::BorrowType::Shared )
        {
            DEBUG(this_var << " - Multi-use non-shared borrow, too complex to do");
            continue;
        }
        DEBUG(this_var << " - Borrow of " << src_lv << " at " << slot.set_loc << ", used " << slot.n_deref_read << " times (dropped " << slot.drop_locs << ")");

        // Locate usage sites (by walking forwards) and check for invalidation
        auto cur_loc = slot.set_loc;
        cur_loc.stmt_idx ++;
        unsigned num_replaced = 0;
        auto replace_cb = [&](::MIR::LValue& lv, auto _vu) {
            if( lv.m_root == this_var.m_root )
            {
                ASSERT_BUG(Span(), !lv.m_wrappers.empty(), cur_loc << " " << lv);
                assert(lv.m_wrappers.front().is_Deref());
                // Make a LValue reference, then overwrite it
                {
                    auto lvr = ::MIR::LValue::MRef(lv);
                    while(lvr.wrapper_count() > 1)
                        lvr.try_unwrap();
                    DEBUG(this_var << " " << cur_loc << " - Replace " << lvr << " with " << src_lv << " in " << lv);
                    lvr.replace(src_lv.clone());
                }
                DEBUG("= " << lv);
                assert(lv.m_root != this_var.m_root);
                assert(num_replaced < slot.n_deref_read);
                num_replaced += 1;
            }
            return false;
            };
        for(bool stop = false; !stop; )
        {
            auto& cur_bb = fcn.blocks[cur_loc.bb_idx];
            for(; cur_loc.stmt_idx < cur_bb.statements.size(); cur_loc.stmt_idx ++)
            {
                auto& stmt = cur_bb.statements[cur_loc.stmt_idx];
                DEBUG(cur_loc << " " << stmt);
                // Replace usage
                bool invalidates = check_invalidates_lvalue(stmt, src_lv);
                visit_mir_lvalues_mut(stmt, replace_cb);
                if( num_replaced == slot.n_deref_read )
                {
                    stop = true;
                    break;
                }
                // Check for invalidation (actual check done before replacement)
                if( invalidates )
                {
                    // Invalidated, stop here.
                    DEBUG(this_var << " - Source invalidated @ " << cur_loc << " in " << stmt);
                    stop = true;
                    break;
                }
            }
            if( stop ) {
                break;
            }
            // Replace usage
            visit_mir_lvalues_mut(cur_bb.terminator, replace_cb);
            if( num_replaced == slot.n_deref_read )
            {
                stop = true;
                break;
            }
            // Check for invalidation
            if( check_invalidates_lvalue(cur_bb.terminator, src_lv) )
            {
                DEBUG(this_var << " - Source invalidated @ " << cur_loc << " in " << cur_bb.terminator);
                stop = true;
                break;
            }

            TU_MATCH_HDRA( (cur_bb.terminator), { )
            default:
                stop = true;
                break;
            // TODO: History is needed to avoid infinite loops from triggering infinite looping here.
            //TU_ARMA(Goto, e) {
            //    cur_pos.bb_idx = e;
            //    cur_pos.stmt_idx = 0;
            //    }
            // NOTE: `Call` can't work in the presense of unwinding, would need to traverse both paths
            //TU_ARMA(Call, e) {
            //    }
            }
        }

        // If the source was an inner deref, update its counts
        if( src_lv.m_root.is_Local() && !src_lv.m_wrappers.empty() && src_lv.m_wrappers.front().is_Deref() )
        {
            usage_info[src_lv.m_root.as_Local()].n_deref_read += num_replaced;
            if(num_replaced == slot.n_deref_read)
            {
                usage_info[src_lv.m_root.as_Local()].n_deref_read -= 1;
            }
        }

        // If all usage sites were updated, then remove the original assignment
        if(num_replaced == slot.n_deref_read)
        {
            DEBUG(this_var << " - Erase " << slot.set_loc << " as it is no longer used (" << src_bb.statements[slot.set_loc.stmt_idx] << ")");
            src_bb.statements[slot.set_loc.stmt_idx] = ::MIR::Statement();
            for(const auto& drop_loc : slot.drop_locs)
            {
                DEBUG(this_var << " - Drop at " << drop_loc);
                fcn.blocks[drop_loc.bb_idx].statements[drop_loc.stmt_idx] = ::MIR::Statement();
            }
        }
#if 0
        else if( num_replaced > 0 )
        {
            auto src_rval = ::std::move(src_bb.statements[slot.set_loc.stmt_idx].as_Assign().src);
            src_bb.statements[slot.set_loc.stmt_idx] = ::MIR::Statement();
            DEBUG(this_var << " - Move " << slot.set_loc << " to after " << cur_loc);
            // TODO: Move the source borrow up to this point.
            auto& cur_bb = fcn.blocks[cur_loc.bb_idx];
            if( cur_loc.stmt_idx >= cur_bb.statements.size() )
            {
                auto push_bb_front = [&fcn,&this_var](unsigned b, ::MIR::RValue s){
                    fcn.blocks[b].statements.insert(fcn.blocks[b].statements.begin(), ::MIR::Statement::make_Assign({ this_var.clone(), ::std::move(s) }));
                    // TODO: Update all references to this block?
                    };
                // Move the borrow to the next block?
                // - Terminators shouldn't be able to invalidate...
                TU_MATCH_HDRA( (cur_bb.terminator), { )
                default:
                    TODO(Span(), "Move borrow to after terminator " << cur_bb.terminator);
                TU_ARMA(Goto, e) {
                    push_bb_front(e, ::std::move(src_rval));
                    }
                TU_ARMA(Call, e) {
                    push_bb_front(e.ret_block, src_rval.clone());
                    push_bb_front(e.panic_block, ::std::move(src_rval));
                    }
                }
            }
            else
            {
                // If invalidated, then there _shouldn't_ be more to come (borrow rules)
                TODO(Span(), "Move borrow to after " << cur_loc);
            }
        }
#endif
        else
        {
            // No replacement, keep the source where it is
            DEBUG(this_var << " - Keep " << slot.set_loc);
        }

        // Any replacements? Then there was an actionable change
        if( num_replaced > 0 )
        {
            changed = true;
        }
    }
#endif

    return changed;
}

// --------------------------------------------------------------------
// Replaces uses of stack slots with what they were assigned with (when
// possible)
// --------------------------------------------------------------------
bool MIR_Optimise_DeTemporary(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);

    changed |= MIR_Optimise_DeTemporary_SingleSetAndUse(state, fcn);
    changed |= MIR_Optimise_DeTemporary_Borrows(state, fcn);


    // OLD ALGORITHM.
    for(unsigned int bb_idx = 0; bb_idx < fcn.blocks.size(); bb_idx ++)
    {
        auto& bb = fcn.blocks[bb_idx];
        ::std::map<unsigned,unsigned>   local_assignments;  // Local number -> statement index
        // TODO: Keep track of what variables would invalidate a local (and compound on assignment)
        ::std::vector<unsigned> statements_to_remove;   // List of statements that have to be removed

        // ----- Helper closures -----
        // > Check if a recorded assignment is no longer valid.
        auto cb_check_invalidate = [&](const ::MIR::LValue& lv, ValUsage vu) {
                for(auto it = local_assignments.begin(); it != local_assignments.end(); )
                {
                    bool invalidated = false;
                    const auto& src_rvalue = bb.statements[it->second].as_Assign().src;

                    // Destination invalidated?
                    if( lv.m_root.is_Local() && it->first == lv.m_root.as_Local() )
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
                            visit_mir_lvalues(src_rvalue, [&](const ::MIR::LValue& s_lv, auto s_vu) {
                                //DEBUG("   " << s_lv << " ?= " << lv);
                                if( s_lv.m_root == lv.m_root )
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
            if( top_lv.m_root.is_Local() )
            {
                bool top_level = top_lv.m_wrappers.empty();
                auto ilv = ::MIR::LValue::new_Local(top_lv.m_root.as_Local());
                auto it = local_assignments.find(top_lv.m_root.as_Local());
                if( it != local_assignments.end() )
                {
                    const auto& new_val = bb.statements[it->second].as_Assign().src.as_Use();
                    // - Copy? All is good.
                    if( state.lvalue_is_copy(ilv) )
                    {
                        top_lv = new_val.clone_wrapped(top_lv.m_wrappers.begin(), top_lv.m_wrappers.end());
                        DEBUG(state << "> Replace (and keep) Local(" << it->first << ") with " << new_val);
                        changed = true;
                    }
                    // - Top-level (directly used) also good.
                    else if( top_level && top_usage == ValUsage::Move )
                    {
                        // TODO: DstMeta/DstPtr _doesn't_ move, so shouldn't trigger this.
                        top_lv = new_val.clone();
                        DEBUG(state << "> Replace (and remove) Local(" << it->first << ") with " << new_val);
                        statements_to_remove.push_back( it->second );
                        local_assignments.erase(it);
                        changed = true;
                    }
                    // - Otherwise, remove the record.
                    else
                    {
                        DEBUG(state << "> Non-copy value used within a LValue, remove record of Local(" << it->first << ")");
                        local_assignments.erase(it);
                    }
                }
            }
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
                const auto& dst_lv = stmt.as_Assign().dst;
                const auto& src_lv = stmt.as_Assign().src.as_Use();
                if( visit_mir_lvalues_inner(src_lv, ValUsage::Read, [&](const auto& lv, auto) { return lv.m_root == dst_lv.m_root; }) )
                {
                    DEBUG(state << "> Don't record, self-referrential");
                }
                else if( ::std::any_of(src_lv.m_wrappers.begin(), src_lv.m_wrappers.end(), [](const auto& w){ return w.is_Deref(); }) )
                {
                    DEBUG(state << "> Don't record, dereference");
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

            changed = true;
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
            if( lv.m_root.is_Local() )
            {
                auto it = replacements.find(lv.m_root.as_Local());
                if( it != replacements.end() )
                {
                    MIR_DEBUG(state, lv << " => Local(" << it->second << ")");
                    lv.m_root = ::MIR::LValue::Storage::new_Local(it->second);
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
                TU_MATCH_HDRA( (a.statements[i], b.statements[i]), {)
                TU_ARMA(Assign, ae, be) {
                    if( ae.dst != be.dst )
                        return false;
                    if( ae.src != be.src )
                        return false;
                    }
                TU_ARMA(Asm, ae, be) {
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
                    }
                TU_ARMA(Asm2, ae, be) {
                    if( ae.lines != be.lines )
                        return false;
                    if( !(ae.options == be.options) )
                        return false;
                    if( ae.params != be.params )
                        return false;
                    }
                TU_ARMA(SetDropFlag, ae, be) {
                    if( ae.idx != be.idx )
                        return false;
                    if( ae.new_val != be.new_val )
                        return false;
                    if( ae.other != be.other )
                        return false;
                    }
                TU_ARMA(Drop, ae, be) {
                    if( ae.kind != be.kind )
                        return false;
                    if( ae.flag_idx != be.flag_idx )
                        return false;
                    if( ae.slot != be.slot )
                        return false;
                    }
                TU_ARMA(ScopeEnd, ae, be) {
                    if( ae.slots != be.slots )
                        return false;
                    }
                }
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
                if( ae.values != be.values )
                    return false;
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
//
// TODO: Is this needed now that SplitAggregates exists?
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
                    if( check_invalidates_lvalue(bb.terminator, slot_lvalue) ) {
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
                                    if( check_invalidates_lvalue(bb.terminator, src_lval) ) {
                                        // Invalidated: Return.
                                        return nullptr;
                                    }
                                    continue ;
                                }
                                if( check_invalidates_lvalue(bb.statements[stmt_idx], src_lval) ) {
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
                if( check_invalidates_lvalue(stmt, slot_lvalue) ) {
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
                    if(vu == ValUsage::Read && lv.m_wrappers.size() > 1 && lv.m_wrappers.front().is_Field() && lv.m_root.is_Local())
                    {
                        auto field_index = lv.m_wrappers.front().as_Field();
                        auto inner_lv = ::MIR::LValue::new_Local(lv.m_root.as_Local());
                        auto outer_lv = ::MIR::LValue::new_Field(inner_lv.clone(), field_index);
                        // TODO: This value _must_ be Copy for this optimisation to work.
                        // - OR, it has to somehow invalidate the original tuple
                        DEBUG(state << "Locating origin of " << lv);
                        ::HIR::TypeRef  tmp;
                        if( !state.m_resolve.type_is_copy(state.sp, state.get_lvalue_type(tmp, inner_lv)) )
                        {
                            DEBUG(state << "- not Copy, can't optimise");
                            return false;
                        }
                        const auto* source_lvalue = get_field(inner_lv, field_index, bb_idx, i);
                        if( source_lvalue )
                        {
                            if( outer_lv != *source_lvalue )
                            {
                                DEBUG(state << "Source is " << *source_lvalue);
                                lv = source_lvalue->clone_wrapped( lv.m_wrappers.begin() + 1, lv.m_wrappers.end() );
                                change_happend = true;
                            }
                            else
                            {
                                DEBUG(state << "No change");
                            }
                            return false;
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
bool MIR_Optimise_ConstPropagate(::MIR::TypeResolve& state, ::MIR::Function& fcn)
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
        else if( tef.name == "needs_drop" )
        {
            // Returns `true` if the actual type given as `T` requires drop glue;
            // returns `false` if the actual type provided for `T` implements `Copy`. (Either otherwise)
            // NOTE: libarena assumes that this returns `true` iff T doesn't require drop glue.
            const auto& ty = tef.params.m_types.at(0);
            // - Only expand at this stage if there's no generics, and no unbound paths
            if( !visit_ty_with(ty, [](const ::HIR::TypeRef& ty)->bool{
                    return ty.data().is_Generic() || TU_TEST1(ty.data(), Path, .binding.is_Unbound());
                }) )
            {
                bool needs_drop = state.m_resolve.type_needs_drop_glue(state.sp, ty);
                bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(te.ret_val), ::MIR::RValue::make_Constant(::MIR::Constant::make_Bool({needs_drop})) }));
                bb.terminator = ::MIR::Terminator::make_Goto(te.ret_block);
                changed = true;
            }
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

        auto check_lv = [&](const ::MIR::LValue& lv)->::MIR::Constant {
                auto it = known_values.find(lv);
                if( it != known_values.end() )
                {
                    DEBUG(state << "Value " << lv << " known to be" << it->second);
                    return it->second.clone();
                }

                // TODO: If the inner of the value is known,
                //   AND all indexes are known - expand
                //if( !lv.m_wrappers.empty() )
                //{
                //    it = known_values.find(lv.m_root);
                //    if( it != known_values.end() )
                //    {
                //        // TODO: Use HIR::Literal instead so composites can be handled.
                //        for(const auto& w : lv.m_wrappers)
                //        {
                //        }
                //    }
                //}

                // Not a known value, and not a known composite
                // - Use a nullptr ItemAddr to indicate this
                return ::MIR::Constant::make_ItemAddr({});
            };
        auto check_param = [&](::MIR::Param& p) {
            if(const auto* pe = p.opt_LValue()) {
                auto nv = check_lv(*pe);
                if( nv.is_ItemAddr() && !nv.as_ItemAddr() )
                {
                    // ItemAddr with a nullptr inner means "no expansion"
                }
                else
                {
                    p = mv$(nv);
                    changed = true;
                }
            }
            };

        // Convert known indexes into field acceses
        auto edit_lval = [&](auto& lv, auto _vu)->bool {
            for(auto& w : lv.m_wrappers)
            {
                if( w.is_Index() )
                {
                    auto it = known_values.find(MIR::LValue::new_Local(w.as_Index()));
                    if( it != known_values.find(lv) && !it->second.is_Const() && !it->second.is_Generic() )
                    {
                        MIR_ASSERT(state, it->second.is_Uint(), "Indexing with non-Uint constant - " << it->second);
                        MIR_ASSERT(state, it->second.as_Uint().t == HIR::CoreType::Usize, "Indexing with non-usize constant - " << it->second);
                        auto idx = it->second.as_Uint().v;
                        MIR_ASSERT(state, idx < (1<<30), "Known index is excessively large");
                        w = MIR::LValue::Wrapper::new_Field( idx );
                        changed = true;
                    }
                }
            }
            return true;
            };

        for(auto& stmt : bb.statements)
        {
            auto stmtidx = &stmt - &bb.statements.front();
            state.set_cur_stmt(bbidx, stmtidx);

            visit_mir_lvalues_mut(stmt, edit_lval);

            // Scan statements forwards:
            // - If a known temporary is used as Param::LValue, replace LValue with the value
            // - If a UniOp has its input known, evaluate
            // - If a BinOp has both values known, evaluate
            if( auto* e = stmt.opt_Assign() )
            {
                struct H {
                    static int64_t truncate_s(::HIR::CoreType ct, int64_t v) {
                        // Truncate unsigned, then sign extend
                        return v;
                    }
                    static uint64_t truncate_u(::HIR::CoreType ct, uint64_t v) {
                        switch(ct)
                        {
                        case ::HIR::CoreType::I8:   case ::HIR::CoreType::U8:   return v & 0xFF;
                        case ::HIR::CoreType::I16:  case ::HIR::CoreType::U16:  return v & 0xFFFF;
                        case ::HIR::CoreType::I32:  case ::HIR::CoreType::U32:  return v & 0xFFFFFFFF;
                        case ::HIR::CoreType::I64:  case ::HIR::CoreType::U64:  return v;
                        case ::HIR::CoreType::I128: case ::HIR::CoreType::U128: return v;
                        // usize/size - need to handle <64 pointer bits
                        case ::HIR::CoreType::Isize:
                        case ::HIR::CoreType::Usize:
                            if(Target_GetPointerBits() < 64)
                                return v & (static_cast<uint64_t>(-1) >> (64 - Target_GetPointerBits()));
                            return v;
                        case ::HIR::CoreType::Char:
                            //MIR_BUG(state, "Invalid use of operator on char");
                            break;
                        default:
                            // Invalid type for Uint literal
                            break;
                        }
                        return v;
                    }
                };

                TU_MATCH_HDRA( (e->src), {)
                TU_ARMA(Use, se) {
                    auto nv = check_lv(se);
                    if( nv.is_ItemAddr() && !nv.as_ItemAddr() )
                    {
                        // ItemAddr with a nullptr inner means "no expansion"
                    }
                    else
                    {
                        e->src = ::MIR::RValue::make_Constant(mv$(nv));
                        changed = true;
                    }
                    }
                TU_ARMA(Constant, se) {
                    // Ignore (knowledge done below)
                    }
                TU_ARMA(SizedArray, se) {
                    check_param(se.val);
                    }
                TU_ARMA(Borrow, se) {
                    // Shared borrows of statics can be better represented with the ItemAddr constant
                    if( se.type == HIR::BorrowType::Shared && se.val.m_wrappers.empty() && se.val.m_root.is_Static() )
                    {
                        e->src = ::MIR::RValue::make_Constant( ::MIR::Constant::make_ItemAddr({ box$(se.val.m_root.as_Static()) }) );
                        changed = true;
                    }
                    }
                TU_ARMA(Cast, se) {
                    ::MIR::Constant new_value;

                    // If casting a number to a number, do the cast and 
                    auto nv = check_lv(se.val);
                    if( !nv.is_ItemAddr() )
                    {
                        if(const auto* te = se.type.data().opt_Primitive())
                        {
                            switch(*te)
                            {
                            case ::HIR::CoreType::U8:
                            case ::HIR::CoreType::U16:
                            case ::HIR::CoreType::U32:
                            case ::HIR::CoreType::U64:
                            case ::HIR::CoreType::U128:
                            case ::HIR::CoreType::Usize:
                                if( const auto* vp = nv.opt_Uint() )
                                {
                                    new_value = ::MIR::Constant::make_Uint({
                                        H::truncate_u(*te, vp->v),
                                        *te
                                        });
                                }
                                else if( const auto* vp = nv.opt_Int() )
                                {
                                    if( *te == HIR::CoreType::U128 )
                                    {
                                        // u128 destination, can only cast if the value is positive (otherwise the sign extension is lost)
                                        if( vp->v < 0 ) {
                                            DEBUG(state << "Cast of negative to u128, sign extension would be lost");
                                            break;
                                        }
                                    }
                                    new_value = ::MIR::Constant::make_Uint({
                                        H::truncate_u(*te, vp->v),
                                        *te
                                        });
                                }
                                else if( const auto* vp = nv.opt_Bool() )
                                {
                                    new_value = ::MIR::Constant::make_Uint({
                                        (vp->v ? 1u : 0u),
                                        *te
                                        });
                                }
                                else
                                {
                                }
                                break;
                            case ::HIR::CoreType::I8:
                            case ::HIR::CoreType::I16:
                            case ::HIR::CoreType::I32:
                            case ::HIR::CoreType::I64:
                            case ::HIR::CoreType::I128:
                            case ::HIR::CoreType::Isize:
                                if( const auto* vp = nv.opt_Uint() )
                                {
                                    // If the destination is i128 and the value is >INT64_MAX, sign extension will break
                                    if( *te == ::HIR::CoreType::I128 )
                                    {
                                        if( vp->v > INT64_MAX ) {
                                            DEBUG(state << "Cast of large value to i128, sign extension would be incorrect");
                                            break;
                                        }
                                    }
                                    new_value = ::MIR::Constant::make_Int({
                                        H::truncate_s(*te, vp->v),
                                        *te
                                        });
                                }
                                else if( const auto* vp = nv.opt_Int() )
                                {
                                    new_value = ::MIR::Constant::make_Int({
                                        H::truncate_s(*te, vp->v),
                                        *te
                                        });
                                }
                                else if( const auto* vp = nv.opt_Bool() )
                                {
                                    new_value = ::MIR::Constant::make_Int({
                                        (vp->v ? 1 : 0),
                                        *te
                                        });
                                }
                                else
                                {
                                }
                                break;
                            case ::HIR::CoreType::F32:
                            case ::HIR::CoreType::F64:
                                // TODO: Cast to float
                                break;
                            case ::HIR::CoreType::Char:
                                // TODO: Only `u8` can be casted to char
                                break;
                            case ::HIR::CoreType::Bool:
                                break;
                            case ::HIR::CoreType::Str:
                                MIR_BUG(state, "Casting to str");
                            }
                        }
                    }
                    else if( known_values_var.count(se.val) )
                    {
                        auto variant_idx = known_values_var.at(se.val);
                        MIR_ASSERT(state, se.type.data().is_Primitive(), "Casting enum to non-primitive - " << se.type);

                        HIR::TypeRef    tmp;
                        const auto& src_ty = state.get_lvalue_type(tmp, se.val);
                        const HIR::Enum& enm = *src_ty.data().as_Path().binding.as_Enum();
                        MIR_ASSERT(state, enm.is_value(), "Casting non-value enum to value");
                        uint32_t v = enm.get_value(variant_idx);

                        auto ct = se.type.data().as_Primitive();
                        switch(ct)
                        {
                        case ::HIR::CoreType::U8:
                        case ::HIR::CoreType::U16:
                        case ::HIR::CoreType::U32:
                        case ::HIR::CoreType::U64:
                        case ::HIR::CoreType::U128:
                        case ::HIR::CoreType::Usize:
                            new_value = ::MIR::Constant::make_Uint({ v, ct });
                            break;
                        case ::HIR::CoreType::I8:
                        case ::HIR::CoreType::I16:
                        case ::HIR::CoreType::I32:
                        case ::HIR::CoreType::I64:
                        case ::HIR::CoreType::I128:
                        case ::HIR::CoreType::Isize:
                            new_value = ::MIR::Constant::make_Int({ static_cast<int32_t>(v), ct });
                            break;
                        case ::HIR::CoreType::F32:
                        case ::HIR::CoreType::F64:
                            // TODO: Cast to float (can variants be casted to float?)
                            break;
                        case ::HIR::CoreType::Char:
                            // TODO: Only `u8` can be casted to char (what about a u8 discriminator?)
                            break;
                        case ::HIR::CoreType::Bool:
                            break;
                        case ::HIR::CoreType::Str:
                            MIR_BUG(state, "Casting to str");
                        }
                    }
                    else
                    {
                    }

                    if( new_value != MIR::Constant() )
                    {
                        DEBUG(state << " " << e->src << " = " << new_value);
                        e->src = mv$(new_value);
                        changed = true;
                    }
                    }
                TU_ARMA(BinOp, se) {
                    check_param(se.val_l);
                    check_param(se.val_r);

                    if( se.val_l.is_Constant() && se.val_r.is_Constant() )
                    {
                        const auto& val_l = se.val_l.as_Constant();
                        const auto& val_r = se.val_r.as_Constant();

                        if( val_l.is_Const() || val_r.is_Const() )
                        {
                            // One of the arms is a named constant, can't check (they're not an actual value, just a
                            // reference to one)
                        }
                        else if( val_l.is_Generic() || val_r.is_Generic() )
                        {
                            // One of the arms is a generic, can't check either
                        }
                        else
                        {
                            ::MIR::Constant new_value;
                            switch(se.op)
                            {
                            case ::MIR::eBinOp::EQ:
                                new_value = ::MIR::Constant::make_Bool({val_l == val_r});
                                break;
                            case ::MIR::eBinOp::NE:
                                new_value = ::MIR::Constant::make_Bool({val_l != val_r});
                                break;
                            case ::MIR::eBinOp::LT:
                                new_value = ::MIR::Constant::make_Bool({val_l < val_r});
                                break;
                            case ::MIR::eBinOp::LE:
                                new_value = ::MIR::Constant::make_Bool({val_l <= val_r});
                                break;
                            case ::MIR::eBinOp::GT:
                                new_value = ::MIR::Constant::make_Bool({val_l > val_r});
                                break;
                            case ::MIR::eBinOp::GE:
                                new_value = ::MIR::Constant::make_Bool({val_l >= val_r});
                                break;

                            case ::MIR::eBinOp::ADD:
                                MIR_ASSERT(state, val_l.tag() == val_r.tag(), "Mismatched types for eBinOp::ADD - " << val_l << " + " << val_r);
                                //{TU_MATCH_HDRA( (val_l, val_r), {)
                                {TU_MATCH_HDRA( (val_l), {)
                                default:
                                    break;
                                TU_ARMA(Float, le) { const auto& re = val_r.as_Float();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::ADD - " << val_l << " / " << val_r);
                                    new_value = ::MIR::Constant::make_Float({ le.v + re.v, le.t });
                                    }
                                // TU_ARMav(Int, (le, re)) {
                                TU_ARMA(Int, le) { const auto& re = val_r.as_Int();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::ADD - " << val_l << " + " << val_r);
                                    new_value = ::MIR::Constant::make_Int({ H::truncate_s(le.t, le.v + re.v), le.t });
                                    }
                                TU_ARMA(Uint, le) { const auto& re = val_r.as_Uint();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::ADD - " << val_l << " + " << val_r);
                                    new_value = ::MIR::Constant::make_Uint({ H::truncate_u(le.t, le.v + re.v), le.t });
                                    }
                                }}
                                break;
                            case ::MIR::eBinOp::SUB:
                                MIR_ASSERT(state, val_l.tag() == val_r.tag(), "Mismatched types for eBinOp::SUB - " << val_l << " + " << val_r);
                                //{TU_MATCH_HDRA( (val_l, val_r), {)
                                {TU_MATCH_HDRA( (val_l), {)
                                default:
                                    break;
                                TU_ARMA(Float, le) { const auto& re = val_r.as_Float();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::SUB - " << val_l << " / " << val_r);
                                    new_value = ::MIR::Constant::make_Float({ le.v - re.v, le.t });
                                    }
                                // TU_ARMav(Int, (le, re)) {
                                TU_ARMA(Int, le) { const auto& re = val_r.as_Int();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::SUB - " << val_l << " - " << val_r);
                                    new_value = ::MIR::Constant::make_Int({ H::truncate_s(le.t, le.v - re.v), le.t });
                                    }
                                TU_ARMA(Uint, le) { const auto& re = val_r.as_Uint();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::SUB - " << val_l << " - " << val_r);
                                    new_value = ::MIR::Constant::make_Uint({ H::truncate_u(le.t, le.v - re.v), le.t });
                                    }
                                }}
                                break;
                            case ::MIR::eBinOp::MUL:
                                MIR_ASSERT(state, val_l.tag() == val_r.tag(), "Mismatched types for eBinOp::MUL - " << val_l << " * " << val_r);
                                //{TU_MATCH_HDRA( (val_l, val_r), {)
                                {TU_MATCH_HDRA( (val_l), {)
                                default:
                                    break;
                                TU_ARMA(Float, le) { const auto& re = val_r.as_Float();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::MUL - " << val_l << " / " << val_r);
                                    new_value = ::MIR::Constant::make_Float({ le.v * re.v, le.t });
                                    }
                                // TU_ARMav(Int, (le, re)) {
                                TU_ARMA(Int, le) { const auto& re = val_r.as_Int();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::MUL - " << val_l << " * " << val_r);
                                    new_value = ::MIR::Constant::make_Int({ H::truncate_s(le.t, le.v * re.v), le.t });
                                    }
                                TU_ARMA(Uint, le) { const auto& re = val_r.as_Uint();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::MUL - " << val_l << " * " << val_r);
                                    new_value = ::MIR::Constant::make_Uint({ H::truncate_u(le.t, le.v * re.v), le.t });
                                    }
                                }}
                                break;
                            case ::MIR::eBinOp::DIV:
                                MIR_ASSERT(state, val_l.tag() == val_r.tag(), "Mismatched types for eBinOp::DIV - " << val_l << " / " << val_r);
                                //{TU_MATCH_HDRA( (val_l, val_r), {)
                                {TU_MATCH_HDRA( (val_l), {)
                                default:
                                    break;
                                TU_ARMA(Float, le) { const auto& re = val_r.as_Float();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::DIV - " << val_l << " / " << val_r);
                                    new_value = ::MIR::Constant::make_Float({ le.v / re.v, le.t });
                                    }
                                // TU_ARMav(Int, (le, re)) {
                                TU_ARMA(Int, le) { const auto& re = val_r.as_Int();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::DIV - " << val_l << " / " << val_r);
                                    if( re.v == 0 ) {
                                        DEBUG(state << "Const eval error: Constant division by zero");
                                    }
                                    else {
                                        new_value = ::MIR::Constant::make_Int({ H::truncate_s(le.t, le.v / re.v), le.t });
                                    }
                                    }
                                TU_ARMA(Uint, le) { const auto& re = val_r.as_Uint();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::DIV - " << val_l << " / " << val_r);
                                    if( re.v == 0 ) {
                                        DEBUG(state << "Const eval error: Constant division by zero");
                                    }
                                    else {
                                        new_value = ::MIR::Constant::make_Uint({ H::truncate_u(le.t, le.v / re.v), le.t });
                                    }
                                    }
                                }}
                                break;
                            case ::MIR::eBinOp::MOD:
                                MIR_ASSERT(state, val_l.tag() == val_r.tag(), "Mismatched types for eBinOp::MOD - " << val_l << " % " << val_r);
                                //{TU_MATCH_HDRA( (val_l, val_r), {)
                                {TU_MATCH_HDRA( (val_l), {)
                                default:
                                    break;
                                // TU_ARMav(Int, (le, re)) {
                                TU_ARMA(Int, le) { const auto& re = val_r.as_Int();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::MOD - " << val_l << " % " << val_r);
                                    MIR_ASSERT(state, re.v != 0, "Const eval error: Constant division by zero");
                                    new_value = ::MIR::Constant::make_Int({ H::truncate_s(le.t, le.v % re.v), le.t });
                                    }
                                TU_ARMA(Uint, le) { const auto& re = val_r.as_Uint();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::MOD - " << val_l << " % " << val_r);
                                    MIR_ASSERT(state, re.v != 0, "Const eval error: Constant division by zero");
                                    new_value = ::MIR::Constant::make_Uint({ H::truncate_u(le.t, le.v % re.v), le.t });
                                    }
                                }}
                                break;

                            case ::MIR::eBinOp::BIT_AND:
                                MIR_ASSERT(state, val_l.tag() == val_r.tag(), "Mismatched types for eBinOp::BIT_AND - " << val_l << " & " << val_r);
                                //TU_MATCH_HDRA( (val_l, val_r), {)
                                TU_MATCH_HDRA( (val_l), {)
                                default:
                                    break;
                                TU_ARMA(Bool, le) { const auto& re = val_r.as_Bool();
                                    new_value = ::MIR::Constant::make_Bool({ le.v && re.v });
                                    }
                                // TU_ARMav(Int, (le, re)) {
                                TU_ARMA(Int, le) { const auto& re = val_r.as_Int();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::BIT_AND - " << val_l << " ^ " << val_r);
                                    new_value = ::MIR::Constant::make_Int({ H::truncate_s(le.t, le.v & re.v), le.t });
                                    }
                                TU_ARMA(Uint, le) { const auto& re = val_r.as_Uint();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::BIT_AND - " << val_l << " ^ " << val_r);
                                    new_value = ::MIR::Constant::make_Uint({ H::truncate_u(le.t, le.v & re.v), le.t });
                                    }
                                }
                                break;
                            case ::MIR::eBinOp::BIT_OR:
                                MIR_ASSERT(state, val_l.tag() == val_r.tag(), "Mismatched types for eBinOp::BIT_OR - " << val_l << " | " << val_r);
                                //TU_MATCH_HDRA( (val_l, val_r), {)
                                TU_MATCH_HDRA( (val_l), {)
                                default:
                                    break;
                                TU_ARMA(Bool, le) { const auto& re = val_r.as_Bool();
                                    new_value = ::MIR::Constant::make_Bool({ le.v || re.v });
                                    }
                                TU_ARMA(Int, le) { const auto& re = val_r.as_Int();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::BIT_OR - " << val_l << " | " << val_r);
                                    new_value = ::MIR::Constant::make_Int({ H::truncate_s(le.t, le.v | re.v), le.t });
                                    }
                                TU_ARMA(Uint, le) { const auto& re = val_r.as_Uint();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::BIT_OR - " << val_l << " | " << val_r);
                                    new_value = ::MIR::Constant::make_Uint({ H::truncate_u(le.t, le.v | re.v), le.t });
                                    }
                                }
                                break;
                            case ::MIR::eBinOp::BIT_XOR:
                                MIR_ASSERT(state, val_l.tag() == val_r.tag(), "Mismatched types for eBinOp::BIT_XOR - " << val_l << " ^ " << val_r);
                                //TU_MATCH_HDRA( (val_l, val_r), {)
                                TU_MATCH_HDRA( (val_l), {)
                                default:
                                    break;
                                TU_ARMA(Bool, le) { const auto& re = val_r.as_Bool();
                                    new_value = ::MIR::Constant::make_Bool({ le.v != re.v });
                                    }
                                // TU_ARMav(Int, (le, re)) {
                                TU_ARMA(Int, le) { const auto& re = val_r.as_Int();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::BIT_XOR - " << val_l << " ^ " << val_r);
                                    new_value = ::MIR::Constant::make_Int({ H::truncate_s(le.t, le.v ^ re.v), le.t });
                                    }
                                TU_ARMA(Uint, le) { const auto& re = val_r.as_Uint();
                                    MIR_ASSERT(state, le.t == re.t, "Mismatched types for eBinOp::BIT_XOR - " << val_l << " ^ " << val_r);
                                    new_value = ::MIR::Constant::make_Uint({ H::truncate_u(le.t, le.v ^ re.v), le.t });
                                    }
                                }
                                break;

                            case ::MIR::eBinOp::BIT_SHL: {
                                uint64_t shift_len = 0;
                                TU_MATCH_HDRA( (val_r), {)
                                default:
                                    MIR_BUG(state, "Mismatched types for eBinOp::BIT_SHL - " << val_l << " << " << val_r << " - " << e->src);
                                    break;
                                TU_ARMA(Int, re) {
                                    shift_len = re.v;
                                    }
                                TU_ARMA(Uint, re) {
                                    shift_len = re.v;
                                    }
                                }
                                TU_MATCH_HDRA( (val_l), {)
                                default:
                                    break;
                                TU_ARMA(Int, le) {
                                    if( le.t != HIR::CoreType::I128 )
                                    {
                                        MIR_ASSERT(state, shift_len < 64, "Const eval error: Over-sized eBinOp::BIT_SHL - " << val_l << " << " << val_r);
                                        new_value = ::MIR::Constant::make_Int({ H::truncate_s(le.t, le.v << shift_len), le.t });
                                    }
                                    }
                                TU_ARMA(Uint, le) {
                                    if( le.t != HIR::CoreType::U128 )
                                    {
                                        MIR_ASSERT(state, shift_len < 64, "Const eval error: Over-sized eBinOp::BIT_SHL - " << val_l << " << " << val_r);
                                        new_value = ::MIR::Constant::make_Uint({ H::truncate_u(le.t, le.v << shift_len), le.t });
                                    }
                                    }
                                }
                                } break;
                            case ::MIR::eBinOp::BIT_SHR:{
                                uint64_t shift_len = 0;
                                TU_MATCH_HDRA( (val_r), {)
                                default:
                                    MIR_BUG(state, "Mismatched types for eBinOp::BIT_SHR - " << val_l << " >> " << val_r);
                                    break;
                                TU_ARMA(Int, re) {
                                    shift_len = re.v;
                                    }
                                TU_ARMA(Uint, re) {
                                    shift_len = re.v;
                                    }
                                }
                                TU_MATCH_HDRA( (val_l), {)
                                default:
                                    break;
                                TU_ARMA(Int, le) {
                                    if( le.t != HIR::CoreType::I128 )
                                    {
                                        MIR_ASSERT(state, shift_len < 64, "Const eval error: Over-sized eBinOp::BIT_SHR - " << val_l << " >> " << val_r);
                                        new_value = ::MIR::Constant::make_Int({ H::truncate_s(le.t, le.v >> shift_len), le.t });
                                    }
                                    }
                                TU_ARMA(Uint, le) {
                                    if( le.t != HIR::CoreType::U128 )
                                    {
                                        MIR_ASSERT(state, shift_len < 64, "Const eval error: Over-sized eBinOp::BIT_SHR - " << val_l << " >> " << val_r);
                                        new_value = ::MIR::Constant::make_Uint({ H::truncate_u(le.t, le.v >> shift_len), le.t });
                                    }
                                    }
                                }
                                } break;
                            // TODO: Other binary operations
                            // Could emit a TODO?
                            default:
                                break;
                            }

                            if( new_value != ::MIR::Constant() )
                            {
                                DEBUG(state << " " << e->src << " = " << new_value);
                                e->src = mv$(new_value);
                                changed = true;
                            }
                        }
                    }
                    }
                TU_ARMA(UniOp, se) {
                    auto it = known_values.find(se.val);
                    if( it != known_values.end() )
                    {
                        const auto& val = it->second;
                        ::MIR::Constant new_value;
                        bool replace = false;
                        switch( se.op )
                        {
                        case ::MIR::eUniOp::INV:
                            TU_MATCH_HDRA( (val), {)
                            TU_ARMA(Uint, ve) {
                                auto val = ve.v;
                                replace = true;
                                switch(ve.t)
                                {
                                case ::HIR::CoreType::U8:
                                case ::HIR::CoreType::U16:
                                case ::HIR::CoreType::U32:
                                case ::HIR::CoreType::Usize:
                                case ::HIR::CoreType::U64:
                                    val = H::truncate_u(ve.t, ~val);
                                    break;
                                case ::HIR::CoreType::U128:
                                    replace = false;
                                    break;
                                case ::HIR::CoreType::Char:
                                    MIR_BUG(state, "Invalid use of ! on char");
                                    break;
                                default:
                                    // Invalid type for Uint literal
                                    replace = false;
                                    break;
                                }
                                new_value = ::MIR::Constant::make_Uint({ val, ve.t });
                                }
                            TU_ARMA(Int, ve) {
                                // ! is valid on Int, it inverts bits the same way as an uint
                                auto val = ve.v;
                                switch(ve.t)
                                {
                                case ::HIR::CoreType::I8:
                                case ::HIR::CoreType::I16:
                                case ::HIR::CoreType::I32:
                                case ::HIR::CoreType::Isize:
                                case ::HIR::CoreType::I64:
                                    val = H::truncate_s(ve.t, ~val);
                                    replace = true;
                                    break;
                                case ::HIR::CoreType::I128:
                                    // TODO: Are there any cases where sign extension stops being correct here?
                                    val = H::truncate_s(ve.t, ~val);
                                    replace = true;
                                    break;
                                case ::HIR::CoreType::Char:
                                    MIR_BUG(state, "Invalid use of ! on char");
                                    break;
                                default:
                                    // Invalid type for Uint literal
                                    replace = false;
                                    break;
                                }
                                new_value = ::MIR::Constant::make_Int({ val, ve.t });
                                }
                            TU_ARMA(Float, ve) {
                                // Not valid?
                                }
                            TU_ARMA(Bool, ve) {
                                new_value = ::MIR::Constant::make_Bool({ !ve.v });
                                replace = true;
                                }
                            TU_ARMA(Bytes, ve) {}
                            TU_ARMA(StaticString, ve) {}
                            TU_ARMA(Const, ve) {
                                // TODO:
                                }
                            TU_ARMA(Generic, ve) {
                                }
                            TU_ARMA(ItemAddr, ve) {
                                }
                            }
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
                                if( !::std::isnan(ve.v) ) {
                                    new_value = ::MIR::Constant::make_Float({ -ve.v, ve.t });
                                    replace = true;
                                }
                                ),
                            (Bool,
                                // Not valid?
                                ),
                            (Bytes, ),
                            (StaticString, ),
                            (Const,
                                // TODO:
                                ),
                            (Generic,
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
                    }
                TU_ARMA(DstMeta, se) {
                    }
                TU_ARMA(DstPtr, se) {
                    }
                TU_ARMA(MakeDst, se) {
                    check_param(se.ptr_val);
                    check_param(se.meta_val);
                    }
                TU_ARMA(Tuple, se) {
                    for(auto& p : se.vals)
                        check_param(p);
                    }
                TU_ARMA(Array, se) {
                    for(auto& p : se.vals)
                        check_param(p);
                    }
                TU_ARMA(UnionVariant, se) {
                    check_param(se.val);
                    }
                TU_ARMA(EnumVariant, se) {
                    for(auto& p : se.vals)
                        check_param(p);
                    }
                TU_ARMA(Struct, se) {
                    for(auto& p : se.vals)
                        check_param(p);
                    }
                }
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
                    else if( const auto* ce = e->src.opt_EnumVariant() )
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
                            else if( it2 != known_values_var.end() ) {
                                known_values_var.insert(::std::make_pair( e->dst.clone(), it2->second ));
                                DEBUG(state << stmt);
                            }
                            else {
                                // Neither known, don't propagate
                            }
                        }
                    }
                    else
                    {
                        // No need to clear, the visit above this if block handles it.
                    }
                }
            }
        }

        state.set_cur_stmt_term(bbidx);
        visit_mir_lvalues_mut(bb.terminator, edit_lval);
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
                if( it->second.is_Const() || it->second.is_Generic() ) {
                }
                else {
                    MIR_ASSERT(state, it->second.is_Bool(), "Terminator::If with known value not Bool - " << it->second);
                    auto new_bb = (it->second.as_Bool().v ? te.bb0 : te.bb1);
                    DEBUG(state << "Convert " << bb.terminator << " into Goto(" << new_bb << ") because condition known to be " << it->second);
                    bb.terminator = ::MIR::Terminator::make_Goto(new_bb);

                    changed = true;
                }
            }
            } break;
        TU_ARM(bb.terminator, Call, te) {
            for(auto& a : te.args)
            {
                check_param(a);
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
// Split aggregated values that are never used by outer value into inner values
// --------------------------------------------------------------------
// NOTE: This is a generalised version of the old de-tuple pass (and fills part of MIR_Optimise_PropagateKnownValues)
//
// NOTE: This has a special case rule that disallowes borrows of the first field: Sometimes a borrow of the first
//       field is used as a proxy for the entire struct.
bool MIR_Optimise_SplitAggregates(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);
    // Find locals that are:
    // - Assigned once
    // - From a constructor
    // - And only ever used via a field access
    // Replace the construction with assignments of `n` locals instead (which can be optimised by further passes)

    struct Potential {
        size_t  src_bb_idx;
        size_t  src_stmt_idx;
        unsigned    variant_idx;

        bool    is_direct_used;
        unsigned    n_write;
        std::vector<unsigned>   replacements;

        Potential(size_t src_bb_idx, size_t src_stmt_idx, unsigned variant_idx = ~0u)
            :src_bb_idx(src_bb_idx)
            ,src_stmt_idx(src_stmt_idx)
            ,variant_idx(variant_idx)
            ,is_direct_used(false)
            ,n_write(0)
        {
        }
    };
    std::map<unsigned, Potential>   potentials;

    // 1. Find locals created from constructors (struct/tuple)
    for(const auto& block : fcn.blocks)
    {
        size_t bb_idx = &block - &fcn.blocks.front();
        for(size_t i = 0; i < block.statements.size(); i++)
        {
            const auto& stmt = block.statements[i];
            if( const auto* se = stmt.opt_Assign() )
            {
                if( !se->dst.is_Local() )
                    continue ;

                if( auto* sse = se->src.opt_Struct() ) {
                    if( sse->vals.size() == 0 )
                        continue ;
                }
                else if( auto* sse = se->src.opt_Tuple() ) {
                    if( sse->vals.size() == 0 )
                        continue ;
                }
                // NOTE: Arrays are eligable (as long as they're only accessed using field operator
                else if( auto* sse = se->src.opt_Array() ) {
                    if( sse->vals.size() == 0 )
                        continue ;
                }
                // Variants are allowed, they store the variant index for later checking
                else if( auto* sse = se->src.opt_EnumVariant() ) {
                    if( sse->vals.size() == 0 )
                        continue ;
                    DEBUG("> BB" << bb_idx << "/" << i << ": POSSIBLE " << stmt);
                    potentials.insert( std::make_pair(se->dst.as_Local(), Potential(bb_idx, i, sse->index)) );
                    continue ;
                }
                // NOTE: Union variants need special handling in the replacement
                else {
                    continue ;
                }

                // Found a potential.
                DEBUG("> BB" << bb_idx << "/" << i << ": POSSIBLE " << stmt);
                potentials.insert( std::make_pair(se->dst.as_Local(), Potential(bb_idx, i)) );
            }
        }
    }
    // - Nothing to do? return early
    if( potentials.empty() )
        return false;

    // 2. Check how the variables are used (allow one write, and no other direct usage)
    // - Removes any potentials that are invalidated.
    visit_mir_lvalues(state, fcn, [&](const MIR::LValue& lv, ValUsage vu)->bool {
        if( lv.m_root.is_Local() )
        {
            // Is this one of the potentials?
            auto it = potentials.find(lv.m_root.as_Local());
            if( it != potentials.end() )
            {
                if( lv.m_wrappers.empty() )
                {
                    // NOTE: A single write is allowed (the assignment)
                    // - Any other would be a re-assignent or a drop
                    if( vu == ValUsage::Write )
                    {
                        it->second.n_write += 1;
                    }
                    else
                    {
                        // Direct usage!
                        it->second.is_direct_used = true;
                    }
                }
                else if( lv.m_wrappers.front().is_Field() )
                {
                    // Field acess: allowed UNLESS it's a borrow of the first field
                    // TODO: Find out what code makes the assumption that `&foo.0` is a good stand-in for `&foo`
                    if( lv.m_wrappers.front().as_Field() == 0 && vu == ValUsage::Borrow )
                    {
                        it->second.is_direct_used = true;
                    }
                }
                else if( lv.m_wrappers.front().is_Downcast() )
                {
                    // Downcast to a variant other than the variant it was constructed as, don't do anything.
                    // - For enums, this is an error (but here we don't know for sure). For unions it's valid behaviour
                    if( lv.m_wrappers.front().as_Downcast() != it->second.variant_idx )
                    {
                        it->second.is_direct_used = true;
                    }
                }
                else
                {
                    // Index and deref are disallowed
                    it->second.is_direct_used = true;
                }

                // If invalidated, delete.
                if( it->second.is_direct_used || it->second.n_write > 1 )
                {
                    const auto& stmt = fcn.blocks[it->second.src_bb_idx].statements[it->second.src_stmt_idx];
                    DEBUG(state << ": REMOVE BB" << it->second.src_bb_idx << "/" << it->second.src_stmt_idx << " " << stmt << " from " << lv /*<< " vu=" << vu*/);
                    potentials.erase(it);
                }
            }
        }
        return true;
        });
    // - All potentials removed? Return early
    if( potentials.empty() )
        return false;

    // 3. Explode sources into locals
    // NOTE: This needs to handle movement of indexes
    for(auto& p : potentials)
    {
        auto bb_idx = p.second.src_bb_idx;
        auto stmt_idx = p.second.src_stmt_idx;
        state.set_cur_stmt(bb_idx, stmt_idx);
        auto& block = fcn.blocks[bb_idx];

        DEBUG("- BB" << bb_idx << "/" << stmt_idx << ": " << block.statements[stmt_idx]);
        // Extract the list of values from the existing statement
        std::vector<MIR::Param> vals;
        {
            auto& src = block.statements[stmt_idx].as_Assign().src;
            if( auto* se = src.opt_Struct() ) {
                vals = std::move(se->vals);
            }
            else if( auto* se = src.opt_Tuple() ) {
                vals = std::move(se->vals);
            }
            else if( auto* se = src.opt_Array() ) {
                vals = std::move(se->vals);
            }
            else if( auto* se = src.opt_EnumVariant() ) {
                vals = std::move(se->vals);
            }
            else if( auto* se = src.opt_UnionVariant() ) {
                vals.push_back( mv$(se->val) );
            }
            else {
                MIR_BUG(state, "Unexpected rvalue type in SplitAggregates - " << src);
            }
        }
        MIR_ASSERT(state, vals.size() > 0, "Optimisation can't apply to empty lists");
        auto offset = vals.size() - 1;

        //for(size_t i = 0; i < block.statements.size(); i ++)
        //    DEBUG("> BB" << bb_idx << "/" << i << ": " << block.statements[i]);

        // Insert new statements as required
        if( offset > 0 )
        {
            block.statements.resize( block.statements.size() + offset );
            // Move all elements [stmt_idx+1 .. ] up by `offset`
            // NOTE: move_backward's third argument is 'past-the-end'
            std::move_backward(block.statements.begin() + stmt_idx + 1, block.statements.end() - offset, block.statements.end());
        }

        // Create new statements (allocating new locals)
        auto new_local_base = fcn.locals.size();
        fcn.locals.resize( fcn.locals.size() + vals.size() );
        p.second.replacements.resize(vals.size());
        for(size_t i = 0; i < vals.size(); i ++)
        {
            // Allocate a new local
            auto new_local = static_cast<unsigned>(new_local_base + i);
            ::HIR::TypeRef  tmp;
            fcn.locals[new_local] = state.get_param_type(tmp, vals[i]).clone();
            p.second.replacements[i] = new_local;
            // Set the relevant statement to be an assignment to that new local
            block.statements[stmt_idx + i] = MIR::Statement::make_Assign({ MIR::LValue::new_Local(new_local), param_to_rvalue(mv$(vals[i])) });
            DEBUG("+ BB" << bb_idx << "/" << (stmt_idx + i) << ": " << block.statements[stmt_idx + i]);
        }

        //for(size_t i = 0; i < block.statements.size(); i ++)
        //    DEBUG("> BB" << bb_idx << "/" << i << ": " << block.statements[i]);

        // If this replacement changed the number of statements in this block, update all existing references.
        if( offset > 0 )
        {
            for(auto& other_p : potentials)
            {
                if(other_p.second.src_bb_idx == bb_idx && other_p.second.src_stmt_idx > stmt_idx )
                {
                    other_p.second.src_stmt_idx += offset;
                }
            }
        }
    }

    // 4. Replace all usages
    visit_mir_lvalues_mut(state, fcn, [&](MIR::LValue& lv, ValUsage vu)->bool {
        if( lv.m_root.is_Local() )
        {
            // Is this one of the potentials?
            auto it = potentials.find(lv.m_root.as_Local());
            if( it != potentials.end() )
            {
                size_t ndel;
                size_t field_idx;
                if( it->second.variant_idx == ~0u )
                {
                    field_idx = lv.m_wrappers.front().as_Field();
                    ndel = 1;
                }
                else
                {
                    MIR_ASSERT(state, lv.m_wrappers[0].is_Downcast(), lv);
                    MIR_ASSERT(state, lv.m_wrappers[1].is_Field(), lv);
                    field_idx = lv.m_wrappers[1].as_Field();
                    ndel = 2;
                }
                auto new_wrappers = std::vector<MIR::LValue::Wrapper>(lv.m_wrappers.begin() + ndel, lv.m_wrappers.end());
                auto new_root = MIR::LValue::Storage::new_Local(it->second.replacements.at(field_idx));
                auto new_lv = MIR::LValue(mv$(new_root), mv$(new_wrappers));
                DEBUG(state << " " << lv << " -> " << new_lv);
                lv = mv$(new_lv);
            }
        }
        return true;
        });

    // If we reach this point, a replacement was done.
    changed = true;
    return true;
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
            for(const auto& w : lv.m_wrappers)
            {
                if( w.is_Index() ){
                    //local_uses[w.as_Index()].read += 1;
                    local_uses[w.as_Index()].borrow += 1;
                }
            }
            if( lv.m_root.is_Local() )
            {
                auto& vu = local_uses[lv.m_root.as_Local()];
                switch(ut)
                {
                case ValUsage::Move:
                case ValUsage::Read:    vu.read += 1;   break;
                case ValUsage::Write:   vu.write += 1;  break;
                case ValUsage::Borrow:  vu.borrow += 1; break;
                }
            }
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
        //::std::map< ::MIR::LValue::CRef, ::MIR::RValue>    replacements;
        ::std::vector< ::std::pair<::MIR::LValue, ::MIR::RValue> >  replacements;
        auto replacements_find = [&replacements](const ::MIR::LValue::CRef& lv) {
            return ::std::find_if(replacements.begin(), replacements.end(), [&](const auto& e) { return lv == e.first; });
            };
        for(const auto& block : fcn.blocks)
        {
            if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;

            for(unsigned int stmt_idx = 0; stmt_idx < block.statements.size(); stmt_idx ++)
            {
                state.set_cur_stmt(&block - &fcn.blocks.front(), stmt_idx);
                const auto& stmt = block.statements[stmt_idx];
                DEBUG(state << stmt);
                // > Assignment
                if( ! stmt.is_Assign() )
                    continue ;
                const auto& e = stmt.as_Assign();
                // > Of a temporary from with a RValue::Use
                if( e.dst.is_Local() )
                {
                    const auto& vu = val_uses.local_uses[e.dst.as_Local()];
                    DEBUG(" - VU " << e.dst << " R:" << vu.read << " W:" << vu.write << " B:" << vu.borrow);
                    // TODO: Allow write many?
                    // > Where the variable is written once and read once
                    if( !( vu.read == 1 && vu.write == 1 && vu.borrow == 0 ) )
                        continue ;
                }
                else
                {
                    continue ;
                }
                if( e.src.is_Use() )
                {
                    // Keep the complexity down
                    const auto* srcp = &e.src.as_Use();
                    if( ::std::any_of(srcp->m_wrappers.begin(), srcp->m_wrappers.end(), [](auto& w) { return !w.is_Field(); }) )
                        continue ;
                    if( !srcp->m_root.is_Local() )
                        continue ;

                    if( replacements_find(*srcp) != replacements.end() )
                    {
                        DEBUG("> Can't replace, source has pending replacement");
                        continue;
                    }
                }
                else
                {
                    continue ;
                }
                bool src_is_lvalue = e.src.is_Use();
                DEBUG("- Locate usage");

                auto is_lvalue_usage = [&](const auto& lv, auto ){
                    return lv.m_root == e.dst.m_root;
                    //return lv == e.dst;
                    };

                // Eligable for replacement
                // Find where this value is used
                // - Stop on a conditional block terminator
                // - Stop if any value mentioned in the source is mutated/invalidated
                bool stop = false;
                bool found = false;
                for(unsigned int si2 = stmt_idx+1; si2 < block.statements.size(); si2 ++)
                {
                    state.set_cur_stmt(&block - &fcn.blocks.front(), si2);
                    const auto& stmt2 = block.statements[si2];
                    DEBUG(state << "[find usage] " << stmt2);

                    // Check for invalidation (done first, to avoid cases where the source is moved into a struct)
                    if( check_invalidates_lvalue(stmt2, e.src.as_Use()) ) {
                        stop = true;
                        break;
                    }

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
                }
                if( !stop )
                {
                    state.set_cur_stmt_term(&block - &fcn.blocks.front());
                    DEBUG(state << "[find usage] " << block.terminator);
                    if( src_is_lvalue )
                    {
                        visit_mir_lvalues(block.terminator, [&](const auto& lv, auto vu) {
                            found |= is_lvalue_usage(lv, vu);
                            return found;
                            });
                    }
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
                        stop = true;
                        ),
                    (Switch,
                        stop = true;
                        ),
                    (SwitchValue,
                        stop = true;
                        ),
                    (Call,
                        stop = true;
                        )
                    )
                }
                // Schedule a replacement in a future pass
                if( found )
                {
                    DEBUG("> Schedule replace " << e.dst << " with " << e.src.as_Use());
                    replacements.push_back( ::std::make_pair(e.dst.clone(), e.src.clone()) );
                }
                else
                {
                    DEBUG("- Single-write/read " << e.dst << " not replaced - couldn't find usage");
                }
            }   // for(stmt : block.statements)
        }

        DEBUG("replacements = " << replacements);

        // Apply replacements within replacements
        for(;;)
        {
            unsigned int inner_replaced_count = 0;
            for(auto& r : replacements)
            {
                visit_mir_lvalues_mut(r.second, [&](::MIR::LValue& lv, auto vu) {
                    if( vu == ValUsage::Read || vu == ValUsage::Move )
                    {
                        visit_mir_lvalue_mut(lv, vu, [&](::MIR::LValue::MRef& lvr, auto vu) {
                            auto it = replacements_find(lvr);
                            if( it != replacements.end() && it->second.is_Use() )
                            {
                                lvr.replace( it->second.as_Use().clone() );
                                inner_replaced_count ++;
                            }
                            return false;
                            });
                    }
                    return false;
                    });
            }
            if( inner_replaced_count == 0 )
                break;
        }
        DEBUG("replacements = " << replacements);

        // Apply replacements
        unsigned int replaced = 0;
        while( replaced < replacements.size() )
        {
            auto old_replaced = replaced;
            auto cb = [&](::MIR::LValue& lv, auto vu){
                return visit_mir_lvalue_mut(lv, vu, [&](::MIR::LValue::MRef& lv, auto vu) {
                    if( vu == ValUsage::Read || vu == ValUsage::Move )
                    {
                        auto it = replacements_find(lv);
                        if( it != replacements.end() )
                        {
                            MIR_ASSERT(state, it->second.tag() != ::MIR::RValue::TAGDEAD, "Replacement of  " << lv << " fired twice");
                            MIR_ASSERT(state, it->second.is_Use(), "Replacing a lvalue with a rvalue - " << lv << " with " << it->second);
                            auto rval = ::std::move(it->second);
                            DEBUG("> Do replace " << lv << " => " << rval);
                            lv.replace( ::std::move(rval.as_Use()) );
                            replaced += 1;
                        }
                    }
                    return false;
                    });
                };
            for(unsigned int block_idx = 0; block_idx < fcn.blocks.size(); block_idx ++)
            {
                auto& block = fcn.blocks[block_idx];
                if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                    continue ;
                for(auto& stmt : block.statements)
                {
                    state.set_cur_stmt(block_idx, (&stmt - &block.statements.front()));
                    DEBUG(state << stmt);
#if 0
                    if( stmt.is_Assign() && stmt.as_Assign().src.is_Use() )
                    {
                        auto& e = stmt.as_Assign();
                        auto it = replacements_find(e.src.as_Use());
                        if( it != replacements.end() )
                        {
                            MIR_ASSERT(state, it->second.tag() != ::MIR::RValue::TAGDEAD, "Replacement of  " << it->first << " fired twice");
                            e.src = mv$(it->second);
                            replaced += 1;
                        }
                    }
                    else
#endif
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
                auto it2 = replacements.end();
                if( it->is_Assign() && (it2 = replacements_find(it->as_Assign().dst)) != replacements.end() ) {
                    DEBUG(state << "Delete " << *it);
                    it = block.statements.erase(it);
                }
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
                if( to_replace_lval.is_Local() ){
                    const auto& vu = val_uses.local_uses[to_replace_lval.as_Local()];
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
                    if( new_dst_lval.is_Local() ) {
                        const auto& vu = val_uses.local_uses[new_dst_lval.as_Local()];
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
                            // Don't care about indexing?
                            return lv.m_root == new_dst_lval.m_root;
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
                    auto lvalue_impacts_dst = [&](const ::MIR::LValue& lv)->bool {
                        // Returns true if the two lvalues share a common root
                        // TODO: Could restrict based on the presence of deref/field accesses?
                        // If `lv` is a local AND matches the index in `new_dst`, check for indexing
                        if( lv.is_Local() )
                        {
                            for(const auto& w : new_dst->m_wrappers)
                            {
                                if( w.is_Index() && w.as_Index() == lv.as_Local() )
                                {
                                    return true;
                                }
                            }
                        }
                        return lv.m_root == new_dst->m_root;
                        };
                    for(auto it = blk2.statements.begin(); it != blk2.statements.end(); ++ it)
                    {
                        state.set_cur_stmt(&blk2 - &fcn.blocks.front(), it - blk2.statements.begin());
                        const auto& stmt = *it;
                        if( stmt.is_Assign() && stmt.as_Assign().src.is_Use() && stmt.as_Assign().src.as_Use() == e.ret_val )
                        {
                            DEBUG(state << "- Replace function return " << e.ret_val << " with " << *new_dst);
                            e.ret_val = new_dst->clone();
                            // TODO: Invalidate the entry, instead of deleting?
                            it = blk2.statements.erase(it);
                            replacement_happend = true;
                            break;
                        }
                        if( visit_mir_lvalues(stmt, [&](const MIR::LValue& lv, ValUsage vu){ return lv == *new_dst || (vu == ValUsage::Write && lvalue_impacts_dst(lv)); }) )
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
                    if( se->dst.is_Local() )
                    {
                        const auto& vu = val_uses.local_uses[se->dst.as_Local()];
                        if( vu.write == 1 && vu.read == 0 && vu.borrow == 0 ) {
                            DEBUG(state << se->dst << " only written, removing write");
                            it = block.statements.erase(it)-1;
                        }
                    }
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
    ::std::vector<bool> used_drop_flags( fcn.drop_flags.size() );
    {
        ::std::vector<bool> read_drop_flags( fcn.drop_flags.size() );
        visit_blocks(state, fcn, [&read_drop_flags,&used_drop_flags](auto , const ::MIR::BasicBlock& block) {
                for(const auto& stmt : block.statements)
                {
                    if( const auto* e = stmt.opt_SetDropFlag() )
                    {
                        if(e->other != ~0u) {
                            read_drop_flags[e->other] = true;
                            used_drop_flags[e->other] = true;
                        }
                        used_drop_flags[e->idx] = true;
                    }
                    else if( const auto* e = stmt.opt_Drop() )
                    {
                        if(e->flag_idx != ~0u) {
                            read_drop_flags[e->flag_idx] = true;
                            used_drop_flags[e->flag_idx] = true;
                        }
                    }
                }
                });
        DEBUG("Un-read drop flags:" << FMT_CB(ss,
            for(size_t i = 0; i < read_drop_flags.size(); i ++)
                if( ! read_drop_flags[i] && used_drop_flags[i] )
                    ss << " " << i;
            ));
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
    }

    // Find any drop flags that are never assigned with a value other than their default, then remove those dead assignments.
    {
        ::std::vector<bool> edited_drop_flags( fcn.drop_flags.size() );
        visit_blocks(state, fcn, [&edited_drop_flags,&fcn](auto , const ::MIR::BasicBlock& block) {
                for(const auto& stmt : block.statements)
                {
                    if( const auto* e = stmt.opt_SetDropFlag() )
                    {
                        if(e->other != ~0u) {
                            // If the drop flag is set based on another, assume it's changed
                            edited_drop_flags[e->idx] = true;
                        }
                        else if( e->new_val != fcn.drop_flags[e->idx] ) {
                            // If the new value is not the default, it's changed
                            edited_drop_flags[e->idx] = true;
                        }
                        else {
                            // Set to the default, doesn't change the 'edited' state
                        }
                    }
                }
                });
        DEBUG("Un-edited drop flags:" << FMT_CB(ss,
            for(size_t i = 0; i < edited_drop_flags.size(); i ++)
                if( ! edited_drop_flags[i] && used_drop_flags[i] )
                    ss << " " << i;
            ));
        visit_blocks_mut(state, fcn, [&edited_drop_flags,&removed_statement,&fcn](auto _id, auto& block) {
                for(auto it = block.statements.begin(); it != block.statements.end(); )
                {
                    // If this is a SetDropFlag and the target flag isn't edited, remove
                    if(const auto* e = it->opt_SetDropFlag())
                    {
                        if( ! edited_drop_flags[e->idx] ) {
                            assert( e->new_val == fcn.drop_flags[e->idx] );
                            removed_statement = true;
                            it = block.statements.erase(it);
                        }
                        else {
                            ++ it;
                        }
                    }
                    else {
                        ++ it;
                    }
                }
                });
    }

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
            if( lv.m_root.is_Local() ) {
                read_locals[lv.m_root.as_Local()] = true;
            }
            for(const auto& w : lv.m_wrappers)
                if(w.is_Index())
                    read_locals[w.as_Index()] = true;
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

    HIR::TypeRef    tmp_ty;
    // Remove useless operations
    for(auto& bb : fcn.blocks)
    {
        // Multi-statement no-ops (round-trip casts, reboorrow+cast)
        for(auto it = bb.statements.begin(); it != bb.statements.end(); ++it)
        {
            state.set_cur_stmt(&bb - fcn.blocks.data(), it - bb.statements.begin());
            // `_0 = &mut *foo`, then `_1 = _0 as *mut T` where `foo: *mut T`
            // - Note: Accepts `_0 = &*foo; _1 = _0 as T` where `foo: T`
            if( it->is_Assign()
                && it->as_Assign().dst.is_Local()
                && it->as_Assign().src.is_Borrow()
                && it->as_Assign().src.as_Borrow().val.is_Deref()
                )
            {
                const auto& dst_lv = it->as_Assign().dst;
                auto src_lv = it->as_Assign().src.as_Borrow().val.clone_unwrapped();
                // Find the next use of this target lvalue
                for(auto it2 = it+1; it2 != bb.statements.end(); ++it2)
                {
                    // If it's a cast back to the original type, then replace with a direct assignment of the original value
                    if(it2->is_Assign()
                        && it2->as_Assign().src.is_Cast()
                        && it2->as_Assign().src.as_Cast().val == dst_lv
                        )
                    {
                        const auto& dst_ty = it2->as_Assign().src.as_Cast().type;
                        HIR::TypeRef    tmp;
                        const auto& orig_ty = state.get_lvalue_type(tmp, src_lv);
                        if( orig_ty == dst_ty )
                        {
                            DEBUG(state << "Reborrow and cast back - " << *it << " and " << *it2);
                            it2->as_Assign().src = std::move(src_lv);
                            break;
                        }
                    }
                    if( check_invalidates_lvalue(*it2, src_lv) )
                    {
                        break;
                    }
                }
            }

            // `_0 = foo as *const T; _1 = _0 as *mut T` where `foo: *mut T`
            // - Note: Accepts `_0 = foo as *const T; _1 = _0 as U` where `foo: U`
            if( it->is_Assign()
                && it->as_Assign().dst.is_Local()
                && it->as_Assign().src.is_Cast()
                && it->as_Assign().src.as_Cast().type.data().is_Pointer()
                )
            {
                const auto& dst_lv = it->as_Assign().dst;
                const auto& src_lv = it->as_Assign().src.as_Cast().val;
                // Find the next use of this target lvalue
                for(auto it2 = it+1; it2 != bb.statements.end(); ++it2)
                {
                    // If it's a cast back to the original type, then replace with a direct assignment of the original value
                    if(it2->is_Assign()
                        && it2->as_Assign().src.is_Cast()
                        && it2->as_Assign().src.as_Cast().val == dst_lv
                        )
                    {
                        const auto& dst_ty = it2->as_Assign().src.as_Cast().type;
                        HIR::TypeRef    tmp;
                        const auto& orig_ty = state.get_lvalue_type(tmp, src_lv);
                        if( orig_ty == dst_ty )
                        {
                            DEBUG(state << "Round-trip pointer cast - " << *it << " and " << *it2);
                            it2->as_Assign().src = src_lv.clone();
                            break;
                        }
                    }
                    if( check_invalidates_lvalue(*it2, src_lv) )
                    {
                        break;
                    }
                }
            }
        }

        for(auto it = bb.statements.begin(); it != bb.statements.end(); )
        {
            state.set_cur_stmt(&bb - fcn.blocks.data(), it - bb.statements.begin());

            // Placeholder: Asm block with empty template and no inputs/outputs/flags
            if( *it == MIR::Statement::make_Asm({}) )
            {
                DEBUG(state << "Empty ASM placeholder, remove - " << *it);
                it = bb.statements.erase(it);
                changed = true;

                continue;
            }

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
                && it->as_Assign().src.as_Borrow().val.clone_unwrapped() == it->as_Assign().dst
                )
            {
                DEBUG(state << "Useless assignment (v = &*v), remove - " << *it);
                it = bb.statements.erase(it);
                changed = true;
            
                continue ;
            }

            // Cast to the same type
            if( it->is_Assign()
                && it->as_Assign().src.is_Cast()
                && it->as_Assign().src.as_Cast().type == state.get_lvalue_type(tmp_ty, it->as_Assign().src.as_Cast().val)
                )
            {
                DEBUG(state << "No-op cast, replace with assignment - " << *it);
                auto v = mv$(it->as_Assign().src.as_Cast().val);
                it->as_Assign().src = MIR::RValue::make_Use({ mv$(v) });
                changed = true;

                ++ it;
                continue ;
            }

            // Drop of Copy type
            if( it->is_Drop()
                && state.lvalue_is_copy(it->as_Drop().slot)
                )
            {
                DEBUG(state << "Drop of Copy type, remove - " << *it);
                it = bb.statements.erase(it);
                changed = true;

                continue;
            }

            ++ it;
        }
    }

    return changed;
}

// --------------------------------------------------------------------
// If the first statement of a block is an assignment from a local, and all sources of that block assign to that local
// - Move the assigment backwards
// --------------------------------------------------------------------
bool MIR_Optimise_GotoAssign(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);

    // 1. Locate blocks that start with an elligable assignemnt 
    // - Target must be "simple" (not a static, no wrappers)
    // - Source can be any lvalue? Restrict to locals for now (static/deref assignment is a side-effect)
    //   > Restrict to single-read locals? Or replace the trigger statement with a reversed copy?
    // 2. Check all source blocks, and see if they assign to that block
    // > Terminator must be: GOTO, or CALL <lv> = ... (with the non-panic arm)
    // 3. If more than half the source blocks assign the source, then move up
    // - Any IF/SWITCH/... terminator blocks the optimisation
    for(auto& dst_bb : fcn.blocks)
    {
        if( dst_bb.statements.empty() )
            continue;
        auto bb_idx = &dst_bb - fcn.blocks.data();
        state.set_cur_stmt(bb_idx, 0);
        auto& stmt = dst_bb.statements[0];
        if( !stmt.is_Assign() )
            continue ;
        if( !stmt.as_Assign().src.is_Use() )
            continue ;
        auto& dst = stmt.as_Assign().dst;
        auto& src = stmt.as_Assign().src.as_Use();

        if( !dst.m_wrappers.empty() || dst.m_root.is_Static() )
            continue;
        if( !src.is_Local() )
            continue ;
        // Source must be a single-read local (so this assignment can be deleted)
        unsigned n_read = 0;
        unsigned n_borrow = 0;
        visit_mir_lvalues(state, fcn, [&](const auto& lv, auto vu) {
            if(lv.m_root == src.m_root) {
                switch(vu)
                {
                case ValUsage::Read:
                case ValUsage::Move:
                    n_read ++;
                    break;
                case ValUsage::Borrow:
                    n_borrow ++;
                    break;
                case ValUsage::Write:
                    // Don't care
                    break;
                }
            }
            return true;
            });
        state.set_cur_stmt(bb_idx, 0);
        if( n_read > 1 || n_borrow > 0 ) {
            DEBUG(state << "Source " << src << " is read " << n_read << " times and borrowed " << n_borrow);
            continue ;
        }
        DEBUG(state << "Eligible assignment (" << stmt << ")");

        // Find source blocks, check terminators/last
        std::vector<unsigned> sources;
        unsigned num_used = 0;
        for(const auto& src_bb : fcn.blocks)
        {
            unsigned bb_idx = &src_bb - fcn.blocks.data();
            bool used = false;
            visit_terminator_target(src_bb.terminator, [&](const auto& tgt) { 
                if( tgt == state.get_cur_block() ) {
                    used = true;
                    sources.push_back(bb_idx);
                }
                });
            if( used )
            {
                TU_MATCH_HDRA( (src_bb.terminator), { )
                TU_ARMA(Goto, e) {
                    if( src_bb.statements.empty() ) {
                        DEBUG(state << "BB" << bb_idx << " empty");
                    }
                    else if( TU_TEST1(src_bb.statements.back(), Assign, .dst == src) ) {
                        DEBUG("BB" << bb_idx << "/" << src_bb.statements.size() << " " << src_bb.statements.back());
                        num_used += 1;
                    }
                    else {
                        DEBUG("BB" << bb_idx << "/" << src_bb.statements.size() << " " << src_bb.statements.back() << " - Doesn't write");
                    }
                    }
                TU_ARMA(Call, e) {
                    if( e.ret_block != state.get_cur_block() ) {
                        DEBUG(state << "BB" << bb_idx << "/TERM " << src_bb.terminator << " - Not return block");
                    }
                    else if( e.ret_val != src ) {
                        DEBUG(state << "BB" << bb_idx << "/TERM " << src_bb.terminator << " - Doesn't write to source");
                    }
                    else {
                        num_used += 1;
                    }
                    }
                break; default:
                    DEBUG(state << "BB" << bb_idx << "/TERM " << src_bb.terminator << " - Wrong terminator type");
                    break;
                }
            }
        }

        // TODO: Allow if one arm doesn't update?
        // - What if a call invalidates the target?
        if( num_used < sources.size() ) {
            DEBUG(state << "- Not all sources set the value");
            continue ;
        }

        changed = true;

        // Time to edit.
        // 1. Update all sources
        for(auto bb_idx : sources)
        {
            auto& src_bb = fcn.blocks[bb_idx];

            if( TU_TEST1(src_bb.terminator, Call, .ret_val == src) )
            {
                DEBUG("- Source block: BB" << bb_idx << " - term " << src_bb.terminator);
                src_bb.terminator.as_Call().ret_val = dst.clone();
            }
            else if( !src_bb.statements.empty() && TU_TEST1(src_bb.statements.back(), Assign, .dst == src) )
            {
                DEBUG("- Source block: BB" << bb_idx << " - tail " << src_bb.statements.back());
                src_bb.statements.back().as_Assign().dst = dst.clone();
            }
            else
            {
                MIR_TODO(state, "Handle copying assignment to source");
            }
            if( !src_bb.statements.empty() )
            {
                DEBUG("+- BB" << bb_idx << "/" << (src_bb.statements.size()-1) << " " << src_bb.statements.back());
            }
            DEBUG("+- BB" << bb_idx << "/TERM " << src_bb.terminator);
        }
        // IF the value is `Copy` (i.e. the initial assignment could be expected to survive), then reverse the destination
        // - Can't do this, it's going to cause infinite recursion!
        if( false && state.lvalue_is_copy(dst) )
        {
            auto d = dst.clone();
            dst = mv$(src);
            src = mv$(d);
            DEBUG(state << "- Updated (" << stmt << ")");
        }
        else
        {
            stmt = MIR::Statement();
            DEBUG(state << "- Deleted");
        }
    }

    return changed;
}

// --------------------------------------------------------------------
// Find re-borrows of values that aren't otherwise used.
//
// - Look for `<local> = &[mut] *<local/arg>`
// - Check if the source is only ever used here (and in a drop)
// - If that's the case, replace usage with a move and delete the drop
//
// TODO: Could allow multiple uses if it's a shared borrow
// --------------------------------------------------------------------
bool MIR_Optimise_UselessReborrows(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);

    // TODO: This doesn't work if the assignment happens in a loop (can lead to multiple moves)
    // - Need to have a way of knowing if a block is a loop member
#if 0
    for(auto& blk : fcn.blocks)
    {
        for(auto& stmt : blk.statements)
        {
            state.set_cur_stmt(&blk - fcn.blocks.data(), &stmt - blk.statements.data());
            // Must be `<local> = &[mut] *<local/arg>`
            if( !stmt.is_Assign() )
                continue ;
            const auto& dst = stmt.as_Assign().dst;
            auto& src_rv = stmt.as_Assign().src;    // Will be updated at the end of the block
            if( !dst.is_Local() )
                continue;
            if( !src_rv.is_Borrow() )
                continue ;
            const auto& src_lv = src_rv.as_Borrow().val;
            if( src_lv.m_wrappers.size() != 1 )
                continue ;
            if( src_lv.m_wrappers[0].is_Deref() == false )
                continue ;
            if( !(src_lv.m_root.is_Local() || src_lv.m_root.is_Argument()) )
                continue ;
            auto src_slot = MIR::LValue(src_lv.m_root.clone(), {});
            // Ensure that the type is a borrow of the same class
            HIR::TypeRef tmp_ty;
            const auto& src_ty = state.get_lvalue_type(tmp_ty, src_slot);
            if(!TU_TEST1(src_ty.m_data, Borrow, .type == src_rv.as_Borrow().type))
                continue;
            DEBUG(state << "Suitable re-borrow operation");

            // With this value, count places it's used
            unsigned n_wrapped = 0;
            unsigned n_drop = 0;
            bool invalidate = false;
            StmtRef drop_pos;
            visit_mir_lvalues(state, fcn, [&](const auto& lv, ValUsage vu)->bool {
                if(lv == src_slot)
                {
                    auto pos = StmtRef(state.get_cur_block(), state.get_cur_stmt_ofs());
                    // Direct usage (only one), should be a drop (look that up)
                    const auto& blk = fcn.blocks.at(pos.bb_idx);
                    if( pos.stmt_idx < blk.statements.size() && blk.statements.at(pos.stmt_idx).is_Drop() )
                    {
                        DEBUG(state << " Dropped");
                        // Valid!
                        n_drop ++;
                        if( n_drop > 1 )
                        {
                            invalidate = true;
                        }
                        else
                        {
                            drop_pos = pos;
                        }
                    }
                    else
                    {
                        invalidate = true;
                    }
                }
                else if(lv.m_root == src_slot.m_root)
                {
                    DEBUG(state << " Wrapped usage");
                    // Wrapped usage (expect only one)
                    n_wrapped ++;
                    if(n_wrapped > 1)
                    {
                        invalidate = true;
                    }
                }
                else
                {
                    // Ignore
                }
                return false;
                });
            state.set_cur_stmt(&blk - fcn.blocks.data(), &stmt - blk.statements.data());
            if( invalidate )
            {
                DEBUG(state << "Source isn't suitable");
                continue ;
            }

            // Can now delete!
            if(n_drop > 0)
            {
                DEBUG(state << "Wiping drop at " << drop_pos);
                // Clear drop statement
                auto& drop_blk = fcn.blocks.at(drop_pos.bb_idx);
                auto& drop_stmt = drop_blk.statements.at(drop_pos.stmt_idx);
                drop_stmt = MIR::Statement();
            }
            // Update the initial assignment
            DEBUG(state << "Updating assignment");
            src_rv = ::MIR::RValue::make_Use(mv$(src_slot));

            changed = true;
        }
    }
#endif

    return changed;
}

// --------------------------------------------------------------------
// Clear all unused blocks
// --------------------------------------------------------------------
bool MIR_Optimise_GarbageCollect_Partial(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool rv = false;
    TRACE_FUNCTION_FR("", rv);
    ::std::vector<bool> visited( fcn.blocks.size() );
    visit_blocks(state, fcn, [&visited](auto bb, const auto& /*block*/) {
            assert( !visited[bb] );
            visited[bb] = true;
            });
    for(unsigned int i = 0; i < visited.size(); i ++)
    {
        auto& blk = fcn.blocks[i];
        if( blk.terminator.is_Incomplete() && blk.statements.empty() )
        {
        }
        else if( visited[i] )
        {
        }
        else
        {
            DEBUG("CLEAR bb" << i);
            blk.statements.clear();
            blk.terminator = ::MIR::Terminator::make_Incomplete({});
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
            // TODO: Consume through indexing/field accesses
            for(const auto& w : lv.m_wrappers)
            {
                if( w.is_Field() ) {
                }
                else {
                    return ;
                }
            }
            if( lv.m_root.is_Local() )
            {
                used_locals[lv.m_root.as_Local()] = true;
            }
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
            else if( const auto* e = stmt.opt_Asm2() )
            {
                for(const auto& p : e->params)
                {
                    if(p.is_Reg() && p.as_Reg().output)
                        assigned_lval(*p.as_Reg().output);
                }
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
        if( visited[i] )
        {
            auto lvalue_cb = [&](::MIR::LValue& lv, auto ) {
                if( lv.m_root.is_Local() )
                {
                    auto e = lv.m_root.as_Local();
                    MIR_ASSERT(state, e < local_rewrite_table.size(), "Variable out of range - " << lv);
                    // If the table entry for this variable is !0, it wasn't marked as used
                    MIR_ASSERT(state, local_rewrite_table.at(e) != ~0u, "LValue " << lv << " incorrectly marked as unused");
                    lv.m_root = ::MIR::LValue::Storage::new_Local(local_rewrite_table.at(e) );
                }
                for(auto& w : lv.m_wrappers)
                {
                    if( w.is_Index())
                    {
                        w = ::MIR::LValue::Wrapper::new_Index(local_rewrite_table.at( w.as_Index() ));
                    }
                }
                return false;
                };
            ::std::vector<bool> to_remove_statements(it->statements.size());
            for(auto& stmt : it->statements)
            {
                auto stmt_idx = &stmt - &it->statements.front();
                state.set_cur_stmt(i, stmt_idx);

                if( stmt == ::MIR::Statement() )
                {
                    DEBUG(state << "Remove " << stmt << " - Pure default");
                    to_remove_statements[stmt_idx] = true;
                    continue ;
                }

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

                    // HACK: Remove drop if it's of an unused value (TODO: only if it's conditional?)
                    if( se->slot.is_Local() && local_rewrite_table[se->slot.as_Local()] == ~0u )
                    {
                        DEBUG(state << "Remove " << stmt << " - Dropping non-set value");
                        to_remove_statements[stmt_idx] = true;
                        continue ;
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
            visit_mir_lvalues_mut(it->terminator, lvalue_cb);
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
                e.bb0 = block_rewrite_table[e.bb0];
                e.bb1 = block_rewrite_table[e.bb1];
                ),
            (Switch,
                for(auto& target : e.targets)
                    target = block_rewrite_table[target];
                ),
            (SwitchValue,
                for(auto& target : e.targets)
                    target = block_rewrite_table[target];
                e.def_target = block_rewrite_table[e.def_target];
                ),
            (Call,
                e.ret_block   = block_rewrite_table[e.ret_block];
                e.panic_block = block_rewrite_table[e.panic_block];
                )
            )

            // Delete all statements flagged in a bitmap for deletion
            assert(it->statements.size() == to_remove_statements.size());
            auto new_end = ::std::remove_if(it->statements.begin(), it->statements.end(), [&](const auto& s){
                    size_t stmt_idx = (&s - &it->statements.front());
                    return to_remove_statements[stmt_idx];
                    });
            it->statements.erase(new_end, it->statements.end());
        }
        ++it;
    }

    auto new_blocks_end = ::std::remove_if(fcn.blocks.begin(), fcn.blocks.end(), [&](const auto& bb) {
        size_t i = &bb - &fcn.blocks.front();
        if( !visited[i] ) {
            DEBUG("GC bb" << i);
        }
        return !visited[i];
        });
    fcn.blocks.erase(new_blocks_end, fcn.blocks.end());

    // NOTE: Drop flags are bool, so can't use the above hack
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


#if 0
// --------------------------------------------------------------------
// Detect patterns
// --------------------------------------------------------------------
bool MIR_Optimise_PatternRec(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    // ASSIGN _1 = VARIANT ? #? (_0: &[T]/&str);
    // ASSIGN _2 = _0@?.1 (where the variant is (usize, usize)
    // ->
    // ASSIGN _2 = DSTMETA _0
}
#endif


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
            //if( ! dynamic_cast<::HIR::ExprNode_Block*>(expr.get()) ) {
            //    return ;
            //}
            auto& mir = expr.get_mir_or_error_mut(Span());
            if( do_minimal_optimisation ) {
                MIR_OptimiseMin(res, p, mir, args, ty);
            }
            else {
                MIR_Optimise(res, p, mir, args, ty);
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

                MIR_Cleanup(resolve, ip, *mono_fcn.code, mono_fcn.arg_tys, mono_fcn.ret_ty);
            }
            else if( hir_fcn.m_code )
            {
                auto& mir = hir_fcn.m_code.get_mir_or_error_mut(Span());
                bool did_opt = MIR_OptimiseInline(resolve, ip, mir, hir_fcn.m_args, hir_fcn.m_return, list);
                mir.trans_enum_state = ::MIR::EnumCachePtr();   // Clear MIR enum cache
                did_inline_on_pass |= did_opt;

                MIR_Cleanup(resolve, ip, mir, hir_fcn.m_args, hir_fcn.m_return);
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
