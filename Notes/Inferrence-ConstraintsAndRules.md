% Constaint-based type inferrence

## Information
- Strong type equalities
 - `let a = b;`
- Coercion points
 - `let a: &[_] = if k { b"1" } else { b"2" };`
 - `if k { 1 as *const _ } else { 1 as *mut _ };`
- De/Re-structuring
 - `let a = (b, c);`
- Outputs
 - `let a = b + c;`
- Primitives
 - `let a = 1;`
- Cast inferrence passthrough
 - `let a: u32 = 0; let b = &a as *const _;`

## Ideas
- Weak equalities (e.g. for casts)
 - Not needed, cast type can give what is needed

## Entries
- Type equality rule (with "can coerce" flag)
 - Pointer to node for coercion

## Actions taken
- Build up ruleset from HIR
 - Destructuring and re-structurings are applied directly
  - E.g. New tuples/arrays, destructuring patterns, ...
 - Direct equalities unify ivars
     - NOTE: Dead ivars (ones that have been aliased) should be omitted from listings
- Iterate rulesets
 - Apply coercions that are known to not coerce
  - E.g. `str` cannot coerce to anything
 - Check associated type projections
  - Apply single-trait rule (If there's only one trait that could apply, use it)
 - Run "revisit" nodes
 - Keep going until out or rules or ambiguity is hit


<!-- vim: ft=markdown
-->
