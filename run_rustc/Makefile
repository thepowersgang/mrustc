RUSTC := ../output/rustc
RUST_SRC := ../rustc-nightly/src/

all: output/libstd.rlib output/hello_world

RUSTFLAGS_output/liblibc.rlib := --cfg stdbuild

$(RUSTC):
	make -C ../ output/rustc -j 3

output/%.rlib: $(RUST_SRC)%/lib.rs $(RUSTC)
	mkdir -p $(dir $@)
	time $(RUSTC) $(RUSTFLAGS_$@) -L output/ -L ../output/libs $< -o $@
output/%.rlib: $(RUST_SRC)%/src/lib.rs $(RUSTC)
	mkdir -p $(dir $@)
	time $(RUSTC) $(RUSTFLAGS_$@) -L output/ -L ../output/libs $< -o $@

output/hello_world: $(RUST_SRC)test/run-pass/hello.rs output/libstd.rlib $(RUSTC) 
	time $(RUSTC) $(RUSTFLAGS_$@) -L output/ -L ../output/libs $< -o $@

fcn_extcrate = $(patsubst %,output/lib%.rlib,$(1))

output/libarena.rlib: output/libstd.rlib
output/liballoc.rlib: output/libcore.rlib
output/libstd_unicode.rlib: output/libcore.rlib
output/libcollections.rlib: output/libcore.rlib output/liballoc.rlib output/libstd_unicode.rlib
output/librand.rlib: output/libcore.rlib
output/liblibc.rlib: output/libcore.rlib
output/libcompiler_builtins.rlib: output/libcore.rlib
output/libstd.rlib: $(call fcn_extcrate, core collections rand libc unwind compiler_builtins alloc_system panic_unwind)
output/libunwind.rlib: $(call fcn_extcrate, core libc)
