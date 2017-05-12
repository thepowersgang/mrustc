Mutabah's Rust Compiler

_In-progress_ alternative rust compiler.

Intro
===
This project is an attempt at creating a simple rust compiler in C++, with the ultimate goal of being a separate re-implementation.

The short-term goal is to compile pre-borrowchecked rust code into some intermediate form (e.g. LLVM IR, x86-64 assembly, or C code). Thankfully, (from what I have seen), the borrow checker is not needed to compile rust code (just to ensure that it's valid)


Building Requirements
=====================
- C++14-compatible compiler (tested with gcc 5.4 and gcc 6)
- C11 compatible C compiler (for output, see above)
- `curl` (for downloading the rust source)
- `cmake` (at least 3.4.3, required for building llvm in rustc)

Current Features
===
- Full compilation chain including HIR and MIR stages (outputting to C)
- Supports just x86-64 linux
- MIR optimisations
- Optionally-enablable exhaustive MIR validation (set the `MRUSTC_FULL_VALIDATE` environment variable)

Short-Term Plans
===
- Fix currently-failing tests (mostly in type inferrence)
- Fix all known TODOs in MIR generation (still some possible leaks)

Medium-Term Goals
===
- Propagate lifetime annotations so that MIR can include a borrow checker


Progress
===
- Compiles static libraries into loadable HIR tree and MIR code
- Generates working executables (most of the test suite)
- Compiles `rustc` that can compile the standard library and "hello, world"

