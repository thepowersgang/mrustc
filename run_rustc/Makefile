# Makefile that builds libstd using the mrustc-built rustc

RUSTC := ../output/rustc
RUST_SRC := ../rustc-1.19.0-src/src/

PREFIX := prefix/
BINDIR := $(PREFIX)bin/
LIBDIR := $(PREFIX)lib/rustlib/x86_64-unknown-linux-gnu/lib/

V ?= @


all: $(BINDIR)rustc $(BINDIR)cargo $(LIBDIR)libstd.rlib $(BINDIR)hello_world

RUSTFLAGS_liblibc := --cfg stdbuild
RUSTFLAGS_libstd := -l dl -l rt -l pthread
RUSTFLAGS_libunwind := -l gcc_s

$(RUSTC):
	make -C ../ output/rustc -j 3
../output/cargo:
	make -C ../ output/cargo -j 3

$(BINDIR)rustc: $(RUSTC)
	@mkdir -p $(dir $@)
	$Vcp $< $@
$(BINDIR)cargo: ../output/cargo
	@mkdir -p $(dir $@)
	$Vcp $< $@

#$(LIBDIR)libstd.rlib-: $(RUSTC) $(BINDIR)cargo $(BINDIR)rustc
#	@mkdir -p ./build/libstd
#	cd $(RUST_SRC)libstd && CARGO_TARGET_DIR=$(abspath ./build/libstd) PATH="$(abspath $(BINDIR)):$$PATH" cargo build
#	cp ./build/libstd/target/libstd.rlib $@

$(LIBDIR)%.rlib: $(RUST_SRC)%/lib.rs $(BINDIR)rustc
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(RUSTC) $(RUSTFLAGS_$@) -L $(LIBDIR) -L ../output/libs $< -o $@ $(RUSTFLAGS_$*)
$(LIBDIR)%.rlib: $(RUST_SRC)%/src/lib.rs $(BINDIR)rustc
	@mkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(RUSTC) $(RUSTFLAGS_$@) -L $(LIBDIR) -L ../output/libs $< -o $@ $(RUSTFLAGS_$*)

$(BINDIR)hello_world: $(RUST_SRC)test/run-pass/hello.rs $(LIBDIR)libstd.rlib $(RUSTC) 
	$Vmkdir -p $(dir $@)
	@echo "[RUSTC] -o $@"
	$Vtime $(DBG) $(RUSTC) $(RUSTFLAGS_$@) -L $(LIBDIR) -L ../output/libs $< -o $@

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
