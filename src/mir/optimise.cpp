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

    bool visit_mir_lvalue_mut(::MIR::LValue& lv, bool is_write, ::std::function<bool(::MIR::LValue& , bool)> cb)
    {
        if( cb(lv, is_write) )
            return true;
        TU_MATCHA( (lv), (e),
        (Variable,
            ),
        (Argument,
            ),
        (Temporary,
            ),
        (Static,
            ),
        (Return,
            ),
        (Field,
            return visit_mir_lvalue_mut(*e.val, is_write, cb);
            ),
        (Deref,
            return visit_mir_lvalue_mut(*e.val, is_write, cb);
            ),
        (Index,
            bool rv = false;
            rv |= visit_mir_lvalue_mut(*e.val, is_write, cb);
            rv |= visit_mir_lvalue_mut(*e.idx, false, cb);
            return rv;
            ),
        (Downcast,
            return visit_mir_lvalue_mut(*e.val, is_write, cb);
            )
        )
        return false;
    }
    bool visit_mir_lvalue(const ::MIR::LValue& lv, bool is_write, ::std::function<bool(const ::MIR::LValue& , bool)> cb)
    {
        return visit_mir_lvalue_mut( const_cast<::MIR::LValue&>(lv), is_write, [&](auto& v, bool im) { return cb(v,im); } );
    }

    bool visit_mir_lvalues_mut(::MIR::RValue& rval, ::std::function<bool(::MIR::LValue& , bool)> cb)
    {
        bool rv = false;
        TU_MATCHA( (rval), (se),
        (Use,
            rv |= visit_mir_lvalue_mut(se, false, cb);
            ),
        (Constant,
            ),
        (SizedArray,
            rv |= visit_mir_lvalue_mut(se.val, false, cb);
            ),
        (Borrow,
            rv |= visit_mir_lvalue_mut(se.val, (se.type != ::HIR::BorrowType::Shared), cb);
            ),
        (Cast,
            rv |= visit_mir_lvalue_mut(se.val, false, cb);
            ),
        (BinOp,
            rv |= visit_mir_lvalue_mut(se.val_l, false, cb);
            rv |= visit_mir_lvalue_mut(se.val_r, false, cb);
            ),
        (UniOp,
            rv |= visit_mir_lvalue_mut(se.val, false, cb);
            ),
        (DstMeta,
            rv |= visit_mir_lvalue_mut(se.val, false, cb);
            ),
        (DstPtr,
            rv |= visit_mir_lvalue_mut(se.val, false, cb);
            ),
        (MakeDst,
            // TODO: Would prefer a flag to indicate "move"
            rv |= visit_mir_lvalue_mut(se.ptr_val, false, cb);
            rv |= visit_mir_lvalue_mut(se.meta_val, false, cb);
            ),
        (Tuple,
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, false, cb);
            ),
        (Array,
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, false, cb);
            ),
        (Variant,
            rv |= visit_mir_lvalue_mut(se.val, false, cb);
            ),
        (Struct,
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, false, cb);
            )
        )
        return rv;
    }
    bool visit_mir_lvalues(const ::MIR::RValue& rval, ::std::function<bool(const ::MIR::LValue& , bool)> cb)
    {
        return visit_mir_lvalues_mut(const_cast<::MIR::RValue&>(rval), [&](auto& lv, bool im){ return cb(lv, im); });
    }

    bool visit_mir_lvalues_mut(::MIR::Statement& stmt, ::std::function<bool(::MIR::LValue& , bool)> cb)
    {
        bool rv = false;
        TU_MATCHA( (stmt), (e),
        (Assign,
            rv |= visit_mir_lvalues_mut(e.src, cb);
            rv |= visit_mir_lvalue_mut(e.dst, true, cb);
            ),
        (Asm,
            for(auto& v : e.inputs)
                rv |= visit_mir_lvalue_mut(v.second, false, cb);
            for(auto& v : e.outputs)
                rv |= visit_mir_lvalue_mut(v.second, true, cb);
            ),
        (SetDropFlag,
            ),
        (Drop,
            rv |= visit_mir_lvalue_mut(e.slot, false, cb);
            )
        )
        return rv;
    }
    bool visit_mir_lvalues(const ::MIR::Statement& stmt, ::std::function<bool(const ::MIR::LValue& , bool)> cb)
    {
        return visit_mir_lvalues_mut(const_cast<::MIR::Statement&>(stmt), [&](auto& lv, bool im){ return cb(lv, im); });
    }

    void visit_mir_lvalues_mut(::MIR::Terminator& term, ::std::function<bool(::MIR::LValue& , bool)> cb)
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
            visit_mir_lvalue_mut(e.cond, false, cb);
            ),
        (Switch,
            visit_mir_lvalue_mut(e.val, false, cb);
            ),
        (Call,
            if( e.fcn.is_Value() ) {
                visit_mir_lvalue_mut(e.fcn.as_Value(), false, cb);
            }
            for(auto& v : e.args)
                visit_mir_lvalue_mut(v, false, cb);
            visit_mir_lvalue_mut(e.ret_val, true, cb);
            )
        )
    }

    void visit_mir_lvalues_mut(::MIR::TypeResolve& state, ::MIR::Function& fcn, ::std::function<bool(::MIR::LValue& , bool)> cb)
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
    void visit_mir_lvalues(::MIR::TypeResolve& state, const ::MIR::Function& fcn, ::std::function<bool(const ::MIR::LValue& , bool)> cb)
    {
        visit_mir_lvalues_mut(state, const_cast<::MIR::Function&>(fcn), [&](auto& lv, bool im){ return cb(lv, im); });
    }
}

bool MIR_Optimise_Inlining(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_PropagateSingleAssignments(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_UnifyTemporaries(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_UnifyBlocks(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_GarbageCollect(::MIR::TypeResolve& state, ::MIR::Function& fcn);

void MIR_Optimise(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    static Span sp;
    TRACE_FUNCTION_F(path);
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };

    // >> Replace targets that point to a block that is just a goto
    for(auto& block : fcn.blocks)
    {
        TU_MATCHA( (block.terminator), (e),
        (Incomplete,
            ),
        (Return,
            ),
        (Diverge,
            ),
        (Goto,
            e = get_new_target(state, e);
            ),
        (Panic,
            ),
        (If,
            e.bb0 = get_new_target(state, e.bb0);
            e.bb1 = get_new_target(state, e.bb1);
            ),
        (Switch,
            for(auto& target : e.targets)
                target = get_new_target(state, target);
            ),
        (Call,
            e.ret_block = get_new_target(state, e.ret_block);
            e.panic_block = get_new_target(state, e.panic_block);
            )
        )
    }

    // >> Merge blocks where a block goto-s to a single-use block.
    {
        ::std::vector<unsigned int> uses( fcn.blocks.size() );
        for(auto& block : fcn.blocks)
        {
            TU_MATCHA( (block.terminator), (e),
            (Incomplete,
                ),
            (Return,
                ),
            (Diverge,
                ),
            (Goto,
                uses[e] ++;
                ),
            (Panic,
                ),
            (If,
                uses[e.bb0] ++;
                uses[e.bb1] ++;
                ),
            (Switch,
                for(auto& target : e.targets)
                    uses[target] ++;
                ),
            (Call,
                uses[e.ret_block] ++;
                uses[e.panic_block] ++;
                )
            )
        }

        unsigned int i = 0;
        for(auto& block : fcn.blocks)
        {
            while( block.terminator.is_Goto() )
            {
                auto tgt = block.terminator.as_Goto();
                if( uses[tgt] != 1 )
                    break ;
                DEBUG("Append bb " << tgt << " to bb" << i);

                assert( &fcn.blocks[tgt] != &block );
                auto src_block = mv$(fcn.blocks[tgt]);
                fcn.blocks[tgt].terminator = ::MIR::Terminator::make_Incomplete({});

                for(auto& stmt : src_block.statements)
                    block.statements.push_back( mv$(stmt) );
                block.terminator = mv$( src_block.terminator );
            }
            i ++;
        }
    }

    bool change_happened;
    do
    {
        change_happened = false;

        // >> Inline short functions
        //change_happened |= MIR_Optimise_Inlining(state, fcn);

        // >> Propagate dead assignments
        while( MIR_Optimise_PropagateSingleAssignments(state, fcn) )
            ;

        // >> Unify duplicate temporaries
        // If two temporaries don't overlap in lifetime (blocks in which they're valid), unify the two
        change_happened = MIR_Optimise_UnifyTemporaries(state, fcn) || change_happened;

        // >> Combine Duplicate Blocks
        change_happened = MIR_Optimise_UnifyBlocks(state, fcn) || change_happened;
    } while( change_happened );


    // DEFENCE: Run validation _before_ GC (so validation errors refer to the pre-gc numbers)
    MIR_Validate(resolve, path, fcn, args, ret_type);
    // GC pass on blocks and variables
    // - Find unused blocks, then delete and rewrite all references.
    MIR_Optimise_GarbageCollect(state, fcn);
}


// --------------------------------------------------------------------
// If two temporaries don't overlap in lifetime (blocks in which they're valid), unify the two
// --------------------------------------------------------------------
bool MIR_Optimise_Inlining(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    for(auto& block : fcn.blocks)
    {
        if(auto* te = block.terminator.opt_Call())
        {
            if( ! te->fcn.is_Path() )
                continue ;

            // Check the size of the target function.
            // Inline IF:
            // - First BB ends with a call and total count is 3
            // - Statement count smaller than 10
        }
    }
    return false;
}

// --------------------------------------------------------------------
// If two temporaries don't overlap in lifetime (blocks in which they're valid), unify the two
// --------------------------------------------------------------------
bool MIR_Optimise_UnifyTemporaries(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    ::std::vector<bool> replacable( fcn.temporaries.size() );
    // 1. Enumerate which (if any) temporaries share the same type
    {
        unsigned int n_found = 0;
        for(unsigned int tmpidx = 0; tmpidx < fcn.temporaries.size(); tmpidx ++)
        {
            if( replacable[tmpidx] )
                continue ;
            for(unsigned int i = tmpidx+1; i < fcn.temporaries.size(); i ++ )
            {
                if( replacable[i] )
                    continue ;
                if( fcn.temporaries[i] == fcn.temporaries[tmpidx] )
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

    struct VarLifetime {
        ::std::vector<bool> blocks;

        VarLifetime(const ::MIR::Function& fcn):
            blocks(fcn.blocks.size())
        {
        }

        bool is_valid() const {
            for(auto v : blocks)
                if( v )
                    return true;
            return false;
        }
        bool overlaps(const VarLifetime& x) const {
            assert(blocks.size() == x.blocks.size());
            for(unsigned int i = 0; i < blocks.size(); i ++)
            {
                if( blocks[i] && x.blocks[i] )
                    return true;
            }
            return false;
        }
        void unify(const VarLifetime& x) {
            assert(blocks.size() == x.blocks.size());
            for(unsigned int i = 0; i < blocks.size(); i ++)
            {
                if( x.blocks[i] )
                    blocks[i] = true;
            }
        }
    };
    //::std::vector<VarLifetime>  var_lifetimes;
    ::std::vector<VarLifetime>  tmp_lifetimes( fcn.temporaries.size(), VarLifetime(fcn) );

    // 1. Calculate lifetimes of all variables/temporaries that are eligable to be merged
    // - Lifetime is from first write to last read. Borrows lead to the value being assumed to live forever
    // - > BUT: Since this is lazy, it's taken as only being the lifetime of non-Copy items (as determined by the drop call or a move)
    {
        auto mark_borrowed = [&](const ::MIR::LValue& lv) {
            if( const auto* ve = lv.opt_Temporary() ) {
                replacable[ve->idx] = false;
            }
            // TODO: Recurse!
            };

        struct State {
            //::std::vector<bool> vars;
            ::std::vector<bool> tmps;

            State() {}
            State(const ::MIR::Function& fcn):
                tmps(fcn.temporaries.size())
            {
            }

            bool merge(const State& other) {
                if( tmps.size() == 0 )
                {
                    assert(other.tmps.size() != 0);
                    tmps = other.tmps;
                    return true;
                }
                else
                {
                    assert(tmps.size() == other.tmps.size());
                    bool rv = false;
                    for(unsigned int i = 0; i < tmps.size(); i ++)
                    {
                        if( tmps[i] != other.tmps[i] && other.tmps[i] ) {
                            tmps[i] = true;
                            rv = true;
                        }
                    }
                    return rv;
                }
            }

            void mark_validity(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& lv, bool val) {
                if( const auto& ve = lv.opt_Temporary() ) {
                    tmps[ve->idx] = val;
                }
                else {
                }
            }
            void move_val(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& lv) {
                ::HIR::TypeRef  tmp;
                if( mir_res.m_resolve.type_is_copy( mir_res.sp, mir_res.get_lvalue_type(tmp, lv) ) ) {
                }
                else {
                    mark_validity(mir_res, lv, false);
                }
            }
        };
        ::std::vector<State>    block_states( fcn.blocks.size() );
        ::std::vector< ::std::pair<unsigned int, State> >   to_visit;
        auto add_to_visit = [&to_visit](unsigned int bb, State state) {
            to_visit.push_back( ::std::make_pair(bb, mv$(state)) );
            };
        to_visit.push_back( ::std::make_pair(0, State(fcn)) );
        while( !to_visit.empty() )
        {
            auto bb_idx = to_visit.back().first;
            auto val_state = mv$(to_visit.back().second);
            to_visit.pop_back();

            // 1. Merge with block state
            if( ! block_states[bb_idx].merge(val_state) )
                continue ;
            //DEBUG("BB" << bb_idx);

            // 2. Run block
            const auto& bb = fcn.blocks[bb_idx];
            for(unsigned int stmt_idx = 0; stmt_idx < bb.statements.size(); stmt_idx ++)
            {
                const auto& stmt = bb.statements[stmt_idx];
                state.set_cur_stmt(bb_idx, stmt_idx);

                switch( stmt.tag() )
                {
                case ::MIR::Statement::TAGDEAD:
                    throw "";
                case ::MIR::Statement::TAG_SetDropFlag:
                    break;
                case ::MIR::Statement::TAG_Drop:
                    val_state.mark_validity( state, stmt.as_Drop().slot, false );
                    break;
                case ::MIR::Statement::TAG_Asm:
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
                        mark_borrowed(se.val);
                        ),
                    (Cast,
                        ),
                    (BinOp,
                        ),
                    (UniOp,
                        ),
                    (DstMeta,
                        ),
                    (DstPtr,
                        ),
                    (MakeDst,
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
                block_states[bb_idx].merge(val_state);
            }

            // 3. During terminator, merge again
            state.set_cur_stmt_term(bb_idx);
            //DEBUG("- " << bb.terminator);
            TU_MATCH(::MIR::Terminator, (bb.terminator), (e),
            (Incomplete,
                // Should be impossible here.
                ),
            (Return,
                block_states[bb_idx].merge(val_state);
                ),
            (Diverge,
                ),
            (Goto,
                block_states[bb_idx].merge(val_state);
                // Push block with the new state
                add_to_visit( e, mv$(val_state) );
                ),
            (Panic,
                // What should be done here?
                ),
            (If,
                // Push blocks
                block_states[bb_idx].merge(val_state);
                add_to_visit( e.bb0, val_state );
                add_to_visit( e.bb1, mv$(val_state) );
                ),
            (Switch,
                block_states[bb_idx].merge(val_state);
                for(const auto& tgt : e.targets)
                {
                    add_to_visit( tgt, val_state );
                }
                ),
            (Call,
                for(const auto& arg : e.args)
                    val_state.move_val( state, arg );
                block_states[bb_idx].merge(val_state);
                // Push blocks (with return valid only in one)
                add_to_visit(e.panic_block, val_state);

                // TODO: If the function returns !, don't follow the ret_block
                val_state.mark_validity( state, e.ret_val, true );
                add_to_visit(e.ret_block, mv$(val_state));
                )
            )
        }

        // Convert block states into temp states
        for(unsigned int bb_idx = 0; bb_idx < block_states.size(); bb_idx ++)
        {
            for(unsigned int tmp_idx = 0; tmp_idx < block_states[bb_idx].tmps.size(); tmp_idx ++)
            {
                tmp_lifetimes[tmp_idx].blocks[bb_idx] = block_states[bb_idx].tmps[tmp_idx];
            }
        }
    }

    // 2. Unify variables of the same type with distinct non-overlapping lifetimes
    ::std::map<unsigned int, unsigned int> replacements;
    ::std::vector<bool> visited( fcn.temporaries.size() );
    bool replacement_needed = false;
    for(unsigned int tmpidx = 0; tmpidx < fcn.temporaries.size(); tmpidx ++)
    {
        if( ! replacable[tmpidx] )  continue ;
        if( visited[tmpidx] )   continue ;
        if( ! tmp_lifetimes[tmpidx].is_valid() )  continue ;
        visited[tmpidx] = true;

        for(unsigned int i = tmpidx+1; i < fcn.temporaries.size(); i ++)
        {
            if( !replacable[i] )
                continue ;
            if( fcn.temporaries[i] != fcn.temporaries[tmpidx] )
                continue ;
            if( ! tmp_lifetimes[i].is_valid() )  continue ;
            // Variables are of the same type, check if they overlap
            if( tmp_lifetimes[tmpidx].overlaps( tmp_lifetimes[i] ) )
                continue ;
            // They overlap, unify
            tmp_lifetimes[tmpidx].unify( tmp_lifetimes[i] );
            replacements[i] = tmpidx;
            replacement_needed = true;
            visited[i] = true;
        }
    }

    if( replacement_needed )
    {
        DEBUG("Replacing temporaries using {" << replacements << "}");
        visit_mir_lvalues_mut(state, fcn, [&](auto& lv, bool) {
            if( auto* ve = lv.opt_Temporary() ) {
                auto it = replacements.find(ve->idx);
                if( it != replacements.end() )
                {
                    MIR_DEBUG(state, lv << " => Temporary(" << it->second << ")");
                    ve->idx = it->second;
                    return true;
                }
            }
            return false;
            });
    }

    return replacement_needed;
}

// --------------------------------------------------------------------
// --------------------------------------------------------------------
bool MIR_Optimise_UnifyBlocks(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    TRACE_FUNCTION_F("");
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
        DEBUG("Unify blocks - " << replacements);
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
            TU_MATCHA( (bb.terminator), (te),
            (Incomplete,
                ),
            (Return,
                ),
            (Diverge,
                ),
            (Goto,
                patch_tgt(te);
                ),
            (Panic,
                patch_tgt(te.dst);
                ),
            (If,
                patch_tgt(te.bb0);
                patch_tgt(te.bb1);
                ),
            (Switch,
                for(auto& tgt : te.targets)
                    patch_tgt(tgt);
                ),
            (Call,
                patch_tgt(te.ret_block);
                patch_tgt(te.panic_block);
                )
            )
            //DEBUG("- " << bb.terminator);
        }

        for(const auto& r : replacements)
        {
            fcn.blocks[r.first] = ::MIR::BasicBlock {};
            //auto _ = mv$(fcn.blocks[r.first].terminator);
        }

        return true;
    }
    else
    {
        return false;
    }
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
    };
    struct {
        ::std::vector<ValUse> var_uses;
        ::std::vector<ValUse> tmp_uses;

        void use_lvalue(const ::MIR::LValue& lv, bool is_write) {
            TU_MATCHA( (lv), (e),
            (Variable,
                auto& vu = var_uses[e];
                if( is_write )
                    vu.write += 1;
                else
                    vu.read += 1;
                ),
            (Argument,
                ),
            (Temporary,
                auto& vu = tmp_uses[e.idx];
                if( is_write )
                    vu.write += 1;
                else
                    vu.read += 1;
                ),
            (Static,
                ),
            (Return,
                ),
            (Field,
                use_lvalue(*e.val, is_write);
                ),
            (Deref,
                use_lvalue(*e.val, is_write);
                ),
            (Index,
                use_lvalue(*e.val, is_write);
                use_lvalue(*e.idx, false);
                ),
            (Downcast,
                use_lvalue(*e.val, is_write);
                )
            )
        }
        void read_lvalue(const ::MIR::LValue& lv) {
            use_lvalue(lv, false);
        }
        void write_lvalue(const ::MIR::LValue& lv) {
            use_lvalue(lv, true);
        }
    } val_uses = {
        ::std::vector<ValUse>(fcn.named_variables.size()),
        ::std::vector<ValUse>(fcn.temporaries.size())
        };
    visit_mir_lvalues(state, fcn, [&](const auto& lv, bool im){ val_uses.use_lvalue(lv, im); return false; });

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
                // TODO: Variables too (can eliminate arguments)
                if( e.dst.is_Temporary() )
                {
                    const auto& vu = val_uses.tmp_uses[e.dst.as_Temporary().idx];
                    DEBUG("VU " << e.dst << " R:" << vu.read << " W:" << vu.write);
                    // > Where the temporary is written once and read once
                    if( !( vu.read == 1 && vu.write == 1 ) )
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
                    const auto& src = e.src.as_Use();
                    if( !( src.is_Temporary() || src.is_Variable() || src.is_Argument() ) )
                        continue ;
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

                auto is_lvalue_usage = [&](const auto& lv, bool ){ return lv == e.dst; };

                auto is_lvalue_in_val = [&](const auto& lv) {
                    return visit_mir_lvalues(e.src, [&](const auto& slv, bool ) { return lv == slv; });
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
                    if( visit_mir_lvalues(block.statements[si2], [&](const auto& lv, bool is_write){ return is_write && is_lvalue_in_val(lv); }) )
                    {
                        stop = true;
                        break;
                    }
                }
                if( !stop )
                {
                    DEBUG(block.terminator);
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
                        if( src_is_lvalue && visit_mir_lvalue(e.cond, false, is_lvalue_usage) )
                            found = true;
                        stop = true;
                        ),
                    (Switch,
                        if( src_is_lvalue && visit_mir_lvalue(e.val, false, is_lvalue_usage) )
                            found = true;
                        stop = true;
                        ),
                    (Call,
                        if( e.fcn.is_Value() )
                            if( src_is_lvalue && visit_mir_lvalue(e.fcn.as_Value(), false, is_lvalue_usage) )
                                found = true;
                        for(const auto& v : e.args)
                        {
                            if( src_is_lvalue && visit_mir_lvalue(v, false, is_lvalue_usage) )
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

        for(;;)
        {
            unsigned int inner_replaced_count = 0;
            for(auto& r : replacements)
            {
                visit_mir_lvalues_mut(r.second, [&](auto& lv, bool is_write) {
                    if( !is_write )
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
            auto cb = [&](auto& lv, bool is_write){
                if( !is_write )
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
        // TODO
        //::std::map< ::MIR::LValue, ::MIR::RValue>    replacements;
        for(auto& block : fcn.blocks)
        {
            for(auto it = block.statements.begin(); it != block.statements.end(); ++it)
            {
                if( !it->is_Assign() )
                    continue;
                if( it->as_Assign().src.tag() == ::MIR::RValue::TAGDEAD )
                    continue ;
                auto& to_replace_lval = it->as_Assign().dst;
                if( const auto* e = to_replace_lval.opt_Temporary() ) {
                    const auto& vu = val_uses.tmp_uses[e->idx];
                    if( !( vu.read == 1 && vu.write == 1 ) )
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

                    // Ensure that the target doesn't change in the intervening time.
                    bool was_invalidated = false;
                    for(auto it3 = it+1; it3 != it2; it3++)
                    {
                        // Closure returns `true` if the passed lvalue is a component of `new_dst_lval`
                        auto is_lvalue_in_val = [&](const auto& lv) {
                            return visit_mir_lvalue(new_dst_lval, false, [&](const auto& slv, bool ) { return lv == slv; });
                            };
                        if( visit_mir_lvalues(*it3, [&](const auto& lv, bool is_write){ return is_write && is_lvalue_in_val(lv); }) )
                        {
                            was_invalidated = true;
                            break;
                        }
                    }

                    // Replacement is valid.
                    if( ! was_invalidated )
                    {
                        DEBUG("Replace assignment of " << to_replace_lval << " with " << new_dst_lval);
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
        for(auto& block : fcn.blocks)
        {
            if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;

            // If the terminator is a call that writes to a 1:1 value, replace the destination value with the eventual destination (if that value isn't used in the meantime)
            if( block.terminator.is_Call() )
            {
                // TODO: What if the destination located here is a 1:1 and its usage is listed to be replaced by the return value.
                auto& e = block.terminator.as_Call();
                // TODO: Support variables too?
                if( !e.ret_val.is_Temporary() )
                    continue ;
                const auto& vu = val_uses.tmp_uses[e.ret_val.as_Temporary().idx];
                if( !( vu.read == 1 && vu.write == 1 ) )
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
                        return visit_mir_lvalue(*new_dst, true, [&](const auto& slv, bool ) { return lv == slv; });
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
                        if( visit_mir_lvalues(stmt, [&](const auto& lv, bool is_write){ return lv == *new_dst || (is_write && lvalue_impacts_dst(lv)); }) )
                        {
                            break;
                        }
                    }
                }
            }
        }
    }

    // TODO: Detect if any optimisations happened, and return true in that case
    return replacement_happend;
}


// --------------------------------------------------------------------
// Remove all unused temporaries and blocks
// --------------------------------------------------------------------
bool MIR_Optimise_GarbageCollect(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    ::std::vector<bool> used_temps( fcn.temporaries.size() );
    ::std::vector<bool> visited( fcn.blocks.size() );
    ::std::vector< ::MIR::BasicBlockId> to_visit;
    to_visit.push_back( 0 );
    while( to_visit.size() > 0 )
    {
        auto bb = to_visit.back(); to_visit.pop_back();
        visited[bb] = true;
        const auto& block = fcn.blocks[bb];

        auto assigned_lval = [&](const ::MIR::LValue& lv) {
            if( lv.is_Temporary() )
                used_temps[lv.as_Temporary().idx] = true;
            };

        for(const auto& stmt : block.statements)
        {
            TU_IFLET( ::MIR::Statement, stmt, Assign, e,
                assigned_lval(e.dst);
            )
        }

        TU_MATCHA( (block.terminator), (e),
        (Incomplete,
            ),
        (Return,
            ),
        (Diverge,
            ),
        (Goto,
            if( !visited[e] )
                to_visit.push_back(e);
            ),
        (Panic,
            ),
        (If,
            if( !visited[e.bb0] )
                to_visit.push_back(e.bb0);
            if( !visited[e.bb1] )
                to_visit.push_back(e.bb1);
            ),
        (Switch,
            for(auto& target : e.targets)
                if( !visited[target] )
                    to_visit.push_back(target);
            ),
        (Call,
            if( !visited[e.ret_block] )
                to_visit.push_back(e.ret_block);
            if( !visited[e.panic_block] )
                to_visit.push_back(e.panic_block);

            assigned_lval(e.ret_val);
            )
        )
    }

    ::std::vector<unsigned int> block_rewrite_table;
    for(unsigned int i = 0, j = 0; i < fcn.blocks.size(); i ++)
    {
        block_rewrite_table.push_back( visited[i] ? j ++ : ~0u );
    }
    ::std::vector<unsigned int> temp_rewrite_table;
    unsigned int n_temp = fcn.temporaries.size();
    for(unsigned int i = 0, j = 0; i < n_temp; i ++)
    {
        if( !used_temps[i] )
        {
            DEBUG("GC Temporary(" << i << ")");
            fcn.temporaries.erase(fcn.temporaries.begin() + j);
        }
        temp_rewrite_table.push_back( used_temps[i] ? j ++ : ~0u );
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
            auto lvalue_cb = [&](auto& lv, bool ) {
                if( lv.is_Temporary() ) {
                    auto& e = lv.as_Temporary();
                    MIR_ASSERT(state, e.idx < temp_rewrite_table.size(), "Temporary out of range - " << lv);
                    // If the table entry for this temporary is !0, it wasn't marked as used
                    MIR_ASSERT(state, temp_rewrite_table.at(e.idx) != ~0u, "LValue " << lv << " incorrectly marked as unused");
                    e.idx = temp_rewrite_table.at(e.idx);
                }
                return false;
                };
            unsigned int stmt_idx = 0;
            for(auto& stmt : it->statements)
            {
                state.set_cur_stmt(i, stmt_idx);
                visit_mir_lvalues_mut(stmt, lvalue_cb);
                stmt_idx ++;
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
                visit_mir_lvalue_mut(e.cond, false, lvalue_cb);
                e.bb0 = block_rewrite_table[e.bb0];
                e.bb1 = block_rewrite_table[e.bb1];
                ),
            (Switch,
                visit_mir_lvalue_mut(e.val, false, lvalue_cb);
                for(auto& target : e.targets)
                    target = block_rewrite_table[target];
                ),
            (Call,
                if( e.fcn.is_Value() ) {
                    visit_mir_lvalue_mut(e.fcn.as_Value(), false, lvalue_cb);
                }
                for(auto& v : e.args)
                    visit_mir_lvalue_mut(v, false, lvalue_cb);
                visit_mir_lvalue_mut(e.ret_val, true, lvalue_cb);
                e.ret_block   = block_rewrite_table[e.ret_block];
                e.panic_block = block_rewrite_table[e.panic_block];
                )
            )

            ++it;
        }
    }

    // TODO: Detect if any optimisations happened, and return true in that case
    return false;
}

void MIR_OptimiseCrate(::HIR::Crate& crate)
{
    ::MIR::OuterVisitor ov { crate, [](const auto& res, const auto& p, auto& expr, const auto& args, const auto& ty)
        {
            MIR_Optimise(res, p, *expr.m_mir, args, ty);
        }
        };
    ov.visit_crate(crate);
}

