# Makefile that builds libstd using the mrustc-built rustc
# 
#
# Inputs: mrustc-built rustc (`rustc_m`) and cargo
# - Step 1: Build libstd with `minicargo` (so there's a rustc-compatible libstd around)
#  > This is stored in `$(PREFIX_S)`
# - Step 2: Build libstd again (this time using `cargo`)
#  > This is stored in `$(PREFIX_2)` AND `$(PREFIX)`
# - Step 3: Build a `rustc` using that libstd

PARLEVEL ?= 1
RUSTC_VERSION_DEF := $(shell cat ../rust-version)

RUSTC_VERSION ?= $(RUSTC_VERSION_DEF)
RUST_SRC ?= ../rustc-$(RUSTC_VERSION)-src/src/
ifeq ($(RUSTC_VERSION),$(RUSTC_VERSION_DEF))
  OUTDIR_SUF ?=
else
  OUTDIR_SUF ?= -$(RUSTC_VERSION)
endif
# Extension for dynamic libraries
ifeq ($(shell uname -s || echo not),Darwin)
  DYLIB_EXT := dylib
else
  DYLIB_EXT := so
endif

RUSTC_TARGET ?= x86_64-unknown-linux-gnu

TARGETVER_LEAST_1_39 := $(shell printf "1.39.0\n$(RUSTC_VERSION)" | sort | tail -n 1 | grep $(RUSTC_VERSION))

ifneq ($(TARGETVER_LEAST_1_39),)
 RUST_SRC_HELLO = $(RUST_SRC)test/ui/hello.rs
 VENDOR_DIR := $(RUST_SRC)../vendor
else
 RUST_SRC_HELLO = $(RUST_SRC)test/run-pass/hello.rs
 VENDOR_DIR := $(RUST_SRC)vendor
endif


OUTDIR := output$(OUTDIR_SUF)/
# Stage 1: standard library built with `rustc_m` (using minicargo)
PREFIX_S := $(OUTDIR)prefix-s/
LIBDIR_S := $(PREFIX_S)lib/rustlib/$(RUSTC_TARGET)/lib/
BINDIR_S := $(PREFIX_S)bin/
# Stage 2: standard library built with `rustc_m` (using cargo)
PREFIX_2 := $(OUTDIR)prefix-2/
LIBDIR_2 := $(PREFIX_2)lib/rustlib/$(RUSTC_TARGET)/lib/
BINDIR_2 := $(PREFIX_2)bin/
# Stage X: clean rustc and stage 2 libstd
PREFIX := $(OUTDIR)prefix/
BINDIR := $(PREFIX)bin/
LIBDIR := $(PREFIX)lib/rustlib/$(RUSTC_TARGET)/lib/
CARGO_HOME := $(PREFIX)cargo_home/

LLVM_CONFIG := $(RUST_SRC)../build/bin/llvm-config
LLVM_TARGETS ?= X86;ARM;AArch64#;Mips;PowerPC;SystemZ;JSBackend;MSP430;Sparc;NVPTX

RUSTC_ENV_VARS := CFG_COMPILER_HOST_TRIPLE=$(RUSTC_TARGET)
RUSTC_ENV_VARS += LLVM_CONFIG=$(abspath $(LLVM_CONFIG))
RUSTC_ENV_VARS += CFG_RELEASE=$(RUSTC_VERSION)	# Claiming stable
RUSTC_ENV_VARS += CFG_RELEASE_CHANNEL=$(RUSTC_CHANNEL)
RUSTC_ENV_VARS += CFG_VERSION=$(RUSTC_VERSION)-stable-mrustc
RUSTC_ENV_VARS += CFG_PREFIX=mrustc
RUSTC_ENV_VARS += CFG_LIBDIR_RELATIVE=lib
RUSTC_ENV_VARS += LD_LIBRARY_PATH=$(abspath $(LIBDIR))
RUSTC_ENV_VARS += REAL_LIBRARY_PATH_VAR=LD_LIBRARY_PATH
RUSTC_ENV_VARS += RUSTC_INSTALL_BINDIR=bin
RUSTC_ENV_VARS += RUSTC_ERROR_METADATA_DST=$(abspath $(PREFIX))

CARGO_ENV := CFG_COMPILER_HOST_TRIPLE=$(RUSTC_TARGET)
CARGO_ENV += RUSTC_BOOTSTRAP=1
CARGO_ENV += CARGO_HOME=$(CARGO_HOME)
CARGO_ENV += RUSTFLAGS="-Z force-unstable-if-unmarked -C link_args=-Wl,-rpath,\$$ORIGIN/../lib"

fn_opt_copy = (test -e "$2" && test ! "$1" -nt "$2") || cp "$1" "$2"

V ?= @


all: $(BINDIR_S)hello_world
all: $(BINDIR)hello_world	# Implies $(LIBDIR)libstd.rlib
all: $(BINDIR)rustc
all: $(BINDIR)cargo

../output$(OUTDIR_SUF)/rustc:
	$(MAKE) -C ../ output$(OUTDIR_SUF)/rustc -j 3
../output$(OUTDIR_SUF)/cargo:
	$(MAKE) -C ../ output$(OUTDIR_SUF)/cargo -j 3

# Stage 0 setup
$(BINDIR)rustc_m: ../output$(OUTDIR_SUF)/rustc
	@mkdir -p $(dir $@)
	cp $< $@
$(BINDIR_S)rustc: ../output$(OUTDIR_SUF)/rustc
	@mkdir -p $(dir $@)
	cp $< $@
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

# -------------------------------------
# Stage 1: Build standard library (using minicargo with rustc)
# - Can't use `cargo` because there isn't a rustc-compatible std yet
# - _could_ use it if mrustc accepted all the arguments rustc does (via the proxy)
# -------------------------------------
$(LIBDIR_S)libstd.rlib: $(BINDIR_S)rustc ../bin/minicargo Makefile
	@echo "[MINICARGO] $(RUST_SRC)libstd > $(LIBDIR_S)"
	mkdir -p $(LIBDIR_S)
	$V+MRUSTC_PATH=$(abspath $(BINDIR_S)rustc) ../bin/minicargo --vendor-dir $(VENDOR_DIR) --script-overrides ../script-overrides/stable-$(RUSTC_VERSION)-linux/ --output-dir $(LIBDIR_S) $(RUST_SRC)libstd
	$V+MRUSTC_PATH=$(abspath $(BINDIR_S)rustc) ../bin/minicargo --vendor-dir $(VENDOR_DIR) --script-overrides ../script-overrides/stable-$(RUSTC_VERSION)-linux/ --output-dir $(LIBDIR_S) $(RUST_SRC)libpanic_unwind
	$V+MRUSTC_PATH=$(abspath $(BINDIR_S)rustc) ../bin/minicargo --vendor-dir $(VENDOR_DIR) --script-overrides ../script-overrides/stable-$(RUSTC_VERSION)-linux/ --output-dir $(LIBDIR_S) $(RUST_SRC)libtest
#	$V+MINICARGO_DYLIB=1 MRUSTC_PATH=$(abspath $(BINDIR_S)rustc) ../bin/minicargo --vendor-dir $(VENDOR_DIR) --script-overrides ../script-overrides/stable-$(RUSTC_VERSION)-linux/ --output-dir $(LIBDIR_S) $(RUST_SRC)libstd

# Build hello_world using the bootstrapping compiler (mrustc-built rustc)
$(BINDIR_S)hello_world: $(RUST_SRC_HELLO) $(LIBDIR_S)libstd.rlib $(BINDIR_S)rustc
	@mkdir -p $(dir $@)
	$V$(BINDIR_S)rustc $(RUSTFLAGS_$@) -L $(LIBDIR_S) $< -o $@

# ---
# Stage 2: Build a proper libstd (which includes dynamc libraries too)
# ---
CARGO_OUTDIR_STAGE2_STD := $(OUTDIR)build-std2/$(RUSTC_TARGET)/release/
CARGO_ENV_STAGE2_STD := CARGO_TARGET_DIR=$(OUTDIR)build-std2 RUSTC=$(abspath rustc_proxy.sh) PROXY_RUSTC=$(abspath $(BINDIR_2)rustc) PROXY_MRUSTC=$(abspath $(BINDIR_S)rustc) $(CARGO_ENV)
$(LIBDIR_2)libtest.rlib: $(LIBDIR_S)libstd.rlib $(BINDIR_2)rustc $(BINDIR_S)rustc $(CARGO_HOME)config Makefile
	@mkdir -p $(LIBDIR_2)
	@echo [CARGO] $(RUST_SRC)libtest/Cargo.toml '>' $(OUTDIR)build-std2
ifeq ($(TARGETVER_LEAST_1_39),)
	$V$(CARGO_ENV_STAGE2_STD) $(BINDIR)cargo build --target $(RUSTC_TARGET) -j $(PARLEVEL) --release --manifest-path $(RUST_SRC)libstd/Cargo.toml --features panic-unwind
	$Vcp --remove-destination $(CARGO_OUTDIR_STAGE2_STD)deps/*.rlib $(CARGO_OUTDIR_STAGE2_STD)deps/*.$(DYLIB_EXT) $(LIBDIR_2)
#	$V$(CARGO_ENV_STAGE2_STD) $(BINDIR)cargo build --target $(RUSTC_TARGET) -j $(PARLEVEL) --release --manifest-path $(RUST_SRC)libpanic_unwind/Cargo.toml
#	$Vcp $(CARGO_OUTDIR_STAGE2_STD)deps/*.rlib $(LIBDIR_2)
	$V$(CARGO_ENV_STAGE2_STD) $(BINDIR)cargo build --target $(RUSTC_TARGET) -j $(PARLEVEL) --release --manifest-path $(RUST_SRC)libtest/Cargo.toml
else
	$V$(CARGO_ENV_STAGE2_STD) $(BINDIR)cargo build --target $(RUSTC_TARGET) -j $(PARLEVEL) --release --manifest-path $(RUST_SRC)libtest/Cargo.toml --features panic-unwind
endif
	$Vcp $(CARGO_OUTDIR_STAGE2_STD)deps/*.rlib $(CARGO_OUTDIR_STAGE2_STD)deps/*.$(DYLIB_EXT) $(LIBDIR_2)

$(BINDIR_2)rustc: ../output$(OUTDIR_SUF)/rustc
	@mkdir -p $(dir $@)
	cp $< $@

# ---
# Stage 3: Build rustc using the above (full) libstd
# ---

# - Build rustc with itself (so we have a rustc with the right ABI)
$(BINDIR)rustc: $(BINDIR_2)rustc $(BINDIR)cargo $(CARGO_HOME)config $(LIBDIR_2)libtest.rlib
	@mkdir -p $(PREFIX)tmp
	@echo [CARGO] $(RUST_SRC)rustc/Cargo.toml '>' $(OUTDIR)build-rustc/
	$V$(RUSTC_ENV_VARS) TMPDIR=$(abspath $(PREFIX)tmp) CARGO_TARGET_DIR=$(OUTDIR)build-rustc RUSTC=$(BINDIR_2)rustc $(CARGO_ENV) $(BINDIR)cargo build --manifest-path $(RUST_SRC)rustc/Cargo.toml --release -j $(PARLEVEL)
ifeq ($(RUSTC_VERSION),1.39.0)
	@echo [CARGO] $(RUST_SRC)librustc_codegen_llvm/Cargo.toml '>' $(OUTDIR)build-rustc/
	$V$(RUSTC_ENV_VARS) TMPDIR=$(abspath $(PREFIX)tmp) CARGO_TARGET_DIR=$(OUTDIR)build-rustc RUSTC=$(BINDIR_2)rustc $(CARGO_ENV) $(BINDIR)cargo rustc --manifest-path $(RUST_SRC)librustc_codegen_llvm/Cargo.toml --release -j $(PARLEVEL) -- -L $(abspath $(OUTDIR)build-rustc/release/deps)
	@mkdir -p $(LIBDIR)../codegen-backends
	@echo "[CP] libraries and results"
	$V$(call fn_opt_copy,$(OUTDIR)build-rustc/release/librustc_codegen_llvm.$(DYLIB_EXT),$(LIBDIR)../codegen-backends/librustc_codegen_llvm-llvm.$(DYLIB_EXT))
else
	@echo "[CP] libraries and results"
endif
	@mkdir -p $(LIBDIR)
	$Vcp $(LIBDIR_2)*.$(DYLIB_EXT) $(OUTDIR)prefix/lib/
	$Vcp $(LIBDIR_2)*.$(DYLIB_EXT) $(LIBDIR_2)*.rlib $(LIBDIR)
	$Vcp $(OUTDIR)build-rustc/release/deps/*.rlib $(LIBDIR)
	$Vcp $(OUTDIR)build-rustc/release/deps/*.$(DYLIB_EXT) $(LIBDIR)
ifeq ($(RUSTC_VERSION),1.19.0)
	$Vcp $(OUTDIR)build-rustc/release/rustc $(BINDIR)rustc_binary
else
	$V$(call fn_opt_copy,$(OUTDIR)build-rustc/release/rustc_binary,$(BINDIR)rustc_binary)
endif
	$Vprintf '#!/bin/sh\nd=$$(dirname $$0)\nLD_LIBRARY_PATH="$(abspath $(OUTDIR)prefix/lib):$(abspath $(LIBDIR))" $$d/rustc_binary $$@' >$@
	$Vchmod +x $@

# Build hello_world using the final fully-bootstrapped compiler
$(BINDIR)hello_world: $(RUST_SRC_HELLO) $(BINDIR)rustc
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$V$(DBG) $(BINDIR)rustc -L $(LIBDIR) $< -o $@
	./$@

