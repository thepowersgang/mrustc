# Makefile that builds libstd using the mrustc-built rustc
# 
#
# Inputs: mrustc-built rustc (`rustc_m`) and cargo
# - Step 1: Build libstd with `minicargo`
#  - This is just so cargo can be used to build things (as it needs a working std for build scripts)
#  > This is stored in `$(PREFIX_S)`
# - Step 2: Build libstd again (this time using `cargo`)
#  - Provides full dynamic and static versions of libstd (needed for rustc's use of dynamic linking)
#  > This is stored in `$(PREFIX_2)` (with the shared libraries stored in `prefix/lib`)
# - Step 3: Build a `rustc` using that libstd
#  - Done so there's an optimised rustc around (mrustc is bad at codegen)
# - Step 4: Build `libstd` with this `rustc`
#  - Needed to match ABIs

PARLEVEL ?= 1
RUSTC_VERSION_DEF := $(shell cat ../rust-version)

RUSTC_VERSION ?= $(RUSTC_VERSION_DEF)
RUST_SRC ?= ../rustc-$(RUSTC_VERSION)-src/
ifeq ($(RUSTC_VERSION),$(RUSTC_VERSION_DEF))
  OUTDIR_SUF ?=
else
  OUTDIR_SUF ?= -$(RUSTC_VERSION)
endif
# Extension for dynamic libraries and platform for script overrides
ifeq ($(shell uname -s || echo not),Darwin)
  DYLIB_EXT := dylib
  PLATFORM := macos
  RUSTC_TARGET ?= x86_64-apple-darwin
else
  DYLIB_EXT := so
  PLATFORM := linux
  RUSTC_TARGET ?= x86_64-unknown-linux-gnu
endif

TARGETVER_LEAST_1_29 := $(shell test "$(RUSTC_VERSION)" ">" "1.29" && echo yes)
TARGETVER_LEAST_1_39 := $(shell test "$(RUSTC_VERSION)" ">" "1.39" && echo yes)
TARGETVER_LEAST_1_54 := $(shell test "$(RUSTC_VERSION)" ">" "1.54" && echo yes)

RUSTC_BINNAME := rustc_binary
ifneq ($(TARGETVER_LEAST_1_54),)
 RUST_SRC_HELLO := $(RUST_SRC)src/test/ui/hello.rs
 RUST_SRC_LIBS := $(RUST_SRC)library/
 RUST_SRC_RUSTC := $(RUST_SRC)compiler/rustc/
 RUST_SRC_CODEGEN := $(RUST_SRC)compiler/rustc_codegen_llvm/
 VENDOR_DIR := $(RUST_SRC)vendor
 RUSTC_BINNAME := rustc_main
else ifneq ($(TARGETVER_LEAST_1_39),)
 RUST_SRC_HELLO := $(RUST_SRC)src/test/ui/hello.rs
 RUST_SRC_LIBS := $(RUST_SRC)src/lib
 RUST_SRC_RUSTC := $(RUST_SRC)src/rustc/
 RUST_SRC_CODEGEN := $(RUST_SRC)src/librustc_codegen_llvm/
 VENDOR_DIR := $(RUST_SRC)vendor
else
 RUST_SRC_HELLO := $(RUST_SRC)src/test/run-pass/hello.rs
 RUST_SRC_LIBS := $(RUST_SRC)src/lib
 RUST_SRC_RUSTC := $(RUST_SRC)src/rustc/
 # librustc_codegen_llvm
 VENDOR_DIR := $(RUST_SRC)src/vendor
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

LLVM_CONFIG := $(RUST_SRC)build/bin/llvm-config
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

CARGO_FLAGS := --target $(RUSTC_TARGET) -j $(PARLEVEL) --release --verbose

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

# Helper rules to ensure that mrustc has built rustc and cargo
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
	@echo "[MINICARGO] $(RUST_SRC_LIBS)std > $(LIBDIR_S)"
	mkdir -p $(LIBDIR_S)
	$V+MRUSTC_PATH=$(abspath $(BINDIR_S)rustc) ../bin/minicargo --vendor-dir $(VENDOR_DIR) --script-overrides ../script-overrides/stable-$(RUSTC_VERSION)-$(PLATFORM)/ --output-dir $(LIBDIR_S) $(RUST_SRC_LIBS)std
	$V+MRUSTC_PATH=$(abspath $(BINDIR_S)rustc) ../bin/minicargo --vendor-dir $(VENDOR_DIR) --script-overrides ../script-overrides/stable-$(RUSTC_VERSION)-$(PLATFORM)/ --output-dir $(LIBDIR_S) $(RUST_SRC_LIBS)panic_unwind
	$V+MRUSTC_PATH=$(abspath $(BINDIR_S)rustc) ../bin/minicargo --vendor-dir $(VENDOR_DIR) --script-overrides ../script-overrides/stable-$(RUSTC_VERSION)-$(PLATFORM)/ --output-dir $(LIBDIR_S) $(RUST_SRC_LIBS)test

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
	@echo [CARGO] $(RUST_SRC_LIBS)test/Cargo.toml '>' $(OUTDIR)build-std2
ifeq ($(TARGETVER_LEAST_1_39),)
	$V$(CARGO_ENV_STAGE2_STD) $(BINDIR)cargo build $(CARGO_FLAGS) --manifest-path $(RUST_SRC_LIBS)std/Cargo.toml --features panic-unwind
	$Vcp --remove-destination $(CARGO_OUTDIR_STAGE2_STD)deps/*.rlib $(CARGO_OUTDIR_STAGE2_STD)deps/*.$(DYLIB_EXT) $(LIBDIR_2)
	$V$(CARGO_ENV_STAGE2_STD) $(BINDIR)cargo build $(CARGO_FLAGS) --manifest-path $(RUST_SRC_LIBS)test/Cargo.toml
else
	$V$(CARGO_ENV_STAGE2_STD) $(BINDIR)cargo build $(CARGO_FLAGS) --manifest-path $(RUST_SRC_LIBS)test/Cargo.toml --features panic-unwind
endif
	$Vcp $(CARGO_OUTDIR_STAGE2_STD)deps/*.rlib $(CARGO_OUTDIR_STAGE2_STD)deps/*.$(DYLIB_EXT) $(LIBDIR_2)

$(BINDIR_2)rustc: ../output$(OUTDIR_SUF)/rustc
	@mkdir -p $(dir $@)
	cp $< $@

# ---
# Stage 3: Build rustc using the above (full) std for building other libs
# ---

CARGO_OUTDIR_RUSTC := $(OUTDIR)build-rustc/$(RUSTC_TARGET)/release/
CARGO_ENV_RUSTC := CARGO_TARGET_DIR=$(OUTDIR)build-rustc RUSTC=$(abspath rustc_proxy.sh) PROXY_RUSTC=$(abspath $(BINDIR_2)rustc) PROXY_MRUSTC=$(abspath $(BINDIR_S)rustc) $(CARGO_ENV)
# - Build rustc with itself (so we have a rustc with the right ABI)
$(BINDIR)rustc: $(BINDIR_2)rustc $(BINDIR)cargo $(CARGO_HOME)config $(LIBDIR_2)libtest.rlib
	@mkdir -p $(PREFIX)tmp
	@echo [CARGO] $(RUST_SRC_RUSTC)Cargo.toml '>' $(OUTDIR)build-rustc/
	$V$(RUSTC_ENV_VARS) TMPDIR=$(abspath $(PREFIX)tmp) $(CARGO_ENV_RUSTC) $(BINDIR)cargo build $(CARGO_FLAGS) --manifest-path $(RUST_SRC_RUSTC)Cargo.toml
ifneq ($(TARGETVER_LEAST_1_39),)
	@echo [CARGO] $(RUST_SRC_CODEGEN)Cargo.toml '>' $(OUTDIR)build-rustc/
	$V$(RUSTC_ENV_VARS) TMPDIR=$(abspath $(PREFIX)tmp) $(CARGO_ENV_RUSTC) $(BINDIR)cargo rustc $(CARGO_FLAGS) --manifest-path $(RUST_SRC_CODEGEN)Cargo.toml -- -L $(abspath $(CARGO_OUTDIR_RUSTC)deps)
	@mkdir -p $(LIBDIR)../codegen-backends
	@echo "[CP] libraries and results ($@)"
	$V$(call fn_opt_copy,$(CARGO_OUTDIR_RUSTC)librustc_codegen_llvm.$(DYLIB_EXT),$(LIBDIR)../codegen-backends/librustc_codegen_llvm-llvm.$(DYLIB_EXT))
else
	@echo "[CP] libraries and results ($@)"
endif
	@mkdir -p $(LIBDIR)
	$Vcp $(LIBDIR_2)*.$(DYLIB_EXT) $(PREFIX)lib
	$Vcp $(CARGO_OUTDIR_RUSTC)deps/*.rlib $(LIBDIR)
	$Vcp $(CARGO_OUTDIR_RUSTC)deps/*.$(DYLIB_EXT) $(LIBDIR)
ifeq ($(RUSTC_VERSION),1.19.0)
	$Vcp $(CARGO_OUTDIR_RUSTC)rustc $(BINDIR)rustc_binary
else
	$V$(call fn_opt_copy,$(CARGO_OUTDIR_RUSTC)$(RUSTC_BINNAME),$(BINDIR)rustc_binary)
endif
	$Vprintf '#!/bin/sh\nd=$$(dirname $$0)\nLD_LIBRARY_PATH="$(abspath $(OUTDIR)prefix/lib):$(abspath $(LIBDIR))" $$d/rustc_binary $$@' >$@
	$Vchmod +x $@

CARGO_OUTDIR_STAGE3_STD := $(OUTDIR)build-std/$(RUSTC_TARGET)/release/
CARGO_ENV_STAGE3_STD := CARGO_TARGET_DIR=$(OUTDIR)build-std RUSTC=$(abspath rustc_proxy.sh) PROXY_RUSTC=$(abspath $(BINDIR)rustc) PROXY_MRUSTC=$(abspath $(BINDIR_2)rustc) $(CARGO_ENV)
$(LIBDIR)libstd.rlib: $(BINDIR)rustc
	@mkdir -p $(LIBDIR)
	@echo [CARGO] $(RUST_SRC_LIBS)test/Cargo.toml '>' $(OUTDIR)build-std2
ifeq ($(TARGETVER_LEAST_1_39),)
	$V$(CARGO_ENV_STAGE3_STD) $(BINDIR)cargo build $(CARGO_FLAGS) --manifest-path $(RUST_SRC_LIBS)std/Cargo.toml --features panic-unwind
	$Vcp --remove-destination $(CARGO_OUTDIR_STAGE3_STD)deps/*.rlib $(CARGO_OUTDIR_STAGE3_STD)deps/*.$(DYLIB_EXT) $(LIBDIR)
	$V$(CARGO_ENV_STAGE3_STD) $(BINDIR)cargo build $(CARGO_FLAGS) --manifest-path $(RUST_SRC_LIBS)test/Cargo.toml
else
	$V$(CARGO_ENV_STAGE3_STD) $(BINDIR)cargo build $(CARGO_FLAGS) --manifest-path $(RUST_SRC_LIBS)test/Cargo.toml --features panic-unwind
endif
	$Vcp $(CARGO_OUTDIR_STAGE3_STD)deps/*.rlib $(CARGO_OUTDIR_STAGE3_STD)deps/*.$(DYLIB_EXT) $(LIBDIR)

# Build hello_world using the final fully-bootstrapped compiler
$(BINDIR)hello_world: $(RUST_SRC_HELLO) $(BINDIR)rustc $(LIBDIR)libstd.rlib
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$V$(DBG) $(BINDIR)rustc -L $(LIBDIR) $< -o $@
	./$@

#.PHONY: TEST_BOOTSTRAP
#all:  TEST_BOOTSTRAP
#TEST_BOOTSTRAP: $(BINDIR)rustc $(LIBDIR)libstd.rlib
#	@echo [CARGO] $(RUST_SRC)src/bootstrap/Cargo.toml '>' $(OUTDIR)build-rustc2/
#	$V$(RUSTC_ENV_VARS) TMPDIR=$(abspath $(PREFIX)tmp) CARGO_TARGET_DIR=$(OUTDIR)build-bootstrap RUSTC=$(BINDIR)rustc $(CARGO_ENV) $(BINDIR)cargo build --manifest-path $(RUST_SRC)bootstrap/Cargo.toml --release -j $(PARLEVEL) --frozen --verbose

