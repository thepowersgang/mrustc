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

PREFIX := prefix$(OUTDIR_SUF)/
BINDIR := $(PREFIX)bin/
LIBDIR := $(PREFIX)lib/rustlib/$(RUSTC_TARGET)/lib/
CARGO_HOME := $(PREFIX)cargo_home/
PREFIX_S := prefix-s$(OUTDIR_SUF)/
LIBDIR_S := $(PREFIX_S)lib/rustlib/$(RUSTC_TARGET)/lib/
BINDIR_S := $(PREFIX_S)bin/

V ?= @


all: $(BINDIR)rustc $(LIBDIR)libstd.rlib $(LIBDIR)libproc_macro.rlib $(BINDIR)hello_world
all: $(BINDIR)cargo

RUSTFLAGS_core := 
RUSTFLAGS_libc := --cfg stdbuild
RUSTFLAGS_std := -l dl -l rt -l pthread
RUSTFLAGS_unwind := -l gcc_s
RUSTFLAGS_alloc := 
RUSTFLAGS_alloc_system := 
RUSTFLAGS_compiler_builtins := --cfg feature=\"compiler-builtins\"

../output$(OUTDIR_SUF)/rustc:
	make -C ../ output$(OUTDIR_SUF)/rustc -j 3
../output$(OUTDIR_SUF)/cargo:
	make -C ../ output$(OUTDIR_SUF)/cargo -j 3

$(BINDIR)rustc: ../output$(OUTDIR_SUF)/rustc
	@mkdir -p $(dir $@)
	$Vcp $< $@
$(BINDIR_S)rustc: ../output$(OUTDIR_SUF)/rustc
	@mkdir -p $(dir $@)
	$Vcp $< $@
$(BINDIR)cargo: ../output$(OUTDIR_SUF)/cargo
	@mkdir -p $(dir $@)
	$Vcp $< $@

$(CARGO_HOME)config: Makefile
	mkdir -p $(dir $@)
	echo "[source.crates-io]" > $@
	echo "replace-with = \"vendored-sources\"" >> $@
	echo "[source.vendored-sources]" >> $@
	echo "directory = \"$(abspath $(RUST_SRC)vendor)\"" >> $@

# Actual libstd build (using cargo)
$(LIBDIR)libstd.rlib: $(BINDIR_S)rustc $(BINDIR)cargo $(LIBDIR_S)libstd.rlib $(CARGO_HOME)config
	@mkdir -p ./build/libstd
	RUSTC=$(BINDIR_S)rustc CFG_COMPILER_HOST_TRIPLE=$(RUSTC_TARGET) RUSTC_BOOTSTRAP=1 CARGO_HOME=$(CARGO_HOME) $(BINDIR)cargo build -Z unstable-options --manifest-path $(RUST_SRC)libstd/Cargo.toml --out-dir $(LIBDIR) --target-dir build$(OUTDIR_SUF) -j 1 --release --verbose
$(LIBDIR)libproc_macro.rlib: $(LIBDIR)libstd.rlib $(CARGO_HOME)config
	@mkdir -p ./build$(OUTDIR_SUF)
	RUSTC=$(BINDIR)rustc CFG_COMPILER_HOST_TRIPLE=$(RUSTC_TARGET) RUSTC_BOOTSTRAP=1 CARGO_HOME=$(CARGO_HOME) $(BINDIR)cargo build -Z unstable-options --manifest-path $(RUST_SRC)libproc_macro/Cargo.toml --out-dir $(LIBDIR) --target-dir build$(OUTDIR_SUF) -j 1 --verbose
#	MINICARGO_COMPILER=$(BINDIR)rustc ../tools/bin/minicargo --vendor-dir $(RUST_SRC)vendor --output-dir build$(OUTDIR_SUF) $(RUST_SRC)libproc_macro

$(BINDIR)hello_world: $(RUST_SRC)test/run-pass/hello.rs $(LIBDIR)libstd.rlib $(BINDIR)rustc
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(BINDIR)rustc $(RUSTFLAGS_$@) -L $(LIBDIR) -L ../output/libs $< -o $@

# 
# - Build libstd in a hacky hard-coded way first, to allow build scripts to work
# 
$(LIBDIR_S)lib%.rlib: $(RUST_SRC)lib%/lib.rs $(BINDIR)rustc
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(BINDIR)rustc --crate-type rlib --crate-name $* -L $(LIBDIR_S) $< -o $@ $(RUSTFLAGS_$*)
$(LIBDIR_S)lib%.rlib: $(RUST_SRC)lib%/src/lib.rs $(BINDIR)rustc
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(BINDIR)rustc --crate-type rlib --crate-name $* -L $(LIBDIR_S) $< -o $@ $(RUSTFLAGS_$*)

fcn_extcrate = $(patsubst %,$(LIBDIR_S)lib%.rlib,$(1))

$(LIBDIR_S)libarena.rlib: $(call fcn_extcrate, std)
$(LIBDIR_S)liballoc.rlib: $(call fcn_extcrate, core compiler_builtins)
ifeq ($(RUSTC_VERSION),1.29.0)
$(LIBDIR_S)liballoc.rlib: $(call fcn_extcrate, alloc_system)
$(LIBDIR_S)liballoc_system.rlib: $(call fcn_extcrate, core libc)
$(LIBDIR_S)libpanic_unwind.rlib: $(call fcn_extcrate, alloc)
endif
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
