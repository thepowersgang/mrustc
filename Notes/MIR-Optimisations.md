% MIR Optimisations (Design Notes)

De-Tuple
========

Purpose: Remove useless tuples created in `match (a, b) { (a,b) => { ... }}`

Algorithm
---------

Detect `Local(N) = Tuple(...)` and record.
If the contents of the tuple is invalidated, delete the record
If a recorded local is moved, delete record
If a recorded local is mutated, delete record
If a recorded local is used via field access, record the field and location
- If a field is accessed twice, delete the record

At the end of the block, remove original tuple assignment and propagate the values to destinations.



De-Temporary (version 1)
========================

Purpose: Eliminate useless temporaries
```
_1 = _0.1
_2 = foo(_1)
```

Algorithm
---------

- Where the pattern `Local(N) = Use(...)` is seen, record that statement as the assignment for `Local(N)`.
- Detect any uses of `Local(N)` in mutation context (e.g. `Field(Local(N), 0) = ...`) or as a borrow and remove from the
  list (as these cause the local to diverge from its source).
- Detect any by-value or mutation uses of the origin value that would mean that moving the assignment forwards would
  lead to a use-after-free or incorrect value.

- Find any uses of locals recorded locals
  - If this is of a Copy value, the replacement
    - Copy values can be reused freely
  - If the value is used at the top level of the source RValue, do the replacemnet and remove the record
    - !Copy value moved, so can't be moved again.
  - Otherwise, remove the record
    - The value was used, so then assignment can't be replaced with later and deleted.

Pseudocode
----------

```
for stmt in block.statements
{
    check_for_mutate_or_borrow_of_dest();
    check_for_mutate_or_move_of_source();
    do_replacements(stmt);
    if let Assign(e) = stmt
    {
       if let Local(idx) = e.dst && let Use(_) = e.src
       {
           assign_list.insert(idx, stmt_idx)
       }
    }
}

fn do_replacements(stmt)
{
    visit_lvalues(stmt, |lv, usage| {
        top_level = true;
        // 1. Search for replacements
        visit_lvalues(lv, |ilv, usage2| {
            if Local(i) == ilv {
                if replacement = find_replacement(i) {
                    if is_copy(ilv) {
                        ilv = replacement
                    }
                    else if top_level {
                        ilv = replacement
                        remove_replacement(i)
                    }
                    else {
                        remove_replacement(i)
                    }
                }
            }
            });
        // Early-return (don't recuse)
        return true;
        });
}
```


Reverse De-temporary
====================

Allows removing useless temporaries (e.g. function returns)

IDEA ONLY.
- Find `... = Local(n)`
- Find where it was defined
- If the destination was invalidated in that time, don't do anything
- If it's mutated or otherwise accessed in the intervening time, don't do anything with it
- If the value is Copy and it's used elsewhere, don't do anything
- Otherwise, remove the assignment and move upwards

Return Backprop
===============

Purpose: Eliminate useless temporaries in creating the return value

Locate `RETURN = Local(N)` and replace use of Local(N) as assignment destination with RETURN

Note: Only replace assignments that are setting the value that will become the return value.

Algorithm
---------
TODO: 

Pseudocode
----------
TODO: 


Dead Assignment Elimination
===========================

Purpose: Remove dead code from previous passes

Remove assignments where the assigned value isn't read/borrowed before next assign

Algorithm
---------
- For all assignments of the form `Local(n) = ...`, seek forwards until the next use or loopback
- If the next use of that particular lvalue is an assignment, delete the original assignment
- If the lvalue is fully reassigned, delete the original assignment
  - Fully reassignment means that the LHS of Index/Field is mutated

Pseudocode
---------

Dead Drop Flag Elimination
==========================

Purpose: Remove unused (or unchanged) drop flags

Algorithm
---------
- Enumerate all changes to drop flags
- If a flag is never set, replace all uses with its default and delete
- If a flag is never used, delete


Constant Propagation
====================

Purpose: Allow deletion of useless code (`if false`) and expansion of `size_of` (and relevant optimisations)

Algorithm
---------
- Track assignments of slots with constants
- Erase record if the slot is invalidated
- Replace usage of the slot where possible with the constant
- Evaluate BinOps with both arguments a known slot


Value Forward Propagation
=========================

Purpose: Determine when a value has the same value in all paths to a BB and move the assignment into that BB

Inlining
========

Purpose: Inline simple methods

Algorithm
---------
- For each function call:
- Check if the called function is simple enough to be inlined
  - A single BB - inline
  - Three bbs (first being a function call) - inline
  - First bb ends with a switch of a constant parameter, and all arms point to return or a return bb

CFG Simplification
==================
Purpose: Remove gotos to blocks that only have a single use

