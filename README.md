# Mutabah's Rust Compiler

_In-progress_ alternative rust compiler. Capable of building a fully-working copy of rustc, but not yet suitable for everyday use.

[![Build Status: windows](https://ci.appveyor.com/api/projects/status/96y4ui20pl8xjm2h/branch/master?svg=true)](https://ci.appveyor.com/project/thepowersgang/mrustc/branch/master)
[![Build Status: Linux/OSX](https://travis-ci.org/thepowersgang/mrustc.svg?branch=master)](https://travis-ci.org/thepowersgang/mrustc)

Intro
===
This project is an attempt at creating a simple rust compiler in C++, with the ultimate goal of being a separate re-implementation.

`mrustc` works by compiling assumed-valid rust code (i.e. without borrow checking) into a high-level assembly (currently using C, but LLVM/cretonne or even direct machine code could work) and getting an external code generator to turn that into optimised machine code. This works because the borrow checker doesn't have any impact on the generated code, just in checking that the code would be valid.

Progress
--------
- Supported Targets:
  - x86-64 linux
  - (incomplete) x86 windows
  - (incomplete) x86-64 windows
- Builds working copies of `rustc` and `cargo` from a release source tarball
  - Supports both rustc 1.19.0 and 1.29.0
- `rustc` bootstrap tested and validated (1.19.0 validated once, 1.29.0 is repeatable)
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
- `make RUSTCSRC` - Downloads the rustc source tarball (1.29.0 by default)
- `make -f minicargo.mk` - Builds `mrustc` and `minicargo`, then builds `libstd`, `libtest`, finally `rustc` and `cargo`
- `make -C run_rustc` - Build `libstd` and a "hello, world" using the above-built rustc

BSD
---
Similar to Linux, but you might need to
- specify the rustc default target explicitly
- specify the compiler
- use `gmake` to run GNU make

e.g. `gmake CC=cc RUSTC_TARGET=x86_64-unknown-freebsd -f minicargo.mk`

Windows
--------
(NOTE: Incomplete, doesn't yet compile executables and missing helper scripts)
- Download and extract `rustc-1.29.0-src.tar.gz` to the repository root (such that the `rustc-1.29.0-src` directory is present)
  - NOTE: I am open to suggestions for how to automate that step
- Open `vsproject/mrustc.sln` and build minicargo
- Run `vsproject/build_rustc_minicargo.cmd` to attempt to build libstd


Building non-rustc code
=======================

To build your own code with mrustc, first you need to build at least libcore (and probably the full standard library).
This can be done on linux by running `make -f minicargo.mk LIBS`, or on windows with `build_std.cmd`.

Next, run
- `minicargo -L <path_to_libstd> <crate_path>` to build a cargo project.
- or, `mrustc -L <path_to_libstd> --out-dir <output_directory> <path_to_main.rs>` to directly invoke mrustc.

For additional options, both programs have a `--help` option.

Diagnosing Issues and Reporting Bugs
====================================

Debugging
---------
Both the makefiles and `minicargo` write the compiler's stdout to a file in the output directory, e.g. when building
`output/libcore.hir` it'll save to `output/libcore.hir_dbg.txt`.
To get full debug output for a compilation run, set the environemnt variable `MRUSTC_DEBUG` to a : separated list of the passes you want to debug
(pass names are printed in every log line). E.g. `MRUSTC_DEBUG=Expand:Parse make -f minicargo.mk`

Bug Reports
-----------
Please try to include the following when submitting a bug report:
- What you're trying to build
- Your host system version (e.g. Ubuntu 17.10)
- C/C++ compiler version
- Revison of the mrustc repo that you're running

Support and Discussion
----------------------
For problems that don't warrant opening an issue, join the IRC channel - `irc.freenode.net#mrustc`


Current Features
================
- Full compilation chain including HIR and MIR stages (outputting to C)
- MIR optimisations (to take some load off the C compiler)
- Optionally-enablable exhaustive MIR validation (set the `MRUSTC_FULL_VALIDATE` environment variable)
- Functional cargo clone (minicargo)
  - Includes build script support
- Procedural macros (custom derive)
- Custom target specifications
  - See `docs/target.md`

Plans
=====

Short-term
----------
- Fix currently-failing tests (mostly in type inference)
- Fix all known TODOs in MIR generation (still some possible leaks)

Medium-term
-----------
- Propagate lifetime annotations so that MIR can include a borrow checker
- Emit C code that is (more) human readable (uses names from the orignal source, reduced/no gotos)
- Add alternate backends (e.g. LLVM IR, cretonne, ...)



Note: All progress is against the source of rustc 1.19.0 AND rustc 1.29.0

