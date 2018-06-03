
Running mrustc/minicargo
========================

The easisest way to compile code with mrustc is to use `minicargo` to build a project using a Cargo.toml file, but if
you need extra control over the output, `mrustc` can be called directly.

minicargo
=========

```
minicargo mycrate/ -L ../libstd_crates --vendor-dir vendored/
```

The above builds the crate specified by `mycrate/Cargo.toml`, looking for compiled versions of the standard library crates in `../libstd_crates` and pre-downloaded crates.io packages in `vendored/`. The compiled crates (both `mycrate` and any of its dependencies) will be placed in the default location ( `output/`)

Options
-------

- `--help, -h`
  - Show basic help
- `--script-overrides <dir>`
  - Folder containing `<cratename>.txt` files that are used as if they were the output from that crate's build script (used for building libstd)
- `--vendor-dir <dir>`
  - Directory containing extracted crates.io packages required to build the current crate (see [https://crates.io/crates/cargo-vendor](cargo-vendor))
- `--output-dir,-o <dir>`
  - Specifies the output directory, used for both dependencies and the final binary.
- `--target <name>`
  - Cross-compile for the specified target
- `-L <dir>`
  - Add a directory to the crate/library search path
- `-j <num>`
  - Run a specified number of build jobs at once
- `-n`
  - Do a dry run (print the crates to be compiled, but don't build any of them)
- `-Z <option>`
  - Debugging/experiemental options (see below)

Debugging options
- `-Z emit-mmir`
  - Use the `mmir` mrustc backend (for use with the "Stanalone MIRI" tool)


mrustc
======
`mrustc`'s command-line interface is very similar to rustc's, taking a path to the crate root, an output directory/file, and optional library search paths and crate paths.

```
mrustc mycrate/main.rs -L ../libstd_crates --crate mylib=mylib.hir -o mycrate.exe
```

The above builds the binary crate rooted at `mycrate/main.rs` and outputs it to `mycrate.exe`. It looks for external
crates in `../libstd_crates`, but also is told that `extern crate mylib;` should load `mylib.hir`.

Options
-------

- `-L <dir>`
  - Search for crate files (`.hir` files) in this directory
- `-o <filename>`
  - Write compiler output (library or executable) to this file
- `-O`
  - Enable optimistion
- `-g`
  - Emit debugging information
- `--out-dir <dir>`
  - Specify the output directory (alternative to `-o`)
- `--extern <crate>=<path>`
  - Specify the path for a given crate (instead of searching for it)
- `--crate-tag <str>`
  - Specify a suffix for symbols and output files
- `--crate-name <str>`
  - Override/set the crate name
- `--crate-type <ty>`
  - Override/set the crate type (rlib, bin, proc-macro)
- `--cfg flag`
  - Set a boolean `#[cfg]`/`cfg!` flag
- `--cfg flag=\"val\"`
  - Set a string `#[cfg]`/`cfg!` flag (Note: This can be repeated with the same name, adding extra options each time. You may need to escape the quotes in your shell
- `--target <name>`
  - Compile code for the given target (if the name has a slash in it, it's treated as the path to a target file)
- `--test`
  - Generate a unit test executable
- `-C <option>`
  - Code-generation options (see below)
- `-Z <option>`
  - Debugging/experiemental options (see below)

Codegen options
- `-C emit-build-command=<filename>`
  - Write the command that would be used to invoke the C compiler to the specified file
- `-C codegen-type=<type>`
  - Switch codegen backends. Valid options are: `c` (The normal C backend), `mmir` (Monomorphised MIR, used for `standalone_miri`)
- `-C emit-depfile=<filename>`
  - Write out a makefile-style dependency file for the crate

Debugging Options
- `-Z disable-mir-opt`
  - Disable MIR optimisations (while still enabling optimisation in the backend)
- `-Z full-validate`
  - Perform expensive MIR validation before translation (can spot codegen bugs, but is VERY slow)
- `-Z full-validate-early`
  - Perform expensive MIR validation before optimisation (even slower)
- `-Z dump-ast`
  - Dump the AST after expansion and name resolution complete
- `-Z dump-hir`
  - Dump the HIR (simplified and resolved AST) at various stages in compilation
- `-Z dump-mir`
  - Dump the MIR for all functions at various stages in compilation
- `-Z stop-after=<stage>`
  - Stop compilation after the specified stage. Valid options are `parse`, `expand`, `resolve`, `typeck`, and `mir`


