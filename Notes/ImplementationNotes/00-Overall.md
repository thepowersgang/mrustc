% MRustC structure overview

Overview
========

AST
---
Load
- "Parse" - Parsing and Lexing
- "LoadCrates" - Extern crate loading
- "Expand" - Attribute and macro expansion
Resolve
- "Resolve Use" - Fix up `use` statement paths
- "Resolve Index" - Build up an index of avaliable names in modules
- "Resolve Absolute" - Name resolution to items, variables, or type parameters.

HIR
---
- "HIR Lower" - Convert AST into HIR
Resolve
- "Resolve Type Aliases" - Replace references to `type` aliases with the referenced type
- "Resolve Bind" - Set binding pointers in paths
- "Resolve UFCS paths" - Determine if unknown UFCS paths point to trait methods or type methods
- "Constant Evaluate" - Constant evaluation (using a simple duck-typing system)
Typecheck
- "Typecheck Outer" - Checks types outside of expressions (where inferrence isn't needed)
- "Typecheck Expressions" - Performs type inferrence and checking within expressions.
Expand
- "Expand HIR Annotate" - Annotates how values are used for later passes
- "Expand HIR Closures" - Converts closure nodes into structs and trait impls
- "Expand HIR Calls" - Converts operator overloads into function calls.
- "Expand HIR Reborrows" - Inserts `&mut *` reborrows wherever a `&mut` would be moved.
- "Typecheck Expressions (validate)" - Runs typechecking again, this time without inferrence.

MIR
---
- "Lower MIR" - Converts HIR expression trees into MIR "functions"
- "MIR Validate" - Checks the sanity of the generated MIR


AST
===
The AST contains a direct translation of the source code into data structures, maintaining ordering and support for unexpanded macros and relative paths.

HIR
===
The HIR contains a far simpler module tree, only supports absolute paths, and doesn't contain higher-level syntatic constructs like `for` loops.

MIR
===
The MIR is just used for function bodies, and provides a pre-monomorphisation "assembly" to both simplify codegen and allow higher level optimisations.

