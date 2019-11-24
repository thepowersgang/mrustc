Target Load
- Parse the passes target specification
Parse: V V V
- Convert the source code 1:1 (excluding comments and whitespace) into AST (ordering is maintained)
LoadCrates: V V V
- Load any explicitly mentioned extern crates (note: not all crates appear here)
Expand: V V V
- Do AST transformations from attributes and macros, also loads the remaining extern crates (std/core, and any triggered by macro expansion)
- Macro expansion also will trigger parsers
Implicit Crates: V V V
- test harness, allocator crate, panic crate
Resolve Use: V V V
- Annotate every `use` item with its source (has to handle some nasty recursion)
Resolve Index: V V V
- Generate an index of visible items for every module (avoids needing recursioon in the next pass)
Resolve Absolute: V V V
- Resolve all paths into either variable names (types/values) or absolute paths.
HIR Lower: V V V
- Convert the now-excessive AST into a simpler format "HIR" (convers both expressions and the module tree)
Resolve Type Aliases: V V V
- Replace any usage of type aliases with the actual type (NOTE: Doesn't do associated types)
Resolve Bind: V V V
- Iterate the HIR tree and set binding annotations on all concrete types (avoids path lookups later)
Resolve HIR Markings: V V V
- Generate "markings" (e.g. for Copy/Sync/Send/...) for all types
Sort Impls: V V V
- (small pass) sort impls into groups (TODO: why is this done so late?)
Resolve UFCS Outer: V V V
- Determine source trait for all top-level <T>::Type paths (aka UfcsUnknown)
Resolve UFCS paths: V V V
- Do the same, but include for expressions this time
- Also normalises the results of the previous pass (expanding known associated types)
Constant Evaluate: V V V
- Evaluate all constants
- NOTE: This is the first stage that can peek forwards (can trigger typecheck+MIR generation for items)
Typecheck Outer: V V V
- Check that impls are sane.
Typecheck Expressions: V V V
- Resolve and check types for all expressions
Expand HIR Annotate: V V V
- Annotate how expressions are used (used for closure extraction and reborrows)
Expand HIR Closures: V V V
- Extract closures into structures implmenting Fn* traits
Expand HIR VTables: V V V
- Generate vtables for types (NOTE: Has to be done after closure generation)
Expand HIR Calls: V V V
- Converts method and callable calls into explicit function calls
Expand HIR Reborrows: V V V
- Apply reborrow rules (taking `&mut *v` instead of `v`)
Expand HIR ErasedType: V V V
- Replace all erased types `impl Trait` with the true type
Typecheck Expressions (validate): V V V
- Double-check that all previous passes haven't broken any of the type system's rules
Lower MIR: V V V
- Convert HIR expressions into a control-flow graph (MIR)
MIR Validate: V V V
- Check that the generated MIR is consistent
MIR Cleanup: V V V
- Perform various transformations on the MIR:
  - Replace reads of `const` items with the item itself
  - Convert casts to unsized types into `MakeDst` operations
MIR Optimise: V V V
- Perform various simple optimisations on the MIR (constant propagation, dead code elimination, borrow elimination)
- Does some inlining too
MIR Validate PO: V V V
- Re-validate the MIR
MIR Validate Full: V V V
- Optionally: Perform expensive state-tracking validation on the MIR
Trans Enumerate: V V V
- Enumerate all items that are needed for code generation (primarily, which types are used for generics)
Trans Auto Impls: V V V
- Create magic trait impls as enumerated in the previous pass
Trans Monomorph: V V V
- Generate monomorphised copies of all functions (with generics replaced with the real types)
MIR Optimise Inline: V V V
- Run optimisation again, this time with full type infomration (primiarly for inlining)
HIR Serialise: V V V
- Write out the HIR dump (module tree and generic/inline MIR)
Trans Codegen: V V V
- Generate final output file (typically emitting a C source file then calling the C compiler)
