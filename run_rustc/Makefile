# Makefile that builds libstd using the mrustc-built rustc
# 
# Inputs: mrustc-built rustc (`rustc_m`) and cargo
# - Step 1: Build libstd with rustc_m (so there's a rustc-compatible libstd around)
#  > This is stored in `$(PREFIX_S)`
# - Step 2: Build rustc with `rustc_m`
#  > This is stored in `$(PREFIX_2)` AND `$(PREFIX)`, but doesn't yet have a standard library
# - Step 3: Build libstd with this `rustc`

RUSTC_VERSION_DEF = $(shell cat ../rust-version)
RUSTC_VERSION ?= $(RUSTC_VERSION_DEF)
RUST_SRC ?= ../rustc-$(RUSTC_VERSION)-src/src/
ifeq ($(RUSTC_VERSION),$(RUSTC_VERSION_DEF))
OUTDIR_SUF ?=
else
OUTDIR_SUF ?= -$(RUSTC_VERSION)
endif

ifeq ($(RUSTC_VERSION),1.39.0)
 RUST_SRC_HELLO = $(RUST_SRC)test/ui/hello.rs
 VENDOR_DIR := $(RUST_SRC)../vendor
else
 RUST_SRC_HELLO = $(RUST_SRC)test/run-pass/hello.rs
 VENDOR_DIR := $(RUST_SRC)vendor
endif

RUSTC_TARGET := x86_64-unknown-linux-gnu

OUTDIR := output$(OUTDIR_SUF)/
# Stage X: Standard library built using `rustc` (using cargo)
PREFIX := $(OUTDIR)prefix/
BINDIR := $(PREFIX)bin/
LIBDIR := $(PREFIX)lib/rustlib/$(RUSTC_TARGET)/lib/
CARGO_HOME := $(PREFIX)cargo_home/
# Stage 2: standard library built with `rustc` (using minicargo)
PREFIX_2 := $(OUTDIR)prefix-2/
LIBDIR_2 := $(PREFIX_2)lib/rustlib/$(RUSTC_TARGET)/lib/
BINDIR_2 := $(PREFIX_2)bin/
# Stage 1: standard library built with `rustc_m` (using minicargo)
PREFIX_S := $(OUTDIR)prefix-s/
LIBDIR_S := $(PREFIX_S)lib/rustlib/$(RUSTC_TARGET)/lib/
BINDIR_S := $(PREFIX_S)bin/

LLVM_CONFIG := $(RUST_SRC)../build/bin/llvm-config
LLVM_TARGETS ?= X86;ARM;AArch64#;Mips;PowerPC;SystemZ;JSBackend;MSP430;Sparc;NVPTX

RUSTC_ENV_VARS := CFG_COMPILER_HOST_TRIPLE=$(RUSTC_TARGET)
RUSTC_ENV_VARS += LLVM_CONFIG=$(abspath $(LLVM_CONFIG))
RUSTC_ENV_VARS += CFG_RELEASE=$(RUSTC_VERSION)	# Claiming stable
RUSTC_ENV_VARS += CFG_RELEASE_CHANNEL=$(RUSTC_CHANNEL)
RUSTC_ENV_VARS += CFG_VERSION=$(RUSTC_VERSION)-stable-mrustc
RUSTC_ENV_VARS += CFG_PREFIX=mrustc
RUSTC_ENV_VARS += CFG_LIBDIR_RELATIVE=lib
RUSTC_ENV_VARS += LD_LIBRARY_PATH=$(abspath output)
RUSTC_ENV_VARS += REAL_LIBRARY_PATH_VAR=LD_LIBRARY_PATH
RUSTC_ENV_VARS += RUSTC_INSTALL_BINDIR=bin

CARGO_ENV := CFG_COMPILER_HOST_TRIPLE=$(RUSTC_TARGET)
CARGO_ENV += RUSTC_BOOTSTRAP=1
CARGO_ENV += CARGO_HOME=$(CARGO_HOME)
CARGO_ENV += RUSTFLAGS="-Z force-unstable-if-unmarked"

fn_opt_copy = (test -e "$2" && test ! "$1" -nt "$2") || cp "$1" "$2"

V ?= @


all: $(BINDIR_S)hello_world
all: $(BINDIR)hello_world	# Implies $(LIBDIR)libstd.rlib
all: $(BINDIR)rustc
all: $(BINDIR)cargo

RUSTFLAGS_core := 
RUSTFLAGS_libc := --cfg stdbuild
RUSTFLAGS_std := -l dl -l rt -l pthread
RUSTFLAGS_unwind := -l gcc_s
RUSTFLAGS_alloc := 
RUSTFLAGS_alloc_system := 
RUSTFLAGS_compiler_builtins := --cfg feature=\"compiler-builtins\"
ifeq ($(RUSTC_VERSION),1.39.0)
  RUSTFLAGS_core += --edition 2018
endif

../output$(OUTDIR_SUF)/rustc:
	$(MAKE) -C ../ output$(OUTDIR_SUF)/rustc -j 3
../output$(OUTDIR_SUF)/cargo:
	$(MAKE) -C ../ output$(OUTDIR_SUF)/cargo -j 3

$(BINDIR)rustc_m: ../output$(OUTDIR_SUF)/rustc
	@mkdir -p $(dir $@)
	cp $< $@
$(BINDIR_S)rustc: ../output$(OUTDIR_SUF)/rustc
	@mkdir -p $(dir $@)
	cp $< $@
#$(PREFIX_S)lib/rustlib/$(RUSTC_TARGET)/codegen-backends/librustc_codegen_llvm-llvm.so: ../output$(OUTDIR_SUF)/rustc-build/librustc_codegen_llvm.so
#	@mkdir -p $(dir $@)
#	cp $< $@
$(BINDIR)cargo: ../output$(OUTDIR_SUF)/cargo
	@mkdir -p $(dir $@)
	cp $< $@

$(CARGO_HOME)config: Makefile
	@mkdir -p $(dir $@)
	@echo "[create] $@"
	$Vecho "[source.crates-io]" > $@
	$Vecho "replace-with = \"vendored-sources\"" >> $@
	$Vecho "[source.vendored-sources]" >> $@
	$Vecho "directory = \"$(abspath $(VENDOR_DIR))\"" >> $@

# Build libstd and friends with the final rustc, but using minicargo (to avoid running build scripts)
$(LIBDIR_2)libtest.rlib: $(BINDIR)rustc
	@mkdir -p $(LIBDIR_2)
	+$VMRUSTC_PATH=$(abspath $(BINDIR)rustc) ../bin/minicargo $(RUST_SRC)libtest --vendor-dir $(VENDOR_DIR) --script-overrides ../script-overrides/stable-$(RUSTC_VERSION)-linux/ --output-dir $(LIBDIR_2)

$(BINDIR_2)rustc: $(BINDIR)rustc
	mkdir -p $(BINDIR_2) $(LIBDIR_2)../codegen-backends
	cp $(BINDIR)rustc_binary $(BINDIR_2)rustc_binary
	cp $(LIBDIR)../codegen-backends/librustc_codegen_llvm-llvm.so $(LIBDIR_2)../codegen-backends/librustc_codegen_llvm-llvm.so
	printf '#!/bin/sh\nd=$$(dirname $$0)\nLD_LIBRARY_PATH="$(abspath $(LIBDIR))" $$d/rustc_binary $$@' >$@
	chmod +x $@

# Actual libstd build (using cargo, and using the above-built libstd as deps)
$(LIBDIR)libstd.rlib: $(BINDIR_2)rustc $(BINDIR)cargo $(LIBDIR_2)libtest.rlib $(CARGO_HOME)config
	@mkdir -p $(OUTDIR)build-std
	@mkdir -p $(LIBDIR)
ifneq ($(RUSTC_VERSION),1.39.0)
	@echo [CARGO] $(RUST_SRC)libstd/Cargo.toml '>' $(OUTDIR)build-std
	$VCARGO_TARGET_DIR=$(OUTDIR)build-std RUSTC=$(BINDIR_2)rustc $(CARGO_ENV) $(BINDIR)cargo build --manifest-path $(RUST_SRC)libstd/Cargo.toml  -j 1 --release --features panic-unwind -v
endif
	@echo [CARGO] $(RUST_SRC)libtest/Cargo.toml '>' $(OUTDIR)build-std
	$VCARGO_TARGET_DIR=$(OUTDIR)build-std RUSTC=$(BINDIR_2)rustc $(CARGO_ENV) $(BINDIR)cargo build --manifest-path $(RUST_SRC)libtest/Cargo.toml -j 1 --release --features panic-unwind -v
	$Vcp --remove-destination $(OUTDIR)build-std/release/deps/*.rlib $(LIBDIR)
ifeq ($(shell uname -s || echo not),Darwin)
	$Vcp --remove-destination $(OUTDIR)build-std/release/deps/*.dylib $(LIBDIR)
else
	$Vcp --remove-destination $(OUTDIR)build-std/release/deps/*.so $(LIBDIR)
endif
# - Build rustc with itself (so we have a rustc with the right ABI)
$(BINDIR)rustc: $(BINDIR)rustc_m $(BINDIR)cargo $(CARGO_HOME)config $(LIBDIR_S)libstd.rlib
	@mkdir -p $(PREFIX)tmp
	@echo [CARGO] $(RUST_SRC)rustc/Cargo.toml '>' $(OUTDIR)build-rustc
	$V$(RUSTC_ENV_VARS) TMPDIR=$(abspath $(PREFIX)tmp) CARGO_TARGET_DIR=$(OUTDIR)build-rustc RUSTC=$(BINDIR_S)rustc RUSTC_ERROR_METADATA_DST=$(abspath $(PREFIX)) $(CARGO_ENV) $(BINDIR)cargo build --manifest-path $(RUST_SRC)rustc/Cargo.toml --release -j 1
ifeq ($(RUSTC_VERSION),1.39.0)
	@echo [CARGO] $(RUST_SRC)librustc_codegen_llvm/Cargo.toml '>' $(OUTDIR)build-rustc
	$V$(RUSTC_ENV_VARS) TMPDIR=$(abspath $(PREFIX)tmp) CARGO_TARGET_DIR=$(OUTDIR)build-rustc RUSTC=$(BINDIR_S)rustc RUSTC_ERROR_METADATA_DST=$(abspath $(PREFIX)) $(CARGO_ENV) $(BINDIR)cargo rustc --manifest-path $(RUST_SRC)librustc_codegen_llvm/Cargo.toml --release -j 1 -- -L $(abspath $(OUTDIR)build-rustc/release/deps)
	@mkdir -p $(LIBDIR)../codegen-backends
	@echo "[CP] libraries and results"
	$V$(call fn_opt_copy,$(OUTDIR)build-rustc/release/librustc_codegen_llvm.so,$(LIBDIR)../codegen-backends/librustc_codegen_llvm-llvm.so)
endif
ifeq ($(shell uname -s || echo not),Darwin)
	$Vcp $(OUTDIR)build-rustc/release/deps/*.dylib $(LIBDIR)
else
	$Vcp $(OUTDIR)build-rustc/release/deps/*.so $(LIBDIR)
endif
	$Vcp $(OUTDIR)build-rustc/release/deps/*.rlib $(LIBDIR)
ifeq ($(RUSTC_VERSION),1.19.0)
	$Vcp $(OUTDIR)build-rustc/release/rustc $(BINDIR)rustc_binary
else
	$V$(call fn_opt_copy,$(OUTDIR)build-rustc/release/rustc_binary,$(BINDIR)rustc_binary)
endif
	printf '#!/bin/sh\nd=$$(dirname $$0)\nLD_LIBRARY_PATH="$(abspath $(LIBDIR))" $$d/rustc_binary $$@' >$@
	chmod +x $@

# Build hello_world using the final fully-bootstrapped compiler
$(BINDIR)hello_world: $(RUST_SRC_HELLO) $(LIBDIR)libstd.rlib $(BINDIR)rustc
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(BINDIR)rustc $(RUSTFLAGS_$@) -L $(LIBDIR) $< -o $@

# Build hello_world using the bootstrapping compiler (mrustc-built rustc)
$(BINDIR_S)hello_world: $(RUST_SRC_HELLO) $(LIBDIR_S)libstd.rlib $(BINDIR_S)rustc
	@mkdir -p $(dir $@)
	$Vtime $(DBG) $(BINDIR_S)rustc $(RUSTFLAGS_$@) -L $(LIBDIR_S) $< -o $@

# 
# - Build libstd in a hacky hard-coded way first, to allow build scripts to work
#  > TODO: Tweak minicargo so it can call rustc (and then use minicargo to do this)
# 
ifeq ($(RUSTC_VERSION),1.39.0)
$(LIBDIR_S)libstd.rlib: $(BINDIR_S)rustc ../bin/minicargo Makefile
	@echo "[MINICARGO] $(RUST_SRC)libstd > $(LIBDIR_S)"
	mkdir -p $(LIBDIR_S)
	$V+MRUSTC_PATH=$(abspath $(BINDIR_S)rustc) ../bin/minicargo $(RUST_SRC)libstd --vendor-dir $(VENDOR_DIR) --script-overrides ../script-overrides/stable-$(RUSTC_VERSION)-linux/ --output-dir $(LIBDIR_S)
	$V+MRUSTC_PATH=$(abspath $(BINDIR_S)rustc) ../bin/minicargo $(RUST_SRC)libtest --vendor-dir $(VENDOR_DIR) --script-overrides ../script-overrides/stable-$(RUSTC_VERSION)-linux/ --output-dir $(LIBDIR_S)
else
$(LIBDIR_S)lib%.rlib: $(RUST_SRC)lib%/lib.rs $(BINDIR_S)rustc
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(BINDIR_S)rustc --crate-type rlib --crate-name $* -L $(LIBDIR_S) $< -o $@ $(RUSTFLAGS_$*)
$(LIBDIR_S)lib%.rlib: $(RUST_SRC)lib%/src/lib.rs $(BINDIR_S)rustc
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(BINDIR_S)rustc --crate-type rlib --crate-name $* -L $(LIBDIR_S) $< -o $@ $(RUSTFLAGS_$*)

fcn_extcrate = $(patsubst %,$(LIBDIR_S)lib%.rlib,$(1))

$(LIBDIR_S)libarena.rlib: $(call fcn_extcrate, std)
$(LIBDIR_S)liballoc.rlib: $(call fcn_extcrate, core compiler_builtins)
ifneq ($(RUSTC_VERSION),1.29.0)
$(LIBDIR_S)liballoc.rlib: $(call fcn_extcrate, alloc_system)
endif
$(LIBDIR_S)liballoc_system.rlib: $(call fcn_extcrate, core libc)
$(LIBDIR_S)libpanic_unwind.rlib: $(call fcn_extcrate, core alloc libc unwind)
$(LIBDIR_S)libstd_unicode.rlib: $(call fcn_extcrate, core compiler_builtins)
$(LIBDIR_S)libcollections.rlib: $(call fcn_extcrate, core alloc std_unicode compiler_builtins)
$(LIBDIR_S)librand.rlib: $(call fcn_extcrate, core compiler_builtins)
$(LIBDIR_S)liblibc.rlib: $(call fcn_extcrate, core compiler_builtins)
$(LIBDIR_S)libcompiler_builtins.rlib: $(call fcn_extcrate, core)
$(LIBDIR_S)libstd.rlib: $(call fcn_extcrate, core libc unwind compiler_builtins alloc_system panic_unwind)
ifeq ($(RUSTC_VERSION),1.19.0)
$(LIBDIR_S)libstd.rlib: $(call fcn_extcrate, collections rand)
endif
$(LIBDIR_S)libunwind.rlib: $(call fcn_extcrate, core libc compiler_builtins)
endif
