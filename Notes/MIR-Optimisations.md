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



De-Temporary
============

Purpose: Eliminate useless temporaries
```
_1 = _0.1
_2 = foo(_1)
```

Algorithm
---------

Where the pattern `Local(N) = Use(...)` is seen, record that statement as the assignment for `Local(N)`.

Detect any uses of `Local(N)` in mutation context (e.g. `Field(Local(N), 0) = ...`) or as a borrow and remove from the
list (as these cause the local to diverge from its source).

Detect any by-value or mutation uses of the origin value that would mean that moving the assignment forwards would
lead to a use-after-free or incorrect value.

If a recorded local is used, replace that usage by the original source. If the value was !Copy, and the use is a move,
 delete the original assignment.
- CATCH: Detecting a by-move of !Copy is hard when a Copy value is used from a !Copy struct, needs knowledge of the
  types involved.
- CATCH: Since partial moves are valid, the !Copy rule above is invalid if it would have been a partial move.
  - Only allow use of a !Copy result when it's just the slot.

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
        // 1. Check if Copy (if it is, don't delete any referenced values)
        delete = (usage == Move && !lv_is_copy(lv));
        // 2. Search for replacements
        visit_lvalues(lv, |ilv, usage2| {
            if Local(i) == ilv {
               ilv = replacement;
               if delete {
                    remove_replacement;
               }
            }
            });
        // Early-return (don't recuse)
        return true;
        });
}
```



Return Backprop
===============

Purpose: Eliminate useless temporaries in creating the return value

Locate `RETURN = Local(N)` and replace use of Local(N) as assignment destination with RETURN

Note: Only replace assignments that are setting the value that will become the return value.



Dead Assignment Elimination
===========================

Purpose: Remove dead code from previous passes

Remove assignments where the assigned value isn't read/borrowed before next assign

Constant Propagation
====================

Purpose: Allow deletion of useless code (`if false`) and expansion of `size_of` (and relevant optimisations)


Value Forward Propagation
=========================

Purpose: Determine when a value has the same value in all paths to a BB and move the assignment into that BB

