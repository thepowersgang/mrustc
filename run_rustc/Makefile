# Makefile that builds libstd using the mrustc-built rustc

RUSTC_VERSION_DEF = $(shell cat ../rust-version)
RUSTC_VERSION ?= $(RUSTC_VERSION_DEF)
RUST_SRC ?= ../rustc-$(RUSTC_VERSION)-src/src/
$(warning ($(RUSTC_VERSION),$(RUST_VERSION_DEF)))
ifeq ($(RUSTC_VERSION),$(RUSTC_VERSION_DEF))
OUTDIR_SUF ?=
else
OUTDIR_SUF ?= -$(RUSTC_VERSION)
endif

PREFIX := prefix$(OUTDIR_SUF)/
BINDIR := $(PREFIX)bin/
LIBDIR := $(PREFIX)lib/rustlib/x86_64-unknown-linux-gnu/lib/

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
	make -C ../ output$(OUTDIR_SUF)/rustc -j 3
../output$(OUTDIR_SUF)/cargo:
	make -C ../ output$(OUTDIR_SUF)/cargo -j 3

$(BINDIR)rustc: ../output$(OUTDIR_SUF)/rustc
	@mkdir -p $(dir $@)
	$Vcp $< $@
$(BINDIR)cargo: ../output$(OUTDIR_SUF)/cargo
	@mkdir -p $(dir $@)
	$Vcp $< $@

#$(LIBDIR)libstd.rlib: $(RUSTC) $(BINDIR)cargo $(BINDIR)rustc
#	@mkdir -p ./build/libstd
#	$(BINDIR)cargo build -Z unstable-options --vendor $(RUST_SRC)vendor --manifest-path $(RUST_SRC)libstd/Cargo.toml --out-dir $(LIBDIR) --target-dir build/libstd

$(LIBDIR)lib%.rlib: $(RUST_SRC)lib%/lib.rs $(BINDIR)rustc
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(BINDIR)rustc --crate-type rlib --crate-name $* -L $(LIBDIR) -L ../output/libs $< -o $@ $(RUSTFLAGS_$*)
$(LIBDIR)lib%.rlib: $(RUST_SRC)lib%/src/lib.rs $(BINDIR)rustc
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(BINDIR)rustc --crate-type rlib --crate-name $* -L $(LIBDIR) -L ../output/libs $< -o $@ $(RUSTFLAGS_$*)

$(BINDIR)hello_world: $(RUST_SRC)test/run-pass/hello.rs $(LIBDIR)libstd.rlib $(RUSTC) 
	$Vmkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(BINDIR)rustc $(RUSTFLAGS_$@) -L $(LIBDIR) -L ../output/libs $< -o $@

fcn_extcrate = $(patsubst %,$(LIBDIR)lib%.rlib,$(1))

$(LIBDIR)libarena.rlib: $(call fcn_extcrate, std)
$(LIBDIR)liballoc.rlib: $(call fcn_extcrate, core compiler_builtins)
ifeq ($(RUSTC_VERSION),1.29.0)
$(LIBDIR)liballoc.rlib: $(call fcn_extcrate, alloc_system)
$(LIBDIR)liballoc_system.rlib: $(call fcn_extcrate, core libc)
endif
$(LIBDIR)libstd_unicode.rlib: $(call fcn_extcrate, core compiler_builtins)
$(LIBDIR)libcollections.rlib: $(call fcn_extcrate, core alloc std_unicode compiler_builtins)
$(LIBDIR)librand.rlib: $(call fcn_extcrate, core compiler_builtins)
$(LIBDIR)liblibc.rlib: $(call fcn_extcrate, core compiler_builtins)
$(LIBDIR)libcompiler_builtins.rlib: $(call fcn_extcrate, core)
$(LIBDIR)libstd.rlib: $(call fcn_extcrate, core collections rand libc unwind compiler_builtins alloc_system panic_unwind)
$(LIBDIR)libunwind.rlib: $(call fcn_extcrate, core libc)
