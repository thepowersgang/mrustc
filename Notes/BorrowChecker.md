
Lifetime Annotations
====================

Problems
--------

Higher-ranked bounds (lifetimes) can be nested, and typecheck/inferrence can change that nesting after resolve
- Potential solution: each HRB block (`for<...>`) is numbered within one of three scopes, the impl scope, item scope, and body scope.
  - Impl scope is for bounds that are visible for the entire `impl` or type block
  - Item scope is for bounds visible within the current impl item (`fn`, `type`, ...)
  - Body scope is for bounds within the body of a function (e.g. in type annotations)
  - Each of these scopes specifies a prefix to the binding IDs, similar to how currently there's three prefixes for generics (impl, item, placeholder)

Region inferrence
- To what extent does type inferrene play into lifetime inferrence? Can lifetimes be inferred as a second pass after
  types are concretely known, or do they play into each other?


Notes on Borrowck
=================

- On borrow, calculate lifetime of asignment (using existing lifetime code)
  - Ignore reborrows?
- Visit all statements in that lifetime and locate places where the borrow is propagated/stored
  - Requires lifetime parameters on functions/&-ptrs to be present
- Assignment of the source value during the lifetime of the borrow is an error
- Dropping of the source value is an error
- Returning the borrow is an error
