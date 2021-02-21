# Makefile that builds libstd using the mrustc-built rustc

RUSTC_VERSION_DEF = $(shell cat ../rust-version)
RUSTC_VERSION ?= $(RUSTC_VERSION_DEF)
RUST_SRC ?= ../rustc-$(RUSTC_VERSION)-src/src/
ifeq ($(RUSTC_VERSION),$(RUSTC_VERSION_DEF))
OUTDIR_SUF ?=
else
OUTDIR_SUF ?= -$(RUSTC_VERSION)
endif

RUSTC_TARGET := x86_64-unknown-linux-gnu

OUTDIR := output$(OUTDIR_SUF)/
PREFIX := $(OUTDIR)prefix/
BINDIR := $(PREFIX)bin/
LIBDIR := $(PREFIX)lib/rustlib/$(RUSTC_TARGET)/lib/
CARGO_HOME := $(PREFIX)cargo_home/
PREFIX_S := $(OUTDIR)prefix-s/
LIBDIR_S := $(PREFIX_S)lib/rustlib/$(RUSTC_TARGET)/lib/
BINDIR_S := $(PREFIX_S)bin/

LLVM_CONFIG := $(RUST_SRC)../build/bin/llvm-config
LLVM_TARGETS ?= X86;ARM;AArch64#;Mips;PowerPC;SystemZ;JSBackend;MSP430;Sparc;NVPTX

RUSTC_ENV_VARS := CFG_COMPILER_HOST_TRIPLE=$(RUSTC_TARGET)
RUSTC_ENV_VARS += LLVM_CONFIG=$(abspath $(LLVM_CONFIG))
RUSTC_ENV_VARS += CFG_RELEASE=
RUSTC_ENV_VARS += CFG_RELEASE_CHANNEL=$(RUSTC_CHANNEL)
RUSTC_ENV_VARS += CFG_VERSION=$(RUSTC_VERSION)-stable-mrustc
RUSTC_ENV_VARS += CFG_PREFIX=mrustc
RUSTC_ENV_VARS += CFG_LIBDIR_RELATIVE=lib
RUSTC_ENV_VARS += LD_LIBRARY_PATH=$(abspath output)

CARGO_ENV := CFG_COMPILER_HOST_TRIPLE=$(RUSTC_TARGET)
CARGO_ENV += RUSTC_BOOTSTRAP=1
CARGO_ENV += CARGO_HOME=$(CARGO_HOME)
CARGO_ENV += RUSTFLAGS="-Z force-unstable-if-unmarked"

V ?= @


all: $(BINDIR)rustc $(LIBDIR)libstd.rlib $(BINDIR)hello_world
all: $(BINDIR)cargo

RUSTFLAGS_core := 
RUSTFLAGS_libc := --cfg stdbuild
RUSTFLAGS_std := -l dl -l rt -l pthread
RUSTFLAGS_unwind := -l gcc_s
RUSTFLAGS_alloc := 
RUSTFLAGS_alloc_system := 
RUSTFLAGS_compiler_builtins := --cfg feature=\"compiler-builtins\"

../output$(OUTDIR_SUF)/rustc:
	$(MAKE) -C ../ output$(OUTDIR_SUF)/rustc -j 3
../output$(OUTDIR_SUF)/cargo:
	$(MAKE) -C ../ output$(OUTDIR_SUF)/cargo -j 3

$(BINDIR)rustc_m: ../output$(OUTDIR_SUF)/rustc
	@mkdir -p $(dir $@)
	$Vcp $< $@
$(BINDIR_S)rustc: ../output$(OUTDIR_SUF)/rustc
	@mkdir -p $(dir $@)
	$Vcp $< $@
$(BINDIR)cargo: ../output$(OUTDIR_SUF)/cargo
	@mkdir -p $(dir $@)
	$Vcp $< $@

$(CARGO_HOME)config: Makefile
	@mkdir -p $(dir $@)
	@echo "[create] $@"
	$Vecho "[source.crates-io]" > $@
	$Vecho "replace-with = \"vendored-sources\"" >> $@
	$Vecho "[source.vendored-sources]" >> $@
	$Vecho "directory = \"$(abspath $(RUST_SRC)vendor)\"" >> $@

# Actual libstd build (using cargo, and using manually-build libstd as deps)
$(LIBDIR)libstd.rlib: $(BINDIR_S)rustc $(BINDIR)cargo $(LIBDIR_S)libstd.rlib $(CARGO_HOME)config
	@mkdir -p $(OUTDIR)build-std
	@mkdir -p $(LIBDIR)
	@echo [CARGO] $(RUST_SRC)libstd/Cargo.toml
	$VCARGO_TARGET_DIR=$(OUTDIR)build-std RUSTC=$(BINDIR_S)rustc $(CARGO_ENV) $(BINDIR)cargo build --manifest-path $(RUST_SRC)libstd/Cargo.toml  -j 1 --release --features panic-unwind
	$Vcp --remove-destination $(OUTDIR)build-std/release/deps/*.rlib $(LIBDIR)
	$Vcp --remove-destination $(OUTDIR)build-std/release/deps/*.so $(LIBDIR)
# libtest
$(LIBDIR)libtest.rlib: $(BINDIR)rustc_m $(LIBDIR)libstd.rlib $(CARGO_HOME)config
	@mkdir -p $(OUTDIR)build-test
	@echo [CARGO] $(RUST_SRC)libtest/Cargo.toml
	$VCARGO_TARGET_DIR=$(OUTDIR)build-test RUSTC=$(BINDIR)rustc_m $(CARGO_ENV) $(BINDIR)cargo build --manifest-path $(RUST_SRC)libtest/Cargo.toml  -j 1 --release
	@mkdir -p $(LIBDIR)
	$Vcp --remove-destination $(OUTDIR)build-test/release/deps/*.rlib $(LIBDIR)
	$Vcp --remove-destination $(OUTDIR)build-test/release/deps/*.so $(LIBDIR)

# - Build rustc with itself (so we have a rustc with the right ABI)
$(BINDIR)rustc: $(BINDIR)rustc_m $(BINDIR)cargo $(CARGO_HOME)config $(LIBDIR)libtest.rlib
	@mkdir -p $(PREFIX)tmp
	@echo [CARGO] $(RUST_SRC)rustc/Cargo.toml
	$V$(RUSTC_ENV_VARS) TMPDIR=$(abspath $(PREFIX)tmp) CARGO_TARGET_DIR=$(OUTDIR)build-rustc RUSTC=$(BINDIR)rustc_m RUSTC_ERROR_METADATA_DST=$(abspath $(PREFIX)) $(CARGO_ENV) $(BINDIR)cargo build --manifest-path $(RUST_SRC)rustc/Cargo.toml --release -j 1
	cp $(OUTDIR)build-rustc/release/deps/*.so $(LIBDIR)
	cp $(OUTDIR)build-rustc/release/deps/*.rlib $(LIBDIR)
ifeq ($(RUSTC_VERSION),1.19.0)
	cp $(OUTDIR)build-rustc/release/rustc $(BINDIR)rustc_binary
else
	cp $(OUTDIR)build-rustc/release/rustc_binary $(BINDIR)rustc_binary
endif
	printf '#!/bin/sh\nd=$$(dirname $$0)\nLD_LIBRARY_PATH="$(abspath $(LIBDIR))" $$d/rustc_binary $$@' >$@
	chmod +x $@

$(BINDIR)hello_world: $(RUST_SRC)test/run-pass/hello.rs $(LIBDIR)libstd.rlib $(BINDIR)rustc
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(BINDIR)rustc $(RUSTFLAGS_$@) -L $(LIBDIR) -L ../output/libs $< -o $@

# 
# - Build libstd in a hacky hard-coded way first, to allow build scripts to work
#  > TODO: Tweak minicargo so it can call rustc (and then use minicargo to do this)
# 
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
ifeq ($(RUSTC_VERSION),1.29.0)
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
$(LIBDIR_S)libunwind.rlib: $(call fcn_extcrate, core libc)
