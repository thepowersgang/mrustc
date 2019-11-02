Problem description:
====================

There's a collection of hard-coded FFI hooks in `miri.cpp` to handle both API
rules (e.g. that the `write` syscall takes a count and a buffer of at least
that valid size) and wrapping non-trivial or dangerous calls (e.g. the pthread
ones).


It would be more useful to have a runtime description/encoding of these APIs
that runs safety checks and handles the magic.


Requirements
============

- Validity checks (pointer validity, tags)
  - These checks should provide useful error messages
- Returned allocation tagging (valid size, opaque tags)
  - Some returned allocations can be complex
- Completely re-definining operations
  - E.g. replacing pthread APIs with checked versions



Design Ideas
============

Raw MIR?
-------
- Downside: How would it differ from actual MIR?


Basic-alike instruction sequence
--------------------------------
- All lines start with a keyword
  - `ASSIGN`
  - `LET`
  - `CALL`


Simplified rust-ish language
----------------------------
- Basic type inference (uni-directional)
- Full AST

