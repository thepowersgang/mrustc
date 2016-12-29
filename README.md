Mutabah's Rust Compiler

_In-progress_ alternative rust compiler.

Intro
===
This project is an attempt at creating a simple rust compiler in C++, with the ultimate goal of being a separate re-implementation.

The short-term goal is to compile pre-borrowchecked rust code into some intermediate form (e.g. LLVM IR, x86-64 assembly, or C code). Thankfully, (from what I have seen), the borrow checker is not needed to compile rust code (just to ensure that it's valid)

Current Features
===
- Attribute and macro expansion
- Resolves all paths to absolute forms
- Converts name-resolved AST into a more compact "HIR" (simplified module and expression tree)
- Hackily evaluates constants
 - Constant evaluation is done by using duck-typing, which is then validated by the Type Check pass
 - This is how rustc did (or still does?) const eval before MIR
- Type inference and checking
- Closure and operator desugaring
- MIR generation (with partial validation pass)
- HIR/MIR (de)serialisation, allowing for `extern crate` handling
- C-based code generation

Short-Term Plans
===
- Parse and Typecheck all run-pass tests

Medium-Term Goals
===
- Compile rustc
- Extensive MIR optimisations
- Propagate lifetime annotations so that MIR can include a borrow checker


Progress
===
- Compiles the standard library into loadable MIR
- Compiles the "hello, world" test into compilable and running C code
- Compiles `rustc` through to failing codegen

