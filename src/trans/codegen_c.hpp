/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/codegen_c.hpp
 * - C codegen backend (internal header)
 */
#pragma once
#include <vector>
#include <memory>

class Node;

struct NodeRef
{
    ::std::unique_ptr<Node>    node;
    size_t  bb_idx;

    NodeRef(size_t idx);
    NodeRef(Node node);

    bool has_target() const;
    size_t target() const;

    bool operator==(size_t idx) const {
        return !node && bb_idx == idx;
    }
};

// A node corresponds to a C statement/block
TAGGED_UNION(Node, Block,
(Block, struct {
    size_t  next_bb;
    ::std::vector<NodeRef>  nodes;
    }),
(If, struct {
    size_t  next_bb;
    const ::MIR::LValue* val;
    NodeRef arm_true;
    NodeRef arm_false;
    }),
(Switch, struct {
    size_t  next_bb;
    const ::MIR::LValue* val;
    ::std::vector<NodeRef>  arms;
    }),
(SwitchValue, struct {
    size_t  next_bb;
    const ::MIR::LValue* val;
    NodeRef def_arm;
    ::std::vector<NodeRef>  arms;
    const ::MIR::SwitchValues*  vals;
    }),
(Loop, struct {
    size_t  next_bb;
    NodeRef code;
    })
);

extern ::std::vector<Node> MIR_To_Structured(const ::MIR::Function& fcn);
