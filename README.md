Mutabah's Rust Compiler

_In-progress_ alternative rust compiler. Not yet suitable for everyday use.

Intro
===
This project is an attempt at creating a simple rust compiler in C++, with the ultimate goal of being a separate re-implementation.

The short-term goal is to compile pre-borrowchecked rust code into some intermediate form (e.g. LLVM IR, x86-64 assembly, or C code). Thankfully, (from what I have seen), the borrow checker is not needed to compile rust code (just to ensure that it's valid)

Getting Started
===============

Linux
-----

- `make RUSTCSRC` - Downloads the rustc source tarball
- `make -f minicargo.mk` - Builds `mrustc` and `minicargo`, then builds `libstd`, `libtest`, finally `rustc`
- `make -C build_rustc` - Build libstd and a "hello, world" using the above-built rustc

Windows
--------
(NOTE: Incomplete, doesn't yet compile executables and missing helper scripts)
- Download and extract `rustc-1.19.0-src.tar.gz` to the repository root (such that the `rustc-1.19.0-src` directory is present)
  - NOTE: I am open to suggestions for how to automate that step
- Open `vsproject/mrustc.sln` and build minicargo

Building Requirements
=====================
- C++14-compatible compiler (tested with gcc 5.4 and gcc 6)
- C11 compatible C compiler (for output, see above)
- `curl` (for downloading the rust source, linux only)
- `cmake` (at least 3.4.3, required for building llvm in rustc)

Current Features
===
- Full compilation chain including HIR and MIR stages (outputting to C)
- Supports just x86-64 linux
- MIR optimisations
- Optionally-enablable exhaustive MIR validation (set the `MRUSTC_FULL_VALIDATE` environment variable)
- Functional cargo clone (minicargo)

Short-Term Plans
===
- Fix currently-failing tests (mostly in type inferrence)
- Fix all known TODOs in MIR generation (still some possible leaks)
- Perform a clean rustc bootstrap (using a mrustc-built compiler as stage0)

Medium-Term Goals
===
- Propagate lifetime annotations so that MIR can include a borrow checker


Progress
===
- Compiles static libraries into loadable HIR tree and MIR code
- Supports custom derive (aka macros 1.1)
- Compiles `rustc` that can compile the standard library and "hello, world"
- Compiles a running `cargo`

Note: All progress is against the source of rustc 1.19.0

