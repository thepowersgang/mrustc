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

void MIR_Validate(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, const ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    TRACE_FUNCTION_F(path);
    Span    sp;
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };
    // Validation rules:

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

    // [ValState] = Value state tracking (use after move, uninit, ...)
    // - [ValState] No drops or usage of uninitalised values (Uninit, Moved, or Dropped)
    // - [ValState] Temporaries are write-once.
    //  - Requires maintaining state information for all variables/temporaries with support for loops
    {
        // > Iterate through code, creating state maps. Save map at the start of each bb.
        struct ValStates {
            enum class State {
                Invalid,
                Either,
                Valid,
            };
            State ret_state = State::Invalid;
            ::std::vector<State> arguments;
            ::std::vector<State> temporaries;
            ::std::vector<State> variables;

            ValStates() {}
            ValStates(size_t n_args, size_t n_temps, size_t n_vars):
                arguments(n_args, State::Valid),
                temporaries(n_temps),
                variables(n_vars)
            {
            }

            bool operator==(const ValStates& x) const {
                if( ret_state != x.ret_state )  return false;
                if( arguments != x.arguments )  return false;
                if( temporaries != x.temporaries )  return false;
                if( variables != x.variables )  return false;
                return true;
            }

            bool empty() const {
                return arguments.empty() && temporaries.empty() && variables.empty();
            }

            bool merge(ValStates& other)
            {
                if( this->empty() )
                {
                    *this = other;
                    return true;
                }
                else if( this->arguments == other.arguments && this->temporaries == other.temporaries && this->variables == other.variables )
                {
                    return false;
                }
                else
                {
                    bool rv = false;
                    rv |= ValStates::merge_lists(this->arguments, other.arguments);
                    rv |= ValStates::merge_lists(this->temporaries, other.temporaries);
                    rv |= ValStates::merge_lists(this->variables, other.variables);
                    return rv;
                }
            }

            void mark_validity(const ::MIR::TypeResolve& state, const ::MIR::LValue& lv, bool is_valid)
            {
                TU_MATCH_DEF( ::MIR::LValue, (lv), (e),
                (
                    ),
                (Return,
                    ret_state = is_valid ? State::Valid : State::Invalid;
                    ),
                (Argument,
                    MIR_ASSERT(state, e.idx < this->arguments.size(), "");
                    this->arguments[e.idx] = is_valid ? State::Valid : State::Invalid;
                    ),
                (Variable,
                    MIR_ASSERT(state, e < this->variables.size(), "");
                    this->variables[e] = is_valid ? State::Valid : State::Invalid;
                    ),
                (Temporary,
                    MIR_ASSERT(state, e.idx < this->temporaries.size(), "");
                    this->temporaries[e.idx] = is_valid ? State::Valid : State::Invalid;
                    )
                )
            }
            void ensure_valid(const ::MIR::TypeResolve& state, const ::MIR::LValue& lv)
            {
                TU_MATCH( ::MIR::LValue, (lv), (e),
                (Variable,
                    MIR_ASSERT(state, e < this->variables.size(), "");
                    if( this->variables[e] != State::Valid )
                        MIR_BUG(state, "Use of non-valid variable - " << lv);
                    ),
                (Temporary,
                    MIR_ASSERT(state, e.idx < this->temporaries.size(), "");
                    if( this->temporaries[e.idx] != State::Valid )
                        MIR_BUG(state, "Use of non-valid temporary - " << lv);
                    ),
                (Argument,
                    MIR_ASSERT(state, e.idx < this->arguments.size(), "");
                    if( this->arguments[e.idx] != State::Valid )
                        MIR_BUG(state, "Use of non-valid argument - " << lv);
                    ),
                (Return,
                    if( this->ret_state != State::Valid )
                        MIR_BUG(state, "Use of non-valid lvalue - " << lv);
                    ),
                (Static,
                    ),
                (Field,
                    ensure_valid(state, *e.val);
                    ),
                (Deref,
                    ensure_valid(state, *e.val);
                    ),
                (Index,
                    ensure_valid(state, *e.val);
                    ensure_valid(state, *e.idx);
                    ),
                (Downcast,
                    ensure_valid(state, *e.val);
                    )
                )
            }
            void move_val(const ::MIR::TypeResolve& state, const ::MIR::LValue& lv)
            {
                ensure_valid(state, lv);
                ::HIR::TypeRef  tmp;
                if( ! state.m_resolve.type_is_copy( state.sp, state.get_lvalue_type(tmp, lv) ) )
                {
                    mark_validity(state, lv, false);
                }
            }
        private:
            static bool merge_lists(::std::vector<State>& a, ::std::vector<State>& b)
            {
                bool rv = false;
                assert( a.size() == b.size() );
                for(unsigned int i = 0; i < a.size(); i++)
                {
                    if( a[i] != b[i] ) {
                        if( a[i] == State::Either || b[i] == State::Either ) {
                            rv = true;
                        }
                        a[i] = b[i] = State::Either;
                    }
                }
                return rv;
            }
        };
        ::std::vector< ValStates>   block_start_states( fcn.blocks.size() );
        ::std::vector< ::std::pair<unsigned int, ValStates> > to_visit_blocks;

        auto add_to_visit = [&](auto idx, auto vs) {
            for(const auto& b : to_visit_blocks)
                if( b.first == idx && b.second == vs)
                    return ;
            if( block_start_states.at(idx) == vs )
                return ;
            to_visit_blocks.push_back( ::std::make_pair(idx, mv$(vs)) );
            };
        to_visit_blocks.push_back( ::std::make_pair(0, ValStates{ args.size(), fcn.temporaries.size(), fcn.named_variables.size() }) );
        while( to_visit_blocks.size() > 0 )
        {
            auto block = to_visit_blocks.back().first;
            auto val_state = mv$( to_visit_blocks.back().second );
            to_visit_blocks.pop_back();
            assert(block < fcn.blocks.size());

            // 1. Apply current state to `block_start_states` (merging if needed)
            // - If no change happened, skip.
            if( ! block_start_states.at(block).merge( val_state ) ) {
                continue ;
            }

            // 2. Using the newly merged state, iterate statements checking the usage and updating state.
            const auto& bb = fcn.blocks[block];
            for(unsigned int stmt_idx = 0; stmt_idx < bb.statements.size(); stmt_idx ++)
            {
                const auto& stmt = bb.statements[stmt_idx];
                state.set_cur_stmt(block, stmt_idx);

                switch( stmt.tag() )
                {
                case ::MIR::Statement::TAGDEAD:
                    throw "";
                case ::MIR::Statement::TAG_Drop:
                    // Invalidate the slot
                    val_state.ensure_valid(state, stmt.as_Drop().slot);
                    val_state.mark_validity( state, stmt.as_Drop().slot, false );
                    break;
                case ::MIR::Statement::TAG_Asm:
                    for(const auto& v : stmt.as_Asm().inputs)
                        val_state.ensure_valid(state, v.second);
                    for(const auto& v : stmt.as_Asm().outputs)
                        val_state.mark_validity( state, v.second, true );
                    break;
                case ::MIR::Statement::TAG_Assign:
                    // Check source (and invalidate sources)
                    TU_MATCH( ::MIR::RValue, (stmt.as_Assign().src), (se),
                    (Use,
                        val_state.move_val(state, se);
                        ),
                    (Constant,
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
                        val_state.ensure_valid(state, se.ptr_val);
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
                }
            }

            // 3. Pass new state on to destination blocks
            state.set_cur_stmt_term(block);
            TU_MATCH(::MIR::Terminator, (bb.terminator), (e),
            (Incomplete,
                // Should be impossible here.
                ),
            (Return,
                // Check if the return value has been set
                val_state.ensure_valid( state, ::MIR::LValue::make_Return({}) );
                // Ensure that no other non-Copy values are valid
                for(unsigned int i = 0; i < val_state.variables.size(); i ++)
                {
                    if( val_state.variables[i] == ValStates::State::Invalid )
                    {
                    }
                    else if( state.m_resolve.type_is_copy(state.sp, fcn.named_variables[i]) )
                    {
                    }
                    else
                    {
                        // TODO: Error, becuase this has just been leaked
                    }
                }
                ),
            (Diverge,
                // TODO: Ensure that cleanup has been performed.
                ),
            (Goto,
                // Push block with the new state
                add_to_visit( e, mv$(val_state) );
                ),
            (Panic,
                // What should be done here?
                ),
            (If,
                // Push blocks
                val_state.ensure_valid( state, e.cond );
                add_to_visit( e.bb0, val_state );
                add_to_visit( e.bb1, mv$(val_state) );
                ),
            (Switch,
                val_state.ensure_valid( state, e.val );
                for(const auto& tgt : e.targets)
                {
                    add_to_visit( tgt, val_state );
                }
                ),
            (Call,
                if( e.fcn.is_Value() )
                    val_state.ensure_valid( state, e.fcn.as_Value() );
                for(const auto& arg : e.args)
                    val_state.ensure_valid( state, arg );
                // Push blocks (with return valid only in one)
                add_to_visit(e.panic_block, val_state);

                // TODO: If the function returns !, don't follow the ret_block
                val_state.mark_validity( state, e.ret_val, true );
                add_to_visit(e.ret_block, mv$(val_state));
                )
            )
        }
    }

    // [Flat] = Basic checks (just iterates BBs)
    // - [Flat] Types must be valid (correct type for slot etc.)
    //  - Simple check of all assignments/calls/...
    {
        for(unsigned int bb_idx = 0; bb_idx < fcn.blocks.size(); bb_idx ++)
        {
            const auto& bb = fcn.blocks[bb_idx];
            for(unsigned int stmt_idx = 0; stmt_idx < bb.statements.size(); stmt_idx ++)
            {
                const auto& stmt = bb.statements[stmt_idx];
                state.set_cur_stmt(bb_idx, stmt_idx);

                switch( stmt.tag() )
                {
                case ::MIR::Statement::TAGDEAD:
                    throw "";
                case ::MIR::Statement::TAG_Assign: {
                    const auto& a = stmt.as_Assign();

                    auto check_type = [&](const auto& src_ty) {
                        ::HIR::TypeRef  tmp;
                        const auto& dst_ty = state.get_lvalue_type(tmp, a.dst);
                        if( src_ty == ::HIR::TypeRef::new_diverge() ) {
                        }
                        else if( src_ty == dst_ty ) {
                        }
                        else {
                            MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is " << src_ty);
                        }
                        };
                    TU_MATCH(::MIR::RValue, (a.src), (e),
                    (Use,
                        ::HIR::TypeRef  tmp;
                        check_type( state.get_lvalue_type(tmp, e) );
                        ),
                    (Constant,
                        // TODO: Check constant types.
                        ::HIR::TypeRef  tmp;
                        const auto& dst_ty = state.get_lvalue_type(tmp, a.dst);
                        TU_MATCH( ::MIR::Constant, (e), (c),
                        (Int,
                            bool good = false;
                            if( dst_ty.m_data.is_Primitive() ) {
                                switch( dst_ty.m_data.as_Primitive() ) {
                                case ::HIR::CoreType::I8:
                                case ::HIR::CoreType::I16:
                                case ::HIR::CoreType::I32:
                                case ::HIR::CoreType::I64:
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
                            check_type( ::HIR::TypeRef(::HIR::CoreType::Bool) );
                            ),
                        (Bytes,
                            // TODO: Check result (could be either &[u8; N] or &[u8])
                            ),
                        (StaticString,
                            check_type( ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ::HIR::CoreType::Str) );
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
                        // TODO: Check return type
                        ),
                    (Cast,
                        // TODO: Check return type
                        // TODO: Check suitability of source type (COMPLEX)
                        ),
                    (BinOp,
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
                        // TODO: Check return type
                        ),
                    (UniOp,
                        // TODO: Check that operation is valid on this type
                        // TODO: Check return type
                        ),
                    (DstMeta,
                        // TODO: Ensure that the input type is a: Generic, Array, or DST
                        // TODO: Check return type
                        ),
                    (DstPtr,
                        // TODO: Ensure that the input type is a DST
                        // TODO: Check return type
                        ),
                    (MakeDst,
                        ),
                    (Tuple,
                        // TODO: Check return type
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
                }
            }

            state.set_cur_stmt_term(bb_idx);
            TU_MATCH(::MIR::Terminator, (bb.terminator), (e),
            (Incomplete,
                ),
            (Return,
                // TODO: Check if the function can return (i.e. if its return type isn't an empty type)
                ),
            (Diverge,
                ),
            (Goto,
                ),
            (Panic,
                ),
            (If,
                // Check that condition lvalue is a bool
                ::HIR::TypeRef  tmp;
                const auto& ty = state.get_lvalue_type(tmp, e.cond);
                if( ty != ::HIR::CoreType::Bool ) {
                    MIR_BUG(state, "Type mismatch in `If` - expected bool, got " << ty);
                }
                ),
            (Switch,
                // Check that the condition is an enum
                ),
            (Call,
                if( e.fcn.is_Value() )
                {
                    ::HIR::TypeRef  tmp;
                    const auto& ty = state.get_lvalue_type(tmp, e.fcn.as_Value());
                    if( ! ty.m_data.is_Function() )
                    {
                        //MIR_BUG(state, "Call Fcn::Value with non-function type - " << ty);
                    }
                }
                // Typecheck arguments and return value
                )
            )
        }
    }
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
