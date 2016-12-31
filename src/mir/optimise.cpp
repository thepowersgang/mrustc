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
            if( visit_mir_lvalue_mut(*e.val, is_write, cb) )
                return true;
            return visit_mir_lvalue_mut(*e.idx, false, cb);
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
    //bool visit_mir_lvalues(const ::MIR::RValue& rval, ::std::function<bool(const ::MIR::LValue& , bool)> cb)
    //{
    //    return visit_mir_lvalues_mut(const_cast<::MIR::RValue&>(rval), [&](auto& lv, bool im){ return cb(lv, im); });
    //}

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

    void visit_mir_lvalues_mut(::MIR::TypeResolve& state, ::MIR::Function& fcn, ::std::function<bool(::MIR::LValue& , bool)> cb)
    {
        for(unsigned int block_idx = 0; block_idx < fcn.blocks.size(); block_idx ++)
        {
            auto& block = fcn.blocks[block_idx];
            if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;
            for(auto& stmt : block.statements)
            {
                state.set_cur_stmt(block_idx, (&stmt - &block.statements.front()));
                visit_mir_lvalues_mut(stmt, cb);
            }
            state.set_cur_stmt_term(block_idx);
            TU_MATCHA( (block.terminator), (e),
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
    }
    void visit_mir_lvalues(::MIR::TypeResolve& state, const ::MIR::Function& fcn, ::std::function<bool(const ::MIR::LValue& , bool)> cb)
    {
        visit_mir_lvalues_mut(state, const_cast<::MIR::Function&>(fcn), [&](auto& lv, bool im){ return cb(lv, im); });
    }

}

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

                for(auto& stmt : src_block.statements)
                    block.statements.push_back( mv$(stmt) );
                block.terminator = mv$( src_block.terminator );
            }
            i ++;
        }
    }

    // >> Combine Duplicate Blocks
    // TODO:


    // >> Propagate dead assignments
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

    // Rules:
    // - Find an assignment `tmp = Use(...)` where the temporary is only written and read once
    // - Stop on any conditional terminator
    // - Any lvalues in the source lvalue must not be mutated between the source assignment and the usage.
    //  > This includes mutation, borrowing, or moving.
    {
        // 1. Function returns (reverse propagate)
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
        // 2. Assignments (forward propagate)
        ::std::map< unsigned int, ::MIR::LValue>    replacements;
        for(const auto& block : fcn.blocks)
        {
            if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;
            //FOREACH_IDX(stmt_idx, block.statements)
            for(unsigned int stmt_idx = 0; stmt_idx < block.statements.size(); stmt_idx ++)
            {
                const auto& stmt = block.statements[stmt_idx];
                // > Assignment
                if( ! stmt.is_Assign() )
                    continue ;
                const auto& e = stmt.as_Assign();
                // > Of a temporary from with a RValue::Use
                // TODO: Variables too.
                // TODO: Allow any rvalue type (if the usage is a RValue::Use)
                if( !( e.dst.is_Temporary() && e.src.is_Use() ) )
                    continue ;
                auto idx = e.dst.as_Temporary().idx;
                const auto& vu = val_uses.tmp_uses[idx];
                // > Where the temporary is written once and read once
                if( !( vu.read == 1 && vu.write == 1 ) )
                    continue ;

                // Get the root value(s) of the source
                // TODO: Handle more complex values. (but don't bother for really complex values?)
                const auto& src = e.src.as_Use();
                if( !( src.is_Temporary() || src.is_Variable() ) )
                    continue ;
                bool src_is_lvalue = true;

                auto is_lvalue_usage = [&](const auto& lv, bool ){ return lv == e.dst; };

                auto is_lvalue_in_val = [&](const auto& lv) {
                    return visit_mir_lvalue(src, true, [&](const auto& slv, bool ) { return lv == slv; });
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
                        // TODO: If the source isn't a Use, ensure that this is a Use
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
                    TU_MATCHA( (block.terminator), (e),
                    (Incomplete,
                        ),
                    (Return,
                        ),
                    (Diverge,
                        ),
                    (Goto,
                        // TODO: Chain.
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
                            if( src_is_lvalue && visit_mir_lvalue(v, false, is_lvalue_usage) )
                                found = true;
                        stop = true;
                        )
                    )
                }
                // Schedule a replacement in a future pass
                if( found )
                {
                    DEBUG("> Replace " << e.dst << " with " << e.src.as_Use());
                    replacements.insert( ::std::make_pair(idx, e.src.as_Use().clone()) );
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
                visit_mir_lvalue_mut(r.second, false, [&](auto& lv, bool is_write) {
                    if( lv.is_Temporary() && !is_write )
                    {
                        auto idx = lv.as_Temporary().idx;
                        auto it = replacements.find(idx);
                        if( it != replacements.end() )
                        {
                            lv = it->second.clone();
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
            visit_mir_lvalues_mut(state, fcn, [&](auto& lv, bool is_write){
                if( lv.is_Temporary() && !is_write )
                {
                    auto idx = lv.as_Temporary().idx;
                    auto it = replacements.find(idx);
                    if( it != replacements.end() )
                    {
                        MIR_ASSERT(state, it->second.tag() != ::MIR::LValue::TAGDEAD, "Replacement of  " << lv << " fired twice");
                        lv = ::std::move(it->second);
                        replaced += 1;
                    }
                }
                return false;
                });
            MIR_ASSERT(state, replaced > old_replaced, "Temporary eliminations didn't advance");
        }
        // Remove assignments of replaced values
        for(auto& block : fcn.blocks)
        {
            for(auto it = block.statements.begin(); it != block.statements.end(); )
            {
                // If the statement was an assign of a replaced temporary, remove it.
                if( it->is_Assign() && it->as_Assign().dst.is_Temporary() && replacements.count( it->as_Assign().dst.as_Temporary().idx ) > 0 )
                    it = block.statements.erase(it);
                else
                    ++it;
            }
        }
    }

    // GC pass on blocks and variables
    // - Find unused blocks, then delete and rewrite all references.
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
    }
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

