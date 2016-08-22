
Mutabah's Rust Compiler

_In-progress_ alternative rust compiler.

Intro
===
This project is an attempt at creating a simple rust compiler in C++, with the ultimate goal of being a seperate reimplementation.

The short-term goal is to compile pre-borrowchecked rust code into some intermediate form (e.g. LLVM IR, x86-64 assembly, or C code). Thankfully, (from what I have seen), the borrow checker is not needed to compile rust code (just to ensure that it's valid)

Current Features
===
- Successfully parses libcore and rustc's run-pass tests
- Attribute and macro expansion
- Resolves all paths to absolute forms
- Converts name-resolved AST into a more compact "HIR" (simplified module and expression ree)
- Hackily evaluates constants
 - Constant evaluation is done by using duck-typing, which is then validated by the Type Check pass
 - This is how rustc did (or still does?) const eval before MIR
- Type inferrence and checking
- Closure and operator desugaring
- MIR generation (with partial validation pass)

Short-Term Plans
===
- Storing of HIR/MIR for `extern crate` handling
- Code generation (including picking the output format)

Medium-Term Goals
===
- Propagate lifetime annotations so that MIR can include a borrow checker

