# Makefile that builds libstd using the mrustc-built rustc

RUSTC := ../output/rustc
RUST_SRC := ../rustc-1.19.0-src/src/

PREFIX := prefix/
BINDIR := $(PREFIX)bin/
LIBDIR := $(PREFIX)lib/

V ?= @


all: $(LIBDIR)libstd.rlib $(BINDIR)hello_world

RUSTFLAGS_output/liblibc.rlib := --cfg stdbuild

$(RUSTC):
	make -C ../ output/rustc -j 3

$(BINDIR)rustc: $(RUSTC)
	@mkdir -p $(dir $@)
	$Vcp $@ $<

$(LIBDIR)%.rlib: $(RUST_SRC)%/lib.rs $(RUSTC)
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(RUSTC) $(RUSTFLAGS_$@) -L output/ -L ../output/libs $< -o $@
$(LIBDIR)%.rlib: $(RUST_SRC)%/src/lib.rs $(RUSTC)
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(RUSTC) $(RUSTFLAGS_$@) -L output/ -L ../output/libs $< -o $@

$(BINDIR)hello_world: $(RUST_SRC)test/run-pass/hello.rs $(LIBDIR)libstd.rlib $(RUSTC) 
	$Vmkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(RUSTC) $(RUSTFLAGS_$@) -L output/ -L ../output/libs $< -o $@

fcn_extcrate = $(patsubst %,$(LIBDIR)lib%.rlib,$(1))

$(LIBDIR)libarena.rlib: $(call fcn_extcrate, std)
$(LIBDIR)liballoc.rlib: $(call fcn_extcrate, core)
$(LIBDIR)libstd_unicode.rlib: $(call fcn_extcrate, core)
$(LIBDIR)libcollections.rlib: $(call fcn_extcrate, core alloc std_unicode)
$(LIBDIR)librand.rlib: $(call fcn_extcrate, core)
$(LIBDIR)liblibc.rlib: $(call fcn_extcrate, core)
$(LIBDIR)libcompiler_builtins.rlib: $(call fcn_extcrate, core)
$(LIBDIR)libstd.rlib: $(call fcn_extcrate, core collections rand libc unwind compiler_builtins alloc_system panic_unwind)
$(LIBDIR)libunwind.rlib: $(call fcn_extcrate, core libc)
