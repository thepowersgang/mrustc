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

## Entries
- Type equality rule (with "can coerce" flag)
 - Pointer to node for coercion

## Actions taken
- Destructuring and re-structurings are applied directly
 - 

```cpp

```


<!-- vim: ft=markdown
-->
