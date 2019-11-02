Problem: Top-level typecheck is fragile, doesn't handle bounds referencing
each other.


Can generate a similar rule structure to the expression handling?


Or, create a new typecheck helper that can handle partially-checked bounds



Requirements:
- UFCS resoluton in bounds and impl headers
   - Does this need to search other impls?
   - A quick check with the playpen implies that non-generics require fully-qualified paths.
   - But Generics don't.

