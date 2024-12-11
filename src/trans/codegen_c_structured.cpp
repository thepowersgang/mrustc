/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/codegen_c_structured.cpp
 * - Converts MIR into a semi-structured form
 */
#include <common.hpp>
#include <mir/mir.hpp>
#include <algorithm>
#include "codegen_c.hpp"

NodeRef::NodeRef(size_t idx):
    bb_idx(idx)
{
    DEBUG("NodeRef(" << idx << ")");
}
NodeRef::NodeRef(Node node_data):
    node(new Node(mv$(node_data))),
    bb_idx(SIZE_MAX)
{
    DEBUG("NodeRef(node)");
}
bool NodeRef::has_target() const
{
    if( node ) {
        TU_MATCHA( (*this->node), (e),
        (Block,
            return e.next_bb != SIZE_MAX;
            ),
        (If,
            return e.next_bb != SIZE_MAX;
            ),
        (Switch,
            return e.next_bb != SIZE_MAX;
            ),
        (SwitchValue,
            return e.next_bb != SIZE_MAX;
            ),
        (Loop,
            return e.next_bb != SIZE_MAX;
            )
        )
        throw "";
    }
    else {
        return true;
    }
}
size_t NodeRef::target() const
{
    if( node ) {
        TU_MATCHA( (*this->node), (e),
        (Block,
            return e.next_bb;
            ),
        (If,
            return e.next_bb;
            ),
        (Switch,
            return e.next_bb;
            ),
        (SwitchValue,
            return e.next_bb;
            ),
        (Loop,
            return e.next_bb;
            )
        )
        throw "";
    }
    else {
        return bb_idx;
    }
}

class Converter
{
    const ::MIR::Function& m_fcn;
public:
    ::std::vector<unsigned> m_block_ref_count;
    ::std::vector<bool> m_blocks_used;

    Converter(const ::MIR::Function& fcn):
        m_fcn(fcn)
    {

    }

    // Returns true if the passed block is the start of a self-contained sequence of blocks
    bool bb_is_opening(size_t bb_idx)
    {
        if( m_blocks_used[bb_idx] ) {
            return false;
        }
        else if( m_block_ref_count[bb_idx] > 1 ) {
            // TODO: Determine if these multiple references are from the block looping back on itself
            return false;
        }
        else {
            return true;
        }
    }
    NodeRef process_node_ref(size_t bb_idx)
    {
        if( bb_is_opening(bb_idx) ) {
            return NodeRef( process_node(bb_idx) );
        }
        else {
            return NodeRef(bb_idx);
        }
    }

    Node process_node(size_t bb_idx)
    {
        TRACE_FUNCTION_F(bb_idx);
        ::std::vector<NodeRef> refs;
        for(;;)
        {
            DEBUG("bb_idx = " << bb_idx);
            assert(bb_idx != SIZE_MAX);
            bool stop = false;
            assert( !m_blocks_used[bb_idx] );
            m_blocks_used[bb_idx] = true;

            refs.push_back( NodeRef(bb_idx) );

            const auto& blk = m_fcn.blocks.at(bb_idx);
            DEBUG("> " << blk.terminator);
            TU_MATCHA( (blk.terminator), (te),
            (Incomplete,
                stop = true;
                ),
            (Goto,
                bb_idx = te;
                ),
            (Panic,
                TODO(Span(), "Panic");
                ),
            (Diverge,
                stop = true;
                bb_idx = SIZE_MAX;
                ),
            (Return,
                stop = true;
                bb_idx = SIZE_MAX;
                ),
            (If,
                auto arm_t = process_node_ref(te.bb_true );
                auto arm_f = process_node_ref(te.bb_false);
                bb_idx = SIZE_MAX;
                if( arm_t.has_target() && arm_f.has_target() ) {
                    if( arm_t.target() == arm_f.target() ) {
                        DEBUG("If targets " << arm_t.target() << " == " << arm_f.target());
                        bb_idx = arm_t.target();
                    }
                    else {
                        stop = true;
                        DEBUG("If targets " << arm_t.target() << " != " << arm_f.target());
                        // TODO: Pick one?
                    }
                }
                else if( arm_t.has_target() ) {
                    DEBUG("If targets " << arm_t.target() << ", NONE");
                    bb_idx = arm_t.target();
                }
                else if( arm_f.has_target() ) {
                    DEBUG("If targets NONE, " << arm_f.target());
                    bb_idx = arm_f.target();
                }
                else {
                    // No target from either arm
                    DEBUG("If targets NONE, NONE");
                    stop = true;
                }
                refs.push_back(Node::make_If({ bb_idx, &te.cond, mv$(arm_t), mv$(arm_f) }));
                ),
            (Switch,
                ::std::vector<NodeRef>  arms;
                ::std::vector<size_t>   next_blocks;
                for(auto& tgt : te.targets)
                {
                    arms.push_back( process_node_ref(tgt) );
                    if( arms.back().has_target() )
                    {
                        next_blocks.push_back( arms.back().target() );
                    }
                }
                // TODO: Make the next block common
                ::std::sort(next_blocks.begin(), next_blocks.end());
                size_t  exit_bb = SIZE_MAX;
                if(!next_blocks.empty())
                {
                    size_t  cur = next_blocks[0];
                    size_t  cur_count = 0;
                    size_t  max_count = 0;
                    for(auto b : next_blocks)
                    {
                        if(cur == b) {
                            cur_count ++;
                        }
                        else {
                            if( cur_count > max_count ) {
                                exit_bb = cur;
                            }
                            cur = b;
                            cur_count = 1;
                        }
                    }
                    if( cur_count > max_count ) {
                        exit_bb = cur;
                    }
                }
                refs.push_back(Node::make_Switch({ exit_bb, &te.val, mv$(arms) }));
                bb_idx = exit_bb;
                if( bb_idx == SIZE_MAX )
                    stop = true;
                ),
            (SwitchValue,
                ::std::vector<NodeRef>  arms;
                ::std::vector<size_t>   next_blocks;
                for(auto& tgt : te.targets)
                {
                    arms.push_back( process_node_ref(tgt) );
                    if( arms.back().has_target() )
                    {
                        next_blocks.push_back( arms.back().target() );
                    }
                }
                auto def_arm = process_node_ref(te.def_target);
                if(def_arm.has_target())
                {
                    next_blocks.push_back(def_arm.target());
                }

                // TODO: Make the next block common
                ::std::sort(next_blocks.begin(), next_blocks.end());
                size_t  exit_bb = SIZE_MAX;
                if(!next_blocks.empty())
                {
                    size_t  cur = next_blocks[0];
                    size_t  cur_count = 0;
                    size_t  max_count = 0;
                    for(auto b : next_blocks)
                    {
                        if(cur == b) {
                            cur_count ++;
                        }
                        else {
                            if( cur_count > max_count ) {
                                exit_bb = cur;
                            }
                            cur = b;
                            cur_count = 1;
                        }
                    }
                    if( cur_count > max_count ) {
                        exit_bb = cur;
                    }
                }

                refs.push_back(Node::make_SwitchValue({ exit_bb, &te.val, mv$(def_arm), mv$(arms), &te.values }));
                stop = true;

                ),
            (Call,
                // NOTE: Let the panic arm just be a goto
                bb_idx = te.ret_block;
                )
            )

            assert(refs.empty() || refs.back().node || refs.back().bb_idx != SIZE_MAX);

            if( stop )
            {
                break;
            }

            // If `bb_idx` is in `refs` as a NodeRef
            auto it = ::std::find(refs.begin(), refs.end(), bb_idx);
            if( it != refs.end() )
            {
                // Wrap bb_idx-s from `it` to `refs.end()` in a `loop` block
                ::std::vector<NodeRef> loop_blocks;
                loop_blocks.reserve(refs.end() - it);
                for(auto it2 = it; it2 != refs.end(); ++it2)
                    loop_blocks.push_back( mv$(*it2) );
                refs.erase(it, refs.end());
                auto loop_node = NodeRef( Node::make_Block({ SIZE_MAX, mv$(loop_blocks) }) );

                refs.push_back( Node::make_Loop({ SIZE_MAX, mv$(loop_node) }) );
                // TODO: If there is only one `goto` in the above loop, assume it's the target
                DEBUG("Loop");
                break;
            }
            else if( bb_is_opening(bb_idx) )
            {
                DEBUG("Destination " << bb_idx << " is unreferenced+unvisited");
            }
            else
            {
                break;
            }
        }

        DEBUG("Block, target=" << bb_idx);
        for(auto& v : refs)
        {
            if( v.node )
                DEBUG((&v - refs.data()) << ": node");
            else
                DEBUG((&v - refs.data()) << ": bb" << v.bb_idx);
        }
        for(auto& v : refs)
            ASSERT_BUG(Span(), v.node || v.bb_idx != SIZE_MAX, (&v - refs.data()));
        return Node::make_Block({ bb_idx, mv$(refs) });
    }
};

::std::vector<Node> MIR_To_Structured(const ::MIR::Function& fcn)
{
    Converter   conv(fcn);
    conv.m_block_ref_count.resize( fcn.blocks.size() );
    conv.m_block_ref_count[0] += 1;
    for(const auto& blk : fcn.blocks)
    {
        TU_MATCHA( (blk.terminator), (te),
        (Incomplete,
            ),
        (Goto,
            conv.m_block_ref_count[te] += 1;
            ),
        (Panic,
            conv.m_block_ref_count[te.dst] += 1;
            ),
        (Diverge,
            ),
        (Return,
            ),
        (If,
            conv.m_block_ref_count[te.bb_true ] += 1;
            conv.m_block_ref_count[te.bb_false] += 1;
            ),
        (Switch,
            for(auto tgt : te.targets)
                conv.m_block_ref_count[tgt] += 1;
            ),
        (SwitchValue,
            for(auto tgt : te.targets)
                conv.m_block_ref_count[tgt] += 1;
            conv.m_block_ref_count[te.def_target] += 1;
            ),
        (Call,
            conv.m_block_ref_count[te.ret_block] += 1;
            conv.m_block_ref_count[te.panic_block] += 1;
            )
        )
    }

    // First Block: Becomes a block in structured output
    // - Terminator selects what the next block will be
    // - 

    // Find next unvisited block
    conv.m_blocks_used.resize( fcn.blocks.size() );
    ::std::vector<Node> nodes;
    for(size_t bb_idx = 0; bb_idx < fcn.blocks.size(); bb_idx ++)
    {
        if( conv.m_blocks_used[bb_idx] )
            continue;

        nodes.push_back( conv.process_node(bb_idx) );
    }


    // Return.
    return nodes;
}


