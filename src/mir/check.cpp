/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/check.cpp
 * - MIR Correctness validation
 */
#include "main_bindings.hpp"
#include "mir.hpp"
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>
#include <mir/helpers.hpp>
#include <mir/visit_crate_mir.hpp>

namespace {
    ::HIR::TypeRef get_metadata_type(const ::MIR::TypeResolve& state, const ::HIR::TypeRef& unsized_ty)
    {
        static Span sp;
        if( const auto* tep = unsized_ty.m_data.opt_TraitObject() )
        {
            const auto& trait_path = tep->m_trait;

            if( trait_path.m_path.m_path == ::HIR::SimplePath() )
            {
                return ::HIR::TypeRef::new_unit();
            }
            else
            {
                const auto& trait = *tep->m_trait.m_trait_ptr;

                const auto& vtable_ty_spath = trait.m_vtable_path;
                MIR_ASSERT(state, vtable_ty_spath != ::HIR::SimplePath(), "Trait with no vtable - " << trait_path);
                const auto& vtable_ref = state.m_resolve.m_crate.get_struct_by_path(state.sp, vtable_ty_spath);
                // Copy the param set from the trait in the trait object
                ::HIR::PathParams   vtable_params = trait_path.m_path.m_params.clone();
                // - Include associated types
                for(const auto& ty_b : trait_path.m_type_bounds) {
                    auto idx = trait.m_type_indexes.at(ty_b.first);
                    if(vtable_params.m_types.size() <= idx)
                        vtable_params.m_types.resize(idx+1);
                    vtable_params.m_types[idx] = ty_b.second.clone();
                }
                // TODO: This should be a pointer
                return ::HIR::TypeRef( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref );
            }
        }
        else if( unsized_ty.m_data.is_Slice() )
        {
            return ::HIR::CoreType::Usize;
        }
        else if( const auto* tep = unsized_ty.m_data.opt_Path() )
        {
            if( tep->binding.is_Struct() )
            {
                switch( tep->binding.as_Struct()->m_struct_markings.dst_type )
                {
                case ::HIR::StructMarkings::DstType::None:
                    return ::HIR::TypeRef();
                case ::HIR::StructMarkings::DstType::Possible: {
                    const auto& path = tep->path.m_data.as_Generic();
                    const auto& str = *tep->binding.as_Struct();
                    auto monomorph = [&](const auto& tpl) {
                        auto rv = monomorphise_type(state.sp, str.m_params, path.m_params, tpl);
                        state.m_resolve.expand_associated_types(sp, rv);
                        return rv;
                        };
                    TU_MATCHA( (str.m_data), (se),
                    (Unit,  MIR_BUG(state, "Unit-like struct with DstType::Possible - " << unsized_ty ); ),
                    (Tuple, return get_metadata_type( state, monomorph(se.back().ent) ); ),
                    (Named, return get_metadata_type( state, monomorph(se.back().second.ent) ); )
                    )
                    throw ""; }
                case ::HIR::StructMarkings::DstType::Slice:
                    return ::HIR::CoreType::Usize;
                case ::HIR::StructMarkings::DstType::TraitObject:
                    return ::HIR::TypeRef::new_unit();  // TODO: Get the actual inner metadata type?
                }
            }
            return ::HIR::TypeRef();
        }
        else
        {
            return ::HIR::TypeRef();
        }
    }
}

//template<typename T>
//::std::ostream& operator<<(::std::ostream& os, const T& v) {
//    v.fmt(os);
//    return os;
//}

// [ValState] = Value state tracking (use after move, uninit, ...)
// - [ValState] No drops or usage of uninitalised values (Uninit, Moved, or Dropped)
// - [ValState] Temporaries are write-once.
//  - Requires maintaining state information for all variables/temporaries with support for loops
void MIR_Validate_ValState(::MIR::TypeResolve& state, const ::MIR::Function& fcn)
{
    TRACE_FUNCTION;
    // > Iterate through code, creating state maps. Save map at the start of each bb.
    struct ValStates {
        enum class State {
            Invalid,
            Either,
            Valid,
        };
        State ret_state = State::Invalid;
        ::std::vector<State> args;
        ::std::vector<State> locals;

        ValStates() {}
        ValStates(size_t n_args, size_t n_locals):
            args(n_args, State::Valid),
            locals(n_locals)
        {
        }

        explicit ValStates(const ValStates& v) = default;
        ValStates(ValStates&& v) = default;
        ValStates& operator=(const ValStates& v) = delete;
        ValStates& operator=(ValStates&& v) = default;

        void fmt(::std::ostream& os) {
            os << "ValStates { ";
            switch(ret_state)
            {
            case State::Invalid:    break;
            case State::Either:
                os << "?";
            case State::Valid:
                os << "rv, ";
                break;
            }
            auto fmt_val_range = [&](const char* prefix, const auto& list) {
                for(auto range : runs(list)) {
                    switch(list[range.first])
                    {
                    case State::Invalid:    continue;
                    case State::Either: os << "?";  break;
                    case State::Valid:  break;
                    }
                    if( range.first == range.second ) {
                        os << prefix << range.first << ", ";
                    }
                    else {
                        os << prefix << range.first << "-" << prefix << range.second << ", ";
                    }
                }
                };
            fmt_val_range("a", this->args);
            fmt_val_range("_", this->locals);
            os << "}";
        }

        bool operator==(const ValStates& x) const {
            if( ret_state != x.ret_state )  return false;
            if( args      != x.args     )  return false;
            if( locals    != x.locals    )  return false;
            return true;
        }

        bool empty() const {
            return locals.empty() && args.empty();
        }

        // NOTE: Moves if this state is empty
        bool merge(unsigned bb_idx, ValStates& other)
        {
            DEBUG("bb" << bb_idx << " this=" << FMT_CB(ss,this->fmt(ss);) << ", other=" << FMT_CB(ss,other.fmt(ss);));
            if( this->empty() )
            {
                *this = ValStates(other);
                return true;
            }
            else if( *this == other )
            {
                return false;
            }
            else
            {
                bool rv = false;
                rv |= ValStates::merge_state(this->ret_state, other.ret_state);
                rv |= ValStates::merge_lists(this->args  , other.args  );
                rv |= ValStates::merge_lists(this->locals, other.locals);
                return rv;
            }
        }

        void mark_validity(const ::MIR::TypeResolve& state, const ::MIR::LValue& lv, bool is_valid)
        {
            if( !lv.m_wrappers.empty())
            {
                return ;
            }
            TU_MATCHA( (lv.m_root), (e),
            (Return,
                ret_state = is_valid ? State::Valid : State::Invalid;
                ),
            (Argument,
                MIR_ASSERT(state, e < this->args.size(), "Argument index out of range " << lv);
                DEBUG("arg$" << e << " = " << (is_valid ? "Valid" : "Invalid"));
                this->args[e] = is_valid ? State::Valid : State::Invalid;
                ),
            (Local,
                MIR_ASSERT(state, e < this->locals.size(), "Local index out of range - " << lv);
                DEBUG("_" << e << " = " << (is_valid ? "Valid" : "Invalid"));
                this->locals[e] = is_valid ? State::Valid : State::Invalid;
                ),
            (Static,
                )
            )
        }
        void ensure_valid(const ::MIR::TypeResolve& state, const ::MIR::LValue& lv)
        {
            TU_MATCHA( (lv.m_root), (e),
            (Return,
                if( this->ret_state != State::Valid )
                    MIR_BUG(state, "Use of non-valid lvalue - " << lv);
                ),
            (Argument,
                MIR_ASSERT(state, e < this->args.size(), "Arg index out of range");
                if( this->args[e] != State::Valid )
                    MIR_BUG(state, "Use of non-valid lvalue - " << lv);
                ),
            (Local,
                MIR_ASSERT(state, e < this->locals.size(), "Local index out of range");
                if( this->locals[e] != State::Valid )
                    MIR_BUG(state, "Use of non-valid lvalue - " << lv);
                ),
            (Static,
                )
            )

            for(const auto& w : lv.m_wrappers)
            {
                if( w.is_Index() )
                {
                    if( this->locals[w.as_Index()] != State::Valid )
                        MIR_BUG(state, "Use of non-valid lvalue - " << ::MIR::LValue::new_Local(w.as_Index()));
                }
            }
        }
        void move_val(const ::MIR::TypeResolve& state, const ::MIR::LValue& lv)
        {
            ensure_valid(state, lv);
            if( ! state.lvalue_is_copy(lv) )
            {
                mark_validity(state, lv, false);
            }
        }
        void move_val(const ::MIR::TypeResolve& state, const ::MIR::Param& p)
        {
            if( const auto* e = p.opt_LValue() )
            {
                move_val(state, *e);
            }
        }
    private:
        static bool merge_state(State& a, State& b)
        {
            bool rv = false;
            if( a != b )
            {
                // NOTE: This is an attempted optimisation to avoid re-running a block when it's not a new state.
                if( a == State::Either /*|| b == State::Either*/ ) {
                }
                else {
                    rv = true;
                }
                a = State::Either;
                b = State::Either;
            }
            return rv;
        }
        static bool merge_lists(::std::vector<State>& a, ::std::vector<State>& b)
        {
            bool rv = false;
            assert( a.size() == b.size() );
            // TODO: This is a really hot bit of code (according to valgrind), need to find a way of cooling it
            for(unsigned int i = 0; i < a.size(); i++)
            {
                rv |= merge_state(a[i], b[i]);
            }
            return rv;
        }
    };
    ::std::vector< ValStates>   block_start_states( fcn.blocks.size() );
    struct ToVisit {
        unsigned int bb;
        ::std::vector<unsigned int> path;
        ValStates   state;
    };
    // TODO: Remove this? The path is useful, but the cloned states are really expensive
    // - Option: Keep the paths, but only ever use the pre-set entry state?
    ::std::vector<ToVisit> to_visit_blocks;

    // TODO: Check that all used locals are also set (anywhere at all)

    auto add_to_visit = [&](unsigned int idx, ::std::vector<unsigned int> src_path, ValStates& vs, bool can_move) {
        for(const auto& b : to_visit_blocks)
            if( b.bb == idx && b.state == vs)
                return ;
        if( block_start_states.at(idx) == vs )
            return ;
        src_path.push_back(idx);
        // TODO: Update the target block, and only visit if we've induced a change
        to_visit_blocks.push_back( ToVisit { idx, mv$(src_path), (can_move ? mv$(vs) : ValStates(vs)) } );
        };
    auto add_to_visit_move = [&](unsigned int idx, ::std::vector<unsigned int> src_path, ValStates vs) {
        add_to_visit(idx, mv$(src_path), vs, true);
        };
    auto add_to_visit_copy = [&](unsigned int idx, ::std::vector<unsigned int> src_path, ValStates& vs) {
        add_to_visit(idx, mv$(src_path), vs, false);
        };
    add_to_visit_move( 0, {}, ValStates { state.m_args.size(), fcn.locals.size() } );
    while( to_visit_blocks.size() > 0 )
    {
        auto block = to_visit_blocks.back().bb;
        auto path = mv$(to_visit_blocks.back().path);
        auto val_state = mv$( to_visit_blocks.back().state );
        to_visit_blocks.pop_back();
        assert(block < fcn.blocks.size());

        // 1. Apply current state to `block_start_states` (merging if needed)
        // - If no change happened, skip.
        if( ! block_start_states.at(block).merge(block, val_state) ) {
            DEBUG("BB" << block << " via [" << path << "] nochange " << FMT_CB(ss,val_state.fmt(ss);));
            continue ;
        }
        ASSERT_BUG(Span(), val_state.locals.size() == fcn.locals.size(), "");
        DEBUG("BB" << block << " via [" << path << "] " << FMT_CB(ss,val_state.fmt(ss);));

        // 2. Using the newly merged state, iterate statements checking the usage and updating state.
        const auto& bb = fcn.blocks[block];
        for(unsigned int stmt_idx = 0; stmt_idx < bb.statements.size(); stmt_idx ++)
        {
            const auto& stmt = bb.statements[stmt_idx];
            state.set_cur_stmt(block, stmt_idx);

            DEBUG(state << stmt);
            switch( stmt.tag() )
            {
            case ::MIR::Statement::TAGDEAD:
                throw "";
            case ::MIR::Statement::TAG_SetDropFlag:
                break;
            case ::MIR::Statement::TAG_Drop:
                // Invalidate the slot
                if( stmt.as_Drop().flag_idx == ~0u )
                {
                    val_state.ensure_valid(state, stmt.as_Drop().slot);
                }
                val_state.mark_validity( state, stmt.as_Drop().slot, false );
                break;
            case ::MIR::Statement::TAG_Asm:
                for(const auto& v : stmt.as_Asm().inputs)
                    val_state.ensure_valid(state, v.second);
                for(const auto& v : stmt.as_Asm().outputs)
                    val_state.mark_validity( state, v.second, true );
                break;
            case ::MIR::Statement::TAG_Assign:
                // Destination must be valid
                for(const auto& w : stmt.as_Assign().dst.m_wrappers)
                {
                    if( w.is_Deref() ) {
                        // TODO: Check validity of the rest of the wrappers.
                    }
                    if( w.is_Index() )
                    {
                        if( val_state.locals[w.as_Index()] != ValStates::State::Valid )
                            MIR_BUG(state, "Use of non-valid lvalue - " << ::MIR::LValue::new_Local(w.as_Index()));
                    }
                }
                // Check source (and invalidate sources)
                TU_MATCH( ::MIR::RValue, (stmt.as_Assign().src), (se),
                (Use,
                    val_state.move_val(state, se);
                    ),
                (Constant,
                    //(void)state.get_const_type(se);
                    ),
                (SizedArray,
                    val_state.move_val(state, se.val);
                    ),
                (Borrow,
                    val_state.ensure_valid(state, se.val);
                    ),
                (Cast,
                    // Well.. it's not exactly moved...
                    val_state.ensure_valid(state, se.val);
                    //val_state.move_val(state, se.val);
                    ),
                (BinOp,
                    val_state.move_val(state, se.val_l);
                    val_state.move_val(state, se.val_r);
                    ),
                (UniOp,
                    val_state.move_val(state, se.val);
                    ),
                (DstMeta,
                    val_state.ensure_valid(state, se.val);
                    ),
                (DstPtr,
                    val_state.ensure_valid(state, se.val);
                    ),
                (MakeDst,
                    //val_state.move_val(state, se.ptr_val);
                    if( const auto* e = se.ptr_val.opt_LValue() )
                        val_state.ensure_valid(state, *e);
                    val_state.move_val(state, se.meta_val);
                    ),
                (Tuple,
                    for(const auto& v : se.vals)
                        val_state.move_val(state, v);
                    ),
                (Array,
                    for(const auto& v : se.vals)
                        val_state.move_val(state, v);
                    ),
                (Variant,
                    val_state.move_val(state, se.val);
                    ),
                (Struct,
                    for(const auto& v : se.vals)
                        val_state.move_val(state, v);
                    )
                )
                // Mark destination as valid
                val_state.mark_validity( state, stmt.as_Assign().dst, true );
                break;
            case ::MIR::Statement::TAG_ScopeEnd:
                //for(auto idx : stmt.as_ScopeEnd().vars)
                //    val_state.mark_validity(state, ::MIR::LValue::make_Variable(idx), false);
                //for(auto idx : stmt.as_ScopeEnd().tmps)
                //    val_state.mark_validity(state, ::MIR::LValue::make_Temporary({idx}), false);
                break;
            }
        }

        // 3. Pass new state on to destination blocks
        state.set_cur_stmt_term(block);
        DEBUG(state << bb.terminator);
        TU_MATCH_HDRA( (bb.terminator), { )
        TU_ARMA(Incomplete, e) {
            // Should be impossible here.
            }
        TU_ARMA(Return, e) {
            // Check if the return value has been set
            val_state.ensure_valid( state, ::MIR::LValue::new_Return() );
            // Ensure that no other non-Copy values are valid
            for(unsigned int i = 0; i < val_state.locals.size(); i ++)
            {
                if( val_state.locals[i] == ValStates::State::Invalid )
                {
                }
                else if( state.m_resolve.type_is_copy(state.sp, fcn.locals[i]) )
                {
                }
                else
                {
                    // TODO: Error, becuase this has just been leaked
                }
            }
            }
        TU_ARMA(Diverge, e) {
            // TODO: Ensure that cleanup has been performed.
            }
        TU_ARMA(Goto, e) {
            // Push block with the new state
            add_to_visit_move( e, mv$(path), mv$(val_state) );
            }
        TU_ARMA(Panic, e) {
            // What should be done here?
            }
        TU_ARMA(If, e) {
            // Push blocks
            val_state.ensure_valid( state, e.cond );
            add_to_visit_copy( e.bb0, path, val_state );
            add_to_visit_move( e.bb1, mv$(path), mv$(val_state) );
            }
        TU_ARMA(Switch, e) {
            val_state.ensure_valid( state, e.val );
            for(const auto& tgt : e.targets)
            {
                add_to_visit( tgt, path, val_state, (&tgt == &e.targets.back()) );
            }
            }
        TU_ARMA(SwitchValue, e) {
            val_state.ensure_valid( state, e.val );
            for(const auto& tgt : e.targets)
            {
                add_to_visit_copy( tgt, path, val_state );
            }
            add_to_visit_move( e.def_target, path, mv$(val_state) );
            }
        TU_ARMA(Call, e) {
            if( e.fcn.is_Value() )
                val_state.ensure_valid( state, e.fcn.as_Value() );
            for(const auto& arg : e.args)
                val_state.move_val( state, arg );
            // Push blocks (with return valid only in one)
            add_to_visit_copy(e.panic_block, path, val_state);

            // TODO: If the function returns !, don't follow the ret_block
            val_state.mark_validity( state, e.ret_val, true );
            add_to_visit_move(e.ret_block, mv$(path), mv$(val_state));
            }
        }
    }
}

void MIR_Validate(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, const ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    TRACE_FUNCTION_F(path);
    Span    sp;
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };
    // Validation rules:

    {
        for(const auto& bb : fcn.blocks)
        {
            state.set_cur_stmt_term(&bb - &fcn.blocks.front());
            MIR_ASSERT(state, bb.terminator.tag() != ::MIR::Terminator::TAGDEAD, "Moved terminator");
        }
    }
    // [CFA] = Control Flow Analysis
    // - [CFA] All code paths from bb0 must end with either a return or a diverge (or loop)
    //  - Requires checking the links between basic blocks, with a bitmap to catch loops/multipath
    {
        bool returns = false;
        ::std::vector<bool> visited_bbs( fcn.blocks.size() );
        ::std::vector<unsigned int> to_visit_blocks;
        to_visit_blocks.push_back(0);
        while( to_visit_blocks.size() > 0 )
        {
            auto block = to_visit_blocks.back();
            to_visit_blocks.pop_back();
            assert(block < fcn.blocks.size());
            if( visited_bbs[block] ) {
                continue ;
            }
            visited_bbs[block] = true;


            state.set_cur_stmt_term(block);

            #define PUSH_BB(idx, desc)  do {\
                if( !(idx < fcn.blocks.size() ) )   MIR_BUG(state,  "Invalid target block - " << desc << " bb" << idx);\
                if( visited_bbs[idx] == false ) {\
                    to_visit_blocks.push_back(idx); \
                }\
                } while(0)
            TU_MATCH(::MIR::Terminator, (fcn.blocks[block].terminator), (e),
            (Incomplete,
                MIR_BUG(state,  "Encounterd `Incomplete` block in control flow");
                ),
            (Return,
                returns = true;
                ),
            (Diverge,
                //can_panic = true;
                ),
            (Goto,
                PUSH_BB(e, "Goto");
                ),
            (Panic,
                PUSH_BB(e.dst, "Panic");
                ),
            (If,
                PUSH_BB(e.bb0, "If true");
                PUSH_BB(e.bb1, "If false");
                ),
            (Switch,
                for(unsigned int i = 0; i < e.targets.size(); i++ ) {
                    PUSH_BB(e.targets[i], "Switch V" << i);
                }
                ),
            (SwitchValue,
                for(unsigned int i = 0; i < e.targets.size(); i++ ) {
                    PUSH_BB(e.targets[i], "SwitchValue " << i);
                }
                PUSH_BB(e.def_target, "SwitchValue def");
                ),
            (Call,
                PUSH_BB(e.ret_block, "Call ret");
                PUSH_BB(e.panic_block, "Call panic");
                )
            )
            #undef PUSH_BB
        }
        if( !returns ) {
            DEBUG("- Function doesn't return.");
        }
    }

    // [Flat] = Basic checks (just iterates BBs)
    // - [Flat] Types must be valid (correct type for slot etc.)
    //  - Simple check of all assignments/calls/...
    DEBUG("=== FLAT CHECKS");
    {
        for(unsigned int bb_idx = 0; bb_idx < fcn.blocks.size(); bb_idx ++)
        {
            const auto& bb = fcn.blocks[bb_idx];
            for(unsigned int stmt_idx = 0; stmt_idx < bb.statements.size(); stmt_idx ++)
            {
                const auto& stmt = bb.statements[stmt_idx];
                state.set_cur_stmt(bb_idx, stmt_idx);
                DEBUG(state << stmt);

                switch( stmt.tag() )
                {
                case ::MIR::Statement::TAGDEAD:
                    throw "";
                case ::MIR::Statement::TAG_SetDropFlag:
                    break;
                case ::MIR::Statement::TAG_Assign: {
                    const auto& a = stmt.as_Assign();
                    ::HIR::TypeRef  dst_tmp;
                    const auto& dst_ty = state.get_lvalue_type(dst_tmp, a.dst);

                    auto check_types = [&](const auto& dst_ty, const auto& src_ty) {
                        if( src_ty == ::HIR::TypeRef::new_diverge() ) {
                            // It's valid to assign to anything from a !
                        }
                        else if( src_ty == dst_ty ) {
                            // Types are equal, good.
                        }
                        else {
                            MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is " << src_ty);
                        }
                        };
                    TU_MATCH(::MIR::RValue, (a.src), (e),
                    (Use,
                        ::HIR::TypeRef  tmp;
                        check_types( dst_ty, state.get_lvalue_type(tmp, e) );
                        ),
                    (Constant,
                        // TODO: Check constant types.
                        TU_MATCH( ::MIR::Constant, (e), (c),
                        (Int,
                            bool good = false;
                            if( dst_ty.m_data.is_Primitive() ) {
                                switch( dst_ty.m_data.as_Primitive() ) {
                                case ::HIR::CoreType::I8:
                                case ::HIR::CoreType::I16:
                                case ::HIR::CoreType::I32:
                                case ::HIR::CoreType::I64:
                                case ::HIR::CoreType::I128:
                                case ::HIR::CoreType::Isize:
                                    good = true;
                                    break;
                                default:
                                    break;
                                }
                            }
                            if( !good ) {
                                MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is a signed integer");
                            }
                            ),
                        (Uint,
                            bool good = false;
                            if( dst_ty.m_data.is_Primitive() ) {
                                switch( dst_ty.m_data.as_Primitive() ) {
                                case ::HIR::CoreType::U8:
                                case ::HIR::CoreType::U16:
                                case ::HIR::CoreType::U32:
                                case ::HIR::CoreType::U64:
                                case ::HIR::CoreType::U128:
                                case ::HIR::CoreType::Usize:
                                case ::HIR::CoreType::Char:
                                    good = true;
                                    break;
                                default:
                                    break;
                                }
                            }
                            if( !good ) {
                                MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is an unsigned integer");
                            }
                            ),
                        (Float,
                            bool good = false;
                            if( dst_ty.m_data.is_Primitive() ) {
                                switch( dst_ty.m_data.as_Primitive() ) {
                                case ::HIR::CoreType::F32:
                                case ::HIR::CoreType::F64:
                                    good = true;
                                    break;
                                default:
                                    break;
                                }
                            }
                            if( !good ) {
                                MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is a floating point value");
                            }
                            ),
                        (Bool,
                            check_types( dst_ty, ::HIR::TypeRef(::HIR::CoreType::Bool) );
                            ),
                        (Bytes,
                            check_types( dst_ty, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ::HIR::TypeRef::new_array(::HIR::CoreType::U8, c.size())) );
                            ),
                        (StaticString,
                            check_types( dst_ty, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ::HIR::CoreType::Str) );
                            ),
                        (Const,
                            // TODO: Check result type against type of const
                            ),
                        (ItemAddr,
                            // TODO: Check result type against pointer to item type
                            )
                        )
                        ),
                    (SizedArray,
                        // TODO: Check that return type is an array
                        // TODO: Check that the input type is Copy
                        ),
                    (Borrow,
                        ::HIR::TypeRef  tmp;
                        check_types( dst_ty, ::HIR::TypeRef::new_borrow(e.type, state.get_lvalue_type(tmp, e.val).clone()) );
                        ),
                    (Cast,
                        // Check return type
                        check_types( dst_ty, e.type );
                        // TODO: Check suitability of source type (COMPLEX)
                        ),
                    (BinOp,
                        /*
                        ::HIR::TypeRef  tmp_l, tmp_r;
                        const auto& ty_l = state.get_lvalue_type(tmp_l, e.val_l);
                        const auto& ty_r = state.get_lvalue_type(tmp_r, e.val_r);
                        // TODO: Check that operation is valid on these types
                        switch( e.op )
                        {
                        case ::MIR::eBinOp::BIT_SHR:
                        case ::MIR::eBinOp::BIT_SHL:
                            break;
                        default:
                            // Check argument types are equal
                            if( ty_l != ty_r )
                                MIR_BUG(state, "Type mismatch in binop, " << ty_l << " != " << ty_r);
                        }
                        */
                        // TODO: Check return type
                        ),
                    (UniOp,
                        // TODO: Check that operation is valid on this type
                        // TODO: Check return type
                        ),
                    (DstMeta,
                        ::HIR::TypeRef  tmp;
                        const auto& ty = state.get_lvalue_type(tmp, e.val);
                        const ::HIR::TypeRef*   ity_p = nullptr;
                        if( (ity_p = state.is_type_owned_box(ty)) )
                            ;
                        else if( ty.m_data.is_Borrow() )
                            ity_p = &*ty.m_data.as_Borrow().inner;
                        else if( ty.m_data.is_Pointer() )
                            ity_p = &*ty.m_data.as_Pointer().inner;
                        else {
                            MIR_BUG(state, "DstMeta requires a &-ptr as input, got " << ty);
                        }
                        const auto& ity = *ity_p;
                        if( ity.m_data.is_Generic() )
                            ;
                        else if( ity.m_data.is_Path() && ity.m_data.as_Path().binding.is_Opaque() )
                            ;
                        else if( ity.m_data.is_Array() )
                            ;
                        else if( ity.m_data.is_Slice() )
                            ;
                        else if( ity.m_data.is_TraitObject() )
                            ;
                        else if( ity.m_data.is_Path() )
                        {
                            // TODO: Check DST type of this path
                        }
                        else
                        {
                            MIR_BUG(state, "DstMeta on invalid type - " << ty);
                        }
                        // TODO: Check return type
                        ),
                    (DstPtr,
                        ::HIR::TypeRef  tmp;
                        const auto& ty = state.get_lvalue_type(tmp, e.val);
                        const ::HIR::TypeRef*   ity_p = nullptr;
                        if( (ity_p = state.is_type_owned_box(ty)) )
                            ;
                        else if( ty.m_data.is_Borrow() )
                            ity_p = &*ty.m_data.as_Borrow().inner;
                        else if( ty.m_data.is_Pointer() )
                            ity_p = &*ty.m_data.as_Pointer().inner;
                        else {
                            MIR_BUG(state, "DstPtr requires a &-ptr as input, got " << ty);
                        }
                        const auto& ity = *ity_p;
                        if( ity.m_data.is_Slice() )
                            ;
                        else if( ity.m_data.is_TraitObject() )
                            ;
                        else if( ity.m_data.is_Path() && ity.m_data.as_Path().binding.is_Opaque() )
                            ;
                        else if( ity.m_data.is_Path() )
                        {
                            // TODO: Check DST type of this path
                        }
                        else
                        {
                            MIR_BUG(state, "DstPtr on invalid type - " << ty);
                        }
                        // TODO: Check return type
                        ),
                    (MakeDst,
                        const ::HIR::TypeRef*   ity_p = nullptr;
                        if( const auto* te = dst_ty.m_data.opt_Borrow() )
                            ity_p = &*te->inner;
                        else if( const auto* te = dst_ty.m_data.opt_Pointer() )
                            ity_p = &*te->inner;
                        else {
                            MIR_BUG(state, "DstMeta requires a pointer as output, got " << dst_ty);
                        }
                        assert(ity_p);
                        auto meta = get_metadata_type(state, *ity_p);
                        if( meta == ::HIR::TypeRef() )
                        {
                            MIR_BUG(state, "DstMeta requires a pointer to an unsized type as output, got " << dst_ty);
                        }
                        // TODO: Check metadata type?

                        // NOTE: Output type checked above.
                        ),
                    (Tuple,
                        if( !dst_ty.m_data.is_Tuple() )
                            MIR_BUG(state, "Tuple assigned slot of invalid type, " << dst_ty);
                        const auto& dst_itys = dst_ty.m_data.as_Tuple();
                        if( dst_itys.size() != e.vals.size() )
                            MIR_BUG(state, "Tuple assigned slot of invalid type, " << dst_ty << " - expected " << e.vals.size() << " elements");
                        for(size_t i = 0; i < e.vals.size(); i++)
                        {
                            ::HIR::TypeRef  tmp2;
                            check_types( dst_itys[i], state.get_param_type(tmp2, e.vals[i]) );
                        }
                        ),
                    (Array,
                        // TODO: Check return type
                        ),
                    (Variant,
                        // TODO: Check return type
                        ),
                    (Struct,
                        // TODO: Check return type
                        )
                    )
                    } break;
                case ::MIR::Statement::TAG_Asm:
                    // TODO: Ensure that values are all thin pointers or integers?
                    break;
                case ::MIR::Statement::TAG_Drop:
                    // TODO: Anything need checking here?
                    break;
                case ::MIR::Statement::TAG_ScopeEnd:
                    // TODO: Mark listed values as descoped
                    break;
                }
            }

            state.set_cur_stmt_term(bb_idx);
            DEBUG(state << bb.terminator);
            TU_MATCH_HDRA( (bb.terminator), {)
            TU_ARMA(Incomplete, e) {
                }
            TU_ARMA(Return, e) {
                // TODO: Check if the function can return (i.e. if its return type isn't an empty type)
                }
            TU_ARMA(Diverge, e) {
                }
            TU_ARMA(Goto, e) {
                }
            TU_ARMA(Panic, e) {
                }
            TU_ARMA(If, e) {
                // Check that condition lvalue is a bool
                ::HIR::TypeRef  tmp;
                const auto& ty = state.get_lvalue_type(tmp, e.cond);
                if( ty != ::HIR::CoreType::Bool ) {
                    MIR_BUG(state, "Type mismatch in `If` - expected bool, got " << ty);
                }
                }
            TU_ARMA(Switch, e) {
                // Check that the condition is an enum
                }
            TU_ARMA(SwitchValue, e) {
                // Check that the condition's type matches the values
                }
            TU_ARMA(Call, e) {
                if( e.fcn.is_Value() )
                {
                    ::HIR::TypeRef  tmp;
                    const auto& ty = state.get_lvalue_type(tmp, e.fcn.as_Value());
                    if( ! ty.m_data.is_Function() )
                    {
                        MIR_BUG(state, "Call Fcn::Value with non-function type - " << ty);
                    }
                }
                // Typecheck arguments and return value
                }
            }
        }
    }

    // [ValState] = Value state tracking (use after move, uninit, ...)
    MIR_Validate_ValState(state, fcn);
}

// --------------------------------------------------------------------

void MIR_CheckCrate(/*const*/ ::HIR::Crate& crate)
{
    ::MIR::OuterVisitor    ov(crate, [](const auto& res, const auto& p, auto& expr, const auto& args, const auto& ty)
        {
            MIR_Validate(res, p, *expr.m_mir, args, ty);
        }
        );
    ov.visit_crate( crate );
}
