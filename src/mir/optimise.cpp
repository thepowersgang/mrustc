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
}

void MIR_Optimise(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    static Span sp;
    TRACE_FUNCTION_F(path);
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };
    
    // 1. Replace targets that point to a block that is just a goto
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
    
    #if 0
    // GC pass on blocks and variables
    // - Find unused blocks, then delete and rewrite all references.
    {
        ::std::vector<bool> visited( fcn.blocks.size() );
        ::std::vector< ::MIR::BasicBlockId> to_visit;
        to_visit.push_back( 0 );
        while( to_visit.size() > 0 )
        {
            auto bb = to_visit.back(); to_visit.pop_back();
            visited[bb] = true;
            
            const auto& block = fcn.blocks[bb];
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
                )
            )
        }
        
        ::std::vector<unsigned int> rewrite_table;
        for(unsigned int i = 0, j = 0; i < fcn.blocks.size(); i ++)
        {
            if( visited[i] ) {
                rewrite_table.push_back(j++);
            }
            else {
                rewrite_table.push_back(~0u);
            }
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
                // Rewrite and advance
                TU_MATCHA( (it->terminator), (e),
                (Incomplete,
                    ),
                (Return,
                    ),
                (Diverge,
                    ),
                (Goto,
                    e = rewrite_table[e];
                    ),
                (Panic,
                    ),
                (If,
                    e.bb0 = rewrite_table[e.bb0];
                    e.bb1 = rewrite_table[e.bb1];
                    ),
                (Switch,
                    for(auto& target : e.targets)
                        target = rewrite_table[target];
                    ),
                (Call,
                    e.ret_block = rewrite_table[e.ret_block];
                    e.panic_block = rewrite_table[e.panic_block];
                    )
                )
                
                ++it;
            }
        }
    }
    #endif
}
