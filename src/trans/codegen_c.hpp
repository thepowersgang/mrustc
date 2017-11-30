/*
 */
#pragma once
#include <vector>
#include <memory>

class Node;

struct NodeRef
{
    ::std::unique_ptr<Node>    node;
    size_t  bb_idx;

    NodeRef(size_t idx): bb_idx(idx) {}
    NodeRef(Node node);

    bool has_target() const;
    size_t target() const;

    bool operator==(size_t idx) const {
        return !node && bb_idx == idx;
    }
};

TAGGED_UNION(Node, Block,
(Block, struct {
    size_t  next_bb;
    ::std::vector<NodeRef>  nodes;
    }),
(If, struct {
    size_t  next_bb;
    const ::MIR::LValue* val;
    NodeRef arm_false;
    NodeRef arm_true;
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
