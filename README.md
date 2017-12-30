Mutabah's Rust Compiler

_In-progress_ alternative rust compiler. Capable of building a fully-working copy of rustc, but not yet suitable for everyday use.

Intro
===
This project is an attempt at creating a simple rust compiler in C++, with the ultimate goal of being a separate re-implementation.

`mrustc` works by comping assumed-valid rust code (i.e. without borrow checking) into a high-level assembly (currently using C, but LLVM/cretonne or even direct machine code could work) and getting an external code generator to turn that into optimised machine code. This works because the borrow checker doesn't have any impact on the generated code, just in checking that the code would be valid.

Progress
--------
- Supported Targets:
  - x86-64 linux
  - (incomplete) x86 windows
  - (incomplete) x86-64 windows
- Builds working copies of `rustc` and `cargo` from a release source tarball
- `rustc` bootstrap tested and validated
  - See the script `TestRustcBootstrap.sh` for how this was done.

Getting Started
===============

Dependencies
------------
- C++14-compatible compiler (tested with gcc 5.4 and gcc 6)
- C11 compatible C compiler (for output, see above)
- `make` (for the mrustc makefiles)
- `patch` (For doing minor edits to the rustc source)
- `libz-dev` (used to reduce size of bytecode files, linux only - windows uses vcpkg to download it)
- `curl` (for downloading the rust source, linux only)
- `cmake` (at least 3.4.3, required for building llvm in rustc)

Linux
-----
- `make RUSTCSRC` - Downloads the rustc source tarball (1.19.0 by default)
- `make -f minicargo.mk` - Builds `mrustc` and `minicargo`, then builds `libstd`, `libtest`, finally `rustc` and `cargo`
- `make -C build_rustc` - Build libstd and a "hello, world" using the above-built rustc

Windows
--------
(NOTE: Incomplete, doesn't yet compile executables and missing helper scripts)
- Download and extract `rustc-1.19.0-src.tar.gz` to the repository root (such that the `rustc-1.19.0-src` directory is present)
  - NOTE: I am open to suggestions for how to automate that step
- Open `vsproject/mrustc.sln` and build minicargo


Diagnosing Issues and Reporting Bugs
====================================

Debugging
---------
Both the makefiles and `minicargo` write the compiler's stdout to a file in the output directory, e.g. when building
`output/libcore.hir` it'll save to `output/libcore.hir_dbg.txt`.
To get full debug output for a compilation run, set the environemnt variable `MRUSTC_DEBUG` to the pass you want to debug
(pass names are printed in every log line). E.g. `MRUSTC_DEBUG=Expand make -f minicargo.mk`

Bug Reports
-----------
Please try to include the following when submitting a bug report:
- What you're trying to build
- Your host system version (e.g. Ubuntu 17.10)
- C/C++ compiler version
- Revison of the mrustc repo that you're running


Current Features
================
- Full compilation chain including HIR and MIR stages (outputting to C)
- MIR optimisations (to take some load off the C compiler)
- Optionally-enablable exhaustive MIR validation (set the `MRUSTC_FULL_VALIDATE` environment variable)
- Functional cargo clone (minicargo)
  - Includes build script support
- Procedural macros (custom derive)

Plans
=====

Short-term
----------
- Fix currently-failing tests (mostly in type inferrence)
- Fix all known TODOs in MIR generation (still some possible leaks)

Medium-term
-----------
- Propagate lifetime annotations so that MIR can include a borrow checker
- Emit C code that is (more) human readable (uses names from the orignal source, reduced/no gotos)
- Add alternate backends (e.g. LLVM IR, cretonne, ...)



Note: All progress is against the source of rustc 1.19.0

