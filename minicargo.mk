#
# minicargo.mk - Makefile that handles invoking `minicargo` to build libstd, rustc, and cargo
#


EXESUF :=
ifeq ($(OS),Windows_NT)
  EXESUF := .exe
endif

# -------------------
# ----- INPUTS ------
# -------------------

# RUSTC_VERSION : Version of rustc to load (picks source dir)
RUSTC_VERSION_DEF := $(shell cat rust-version)
RUSTC_VERSION ?= $(RUSTC_VERSION_DEF)

# OUTDIR_SUF : Output directory suffix
ifeq ($(RUSTC_VERSION),$(RUSTC_VERSION_DEF))
  OUTDIR_SUF_DEF :=
else
  OUTDIR_SUF_DEF := -$(RUSTC_VERSION)
endif
OUTDIR_SUF ?= $(OUTDIR_SUF_DEF)

# MMIR : Set to non-empty to compile Monomorphised MIR
MMIR ?=
# RUSTC_CHANNEL : `rustc` release channel (picks source dir)
RUSTC_CHANNEL ?= stable
# PARLEVEL : `minicargo`'s job count
PARLEVEL ?= 1
# Additional flags for `minicargo` (e.g. library paths)
MINICARGO_FLAGS ?=
# RUST_TESTS_FINAL_STAGE : Final stage for tests run as part of the rust_tests target.
#  VALID OPTIONS: parse, expand, mir, ALL
RUST_TESTS_FINAL_STAGE ?= ALL
# MRUSTC : Executable path to `mrustc`
MRUSTC ?= bin/mrustc$(EXESUF)
# MINICARGO : Executable path to `minicargo`
MINICARGO ?= bin/minicargo$(EXESUF)
# LLVM_TARGETS : Target list for llvm
LLVM_TARGETS ?= X86;ARM;AArch64#;Mips;PowerPC;SystemZ;JSBackend;MSP430;Sparc;NVPTX

ifeq ($(OS),Windows_NT)
  OVERRIDE_SUFFIX ?= -windows
else ifeq ($(shell uname -s || echo not),Darwin)
  OVERRIDE_SUFFIX ?= -macos
else
  OVERRIDE_SUFFIX ?= -linux
endif

# --- Pepare minicargo flags etc ---
# Set up for MMIR mode
ifneq ($(MMIR),)
  OUTDIR_SUF := $(OUTDIR_SUF)-mmir
  MINICARGO_FLAGS += -Z emit-mmir
endif
# Job count
ifneq ($(PARLEVEL),1)
  MINICARGO_FLAGS += -j $(PARLEVEL)
endif
# Target override
ifeq ($(MRUSTC_TARGET),)
else
  MINICARGO_FLAGS += --target $(MRUSTC_TARGET)
  OUTDIR_SUF := $(OUTDIR_SUF)-$(MRUSTC_TARGET)
endif

OUTDIR := output$(OUTDIR_SUF)/

CARGO_ENV_VARS :=
USE_MERGED_BUILD=1
ifeq ($(RUSTC_VERSION),1.19.0)
  RUSTC_OUT_BIN := rustc
  USE_MERGED_BUILD=0
else ifeq ($(RUSTC_VERSION),1.29.0)
  # Diabled due to linking issues
  # - libssh uses an openssl feature that isn't enabled ("engines"), see openssl-src/src/lib.rs:106
  # - But if that feature is enabled, then libcurl doesn't compile :(
  #MINICARGO_FLAGS_$(OUTDIR)cargo := --features vendored-openssl
  #CARGO_ENV_VARS += LIBCURL_NO_PKG_CONFIG=1
  RUSTC_OUT_BIN := rustc_binary
  USE_MERGED_BUILD=0
else ifeq ($(RUSTC_VERSION),1.39.0)
  MINICARGO_FLAGS_$(OUTDIR)cargo := --features vendored-openssl
  RUSTC_OUT_BIN := rustc_binary
else
  MINICARGO_FLAGS_$(OUTDIR)cargo := --features vendored-openssl
  MINICARGO_FLAGS_$(OUTDIR)rustc := --features llvm
  RUSTC_OUT_BIN := rustc_main
endif

ifeq ($(RUSTC_CHANNEL),nightly)
  RUSTCSRC := rustc-nightly-src/
else
  RUSTCSRC := rustc-$(RUSTC_VERSION)-src/
endif
RUSTC_SRC_DL := $(RUSTCSRC)/dl-version
ifeq ($(RUSTC_VERSION),1.19.0)
  VENDOR_DIR := $(RUSTCSRC)src/vendor
else ifeq ($(RUSTC_VERSION),1.29.0)
  VENDOR_DIR := $(RUSTCSRC)src/vendor
else
  VENDOR_DIR := $(RUSTCSRC)vendor
  MINICARGO_FLAGS += --manifest-overrides rustc-$(RUSTC_VERSION)-overrides.toml
endif
ifeq ($(RUSTC_VERSION),1.54.0)
  RUST_LIB_PREFIX := library/
else ifeq ($(RUSTC_VERSION),1.74.0)
  RUST_LIB_PREFIX := library/
else
  RUST_LIB_PREFIX := src/lib
endif

ifeq ($(RUSTC_VERSION),1.19.0)
  LLVM_DIR := src/llvm
else ifeq ($(RUSTC_VERSION),1.29.0)
  LLVM_DIR := src/llvm
else
  LLVM_DIR := src/llvm-project/llvm
endif

SRCDIR_RUSTC := src/rustc
SRCDIR_RUSTC_DRIVER := src/librustc_driver
ifeq ($(RUSTC_VERSION),1.54.0)
  SRCDIR_RUSTC := compiler/rustc
  SRCDIR_RUSTC_DRIVER := compiler/rustc_driver
endif
ifeq ($(RUSTC_VERSION),1.74.0)
  SRCDIR_RUSTC := compiler/rustc
  SRCDIR_RUSTC_DRIVER := compiler/rustc_driver
endif

SRCDIR_RUST_TESTS := $(RUSTCSRC)src/test/
ifeq ($(RUSTC_VERSION),1.74.0)
SRCDIR_RUST_TESTS := $(RUSTCSRC)tests/
endif

LLVM_CONFIG := $(RUSTCSRC)build/bin/llvm-config
ifeq ($(shell uname -s || echo not),Darwin)
 # /usr/bin/uname because uname might call coreutils
 # which can make the arm64 uname called when
 # running under the Rosetta execution environment.
 ifeq ($(shell /usr/bin/uname -m || echo not),arm64)
   RUSTC_TARGET ?= aarch64-apple-darwin
 else ifeq ($(shell /usr/bin/uname -p || echo not),powerpc)
   RUSTC_TARGET ?= powerpc-apple-darwin
 else
   RUSTC_TARGET ?= x86_64-apple-darwin
 endif
else ifeq ($(OS),Windows_NT)
  RUSTC_TARGET ?= x86_64-windows-gnu
else
  RUSTC_TARGET ?= x86_64-unknown-linux-gnu
endif
# Directory for minicargo build script overrides
OVERRIDE_DIR := script-overrides/$(RUSTC_CHANNEL)-$(RUSTC_VERSION)$(OVERRIDE_SUFFIX)/


# ---------------------------------------------------------------------
#  Top-level targets
# ---------------------------------------------------------------------

.PHONY: $(OUTDIR)libstd.rlib $(OUTDIR)libtest.rlib $(OUTDIR)libpanic_unwind.rlib $(OUTDIR)libproc_macro.rlib
.PHONY: $(OUTDIR)rustc $(OUTDIR)cargo

.PHONY: all test LIBS
.PHONY: RUSTCSRC


all: $(OUTDIR)rustc

test: $(OUTDIR)rust/test_run-pass_hello_out.txt

RUSTCSRC: $(RUSTC_SRC_DL)

.PHONY: rust_tests local_tests
rust_tests: RUST_TESTS_run-pass
# rust_tests-run-fail
# rust_tests-compile-fail

# --- Ensure that mrustc/minicargo are built ---
.PHONY: bin/mrustc$(EXESUF) bin/minicargo$(EXESUF) bin/testrunner$(EXESUF) bin/standalone_miri$(EXESUF)
bin/mrustc$(EXESUF):
	$(MAKE) -f Makefile all
	test -e $@

ifeq ($(MMIR),)
  MINICARGO_DEP_SMIRI=
else
  MINICARGO_DEP_SMIRI=bin/standalone_miri$(EXESUF)
endif

bin/minicargo$(EXESUF): $(MINICARGO_DEP_SMIRI)
	$(MAKE) -C tools/minicargo/
	test -e $@
bin/standalone_miri$(EXESUF):
	$(MAKE) -C tools/minicargo/
	test -e $@
bin/testrunner$(EXESUF):
	$(MAKE) -C tools/testrunner/
	test -e $@


#
# rustc (with std/cargo) source download
#
RUSTC_SRC_TARBALL := rustc-$(RUSTC_VERSION)-src.tar.gz
$(RUSTC_SRC_TARBALL):
	@echo [CURL] $@
	@rm -f $@
	@curl -sS https://static.rust-lang.org/dist/$@ -o $@
rustc-$(RUSTC_VERSION)-src/extracted: $(RUSTC_SRC_TARBALL)
	tar -xzf $(RUSTC_SRC_TARBALL)
	touch $@
$(RUSTC_SRC_DL): rustc-$(RUSTC_VERSION)-src/extracted rustc-$(RUSTC_VERSION)-src.patch
	cd $(RUSTCSRC) && patch -p0 < ../rustc-$(RUSTC_VERSION)-src.patch;
	touch $@

# Standard library crates
# - libstd, libpanic_unwind, libtest and libgetopts
# - libproc_macro (mrustc)
ifeq ($(USE_MERGED_BUILD),1)
$(RUSTCSRC)mrustc-stdlib/Cargo.toml: $(RUSTC_SRC_DL) minicargo.mk
	@mkdir -p $(dir $@)
	@echo "#![no_core]" > $(dir $@)/lib.rs
	@echo "[package]" > $@
	@echo "name = \"mrustc_standard_library\"" >> $@
	@echo "version = \"0.0.0\"" >> $@
	@echo "[lib]" >> $@
	@echo "path = \"lib.rs\"" >> $@
	@echo "[dependencies]" >> $@
	@echo "std = { path = \"../$(RUST_LIB_PREFIX)std\" }" >> $@
	@echo "panic_unwind = { path = \"../$(RUST_LIB_PREFIX)panic_unwind\" }" >> $@
	@echo "test = { path = \"../$(RUST_LIB_PREFIX)test\" }" >> $@
LIBS: $(RUSTCSRC)mrustc-stdlib/Cargo.toml $(MRUSTC) $(MINICARGO)
	+$(MINICARGO) --vendor-dir $(VENDOR_DIR) --script-overrides $(OVERRIDE_DIR) --output-dir $(OUTDIR) $(MINICARGO_FLAGS) $(RUSTCSRC)mrustc-stdlib/
	+$(MINICARGO) --output-dir $(OUTDIR) $(MINICARGO_FLAGS) lib/libproc_macro
else
LIBS: $(MRUSTC) $(MINICARGO) $(RUSTC_SRC_DL)
	+$(MINICARGO) --vendor-dir $(VENDOR_DIR) --script-overrides $(OVERRIDE_DIR) --output-dir $(OUTDIR) $(MINICARGO_FLAGS) $(RUSTCSRC)$(RUST_LIB_PREFIX)std
	+$(MINICARGO) --vendor-dir $(VENDOR_DIR) --script-overrides $(OVERRIDE_DIR) --output-dir $(OUTDIR) $(MINICARGO_FLAGS) $(RUSTCSRC)$(RUST_LIB_PREFIX)panic_unwind
	+$(MINICARGO) --vendor-dir $(VENDOR_DIR) --script-overrides $(OVERRIDE_DIR) --output-dir $(OUTDIR) $(MINICARGO_FLAGS) $(RUSTCSRC)$(RUST_LIB_PREFIX)test
	+$(MINICARGO) --output-dir $(OUTDIR) $(MINICARGO_FLAGS) lib/libproc_macro
endif

# Dynamically linked version of the standard library
$(OUTDIR)test/libtest.so: $(RUSTC_SRC_DL)
	mkdir -p $(dir $@)
	+MINICARGO_DYLIB=1 $(MINICARGO) $(RUSTCSRC)$(RUST_LIB_PREFIX)std          --vendor-dir $(VENDOR_DIR) --script-overrides $(OVERRIDE_DIR) --output-dir $(dir $@) $(MINICARGO_FLAGS)
	+MINICARGO_DYLIB=1 $(MINICARGO) $(RUSTCSRC)$(RUST_LIB_PREFIX)panic_unwind --vendor-dir $(VENDOR_DIR) --script-overrides $(OVERRIDE_DIR) --output-dir $(dir $@) $(MINICARGO_FLAGS)
	+MINICARGO_DYLIB=1 $(MINICARGO) $(RUSTCSRC)$(RUST_LIB_PREFIX)test         --vendor-dir $(VENDOR_DIR) --output-dir $(dir $@) $(MINICARGO_FLAGS)
	test -e $@

RUSTC_ENV_VARS := CFG_COMPILER_HOST_TRIPLE=$(RUSTC_TARGET)
RUSTC_ENV_VARS += LLVM_CONFIG=$(abspath $(LLVM_CONFIG))
RUSTC_ENV_VARS += CFG_RELEASE=$(RUSTC_VERSION)	# Claiming stable
RUSTC_ENV_VARS += CFG_RELEASE_CHANNEL=$(RUSTC_CHANNEL)
RUSTC_ENV_VARS += CFG_VERSION=$(RUSTC_VERSION)-$(RUSTC_CHANNEL)-mrustc
RUSTC_ENV_VARS += CFG_PREFIX=mrustc
RUSTC_ENV_VARS += CFG_LIBDIR_RELATIVE=lib
RUSTC_ENV_VARS += LD_LIBRARY_PATH=$(abspath $(OUTDIR))
RUSTC_ENV_VARS += REAL_LIBRARY_PATH_VAR=LD_LIBRARY_PATH
RUSTC_ENV_VARS += RUSTC_INSTALL_BINDIR=bin

$(OUTDIR)rustc: $(MRUSTC) $(MINICARGO) LIBS $(LLVM_CONFIG)
	mkdir -p $(OUTDIR)rustc-build
	+$(RUSTC_ENV_VARS) $(MINICARGO) $(RUSTCSRC)$(SRCDIR_RUSTC) --vendor-dir $(VENDOR_DIR) --output-dir $(OUTDIR)rustc-build -L $(OUTDIR) $(MINICARGO_FLAGS) $(MINICARGO_FLAGS_$@)
	test -e $@ -a ! $(OUTDIR)rustc-build/$(RUSTC_OUT_BIN) -nt $@ || cp $(OUTDIR)rustc-build/$(RUSTC_OUT_BIN) $@
$(OUTDIR)rustc-build/librustc_driver.rlib: $(MRUSTC) $(MINICARGO) LIBS
	mkdir -p $(OUTDIR)rustc-build
	+$(RUSTC_ENV_VARS) $(MINICARGO) $(RUSTCSRC)$(SRCDIR_RUSTC_DRIVER) --vendor-dir $(VENDOR_DIR) --output-dir $(OUTDIR)rustc-build -L $(OUTDIR) $(MINICARGO_FLAGS) $(MINICARGO_FLAGS_$(OUTDIR)rustc)
$(OUTDIR)cargo: $(MRUSTC) LIBS
	mkdir -p $(OUTDIR)cargo-build
	+$(CARGO_ENV_VARS) $(MINICARGO) $(RUSTCSRC)src/tools/cargo --vendor-dir $(VENDOR_DIR) --output-dir $(OUTDIR)cargo-build -L $(OUTDIR) $(MINICARGO_FLAGS) $(MINICARGO_FLAGS_$@)
	test -e $@ -a ! $(OUTDIR)cargo-build/cargo -nt $@ || cp $(OUTDIR)cargo-build/cargo $@

# Reference $(RUSTCSRC)src/bootstrap/native.rs for these values
LLVM_CMAKE_OPTS := LLVM_TARGET_ARCH=$(firstword $(subst -, ,$(RUSTC_TARGET))) LLVM_DEFAULT_TARGET_TRIPLE=$(RUSTC_TARGET)
LLVM_CMAKE_OPTS += LLVM_TARGETS_TO_BUILD="$(LLVM_TARGETS)"
LLVM_CMAKE_OPTS += LLVM_ENABLE_ASSERTIONS=OFF
LLVM_CMAKE_OPTS += LLVM_INCLUDE_EXAMPLES=OFF LLVM_INCLUDE_TESTS=OFF LLVM_INCLUDE_DOCS=OFF
LLVM_CMAKE_OPTS += LLVM_INCLUDE_BENCHMARKS=OFF
LLVM_CMAKE_OPTS += LLVM_ENABLE_ZLIB=OFF LLVM_ENABLE_TERMINFO=OFF LLVM_ENABLE_LIBEDIT=OFF WITH_POLLY=OFF
LLVM_CMAKE_OPTS += CMAKE_CXX_COMPILER="$(CXX)" CMAKE_C_COMPILER="$(CC)"
LLVM_CMAKE_OPTS += CMAKE_BUILD_TYPE=Release
LLVM_CMAKE_OPTS += $(LLVM_CMAKE_OPTS_EXTRA)


$(RUSTCSRC)build/bin/llvm-config: $(RUSTCSRC)build/Makefile
	$Vcd $(RUSTCSRC)build && $(MAKE) -j $(PARLEVEL)

$(RUSTCSRC)build/Makefile: $(RUSTCSRC)$(LLVM_DIR)/CMakeLists.txt
	@mkdir -p $(RUSTCSRC)build
	$Vcd $(RUSTCSRC)build && cmake $(addprefix -D , $(LLVM_CMAKE_OPTS)) ../$(LLVM_DIR)

#
# Developement-only targets
#
$(OUTDIR)libcore.rlib: $(MRUSTC) $(MINICARGO)
	$(MINICARGO) $(RUSTCSRC)src/libcore --script-overrides $(OVERRIDE_DIR) --output-dir $(OUTDIR) $(MINICARGO_FLAGS)
$(OUTDIR)liballoc.rlib: $(MRUSTC) $(MINICARGO)
	$(MINICARGO) $(RUSTCSRC)src/liballoc --vendor-dir $(VENDOR_DIR) --script-overrides $(OVERRIDE_DIR) --output-dir $(OUTDIR) $(MINICARGO_FLAGS)
$(OUTDIR)rustc-build/librustdoc.rlib: $(MRUSTC) LIBS
	$(MINICARGO) $(RUSTCSRC)src/librustdoc --vendor-dir $(VENDOR_DIR) --output-dir $(dir $@) -L $(OUTDIR) $(MINICARGO_FLAGS)
#$(OUTDIR)cargo-build/libserde-1_0_6.rlib: $(MRUSTC) LIBS
#	$(MINICARGO) $(VENDOR_DIR)/serde --vendor-dir $(VENDOR_DIR) --output-dir $(dir $@) -L $(OUTDIR) $(MINICARGO_FLAGS)
$(OUTDIR)cargo-build/libgit2-0_6_6.rlib: $(MRUSTC) LIBS
	$(MINICARGO) $(VENDOR_DIR)/git2 --vendor-dir $(VENDOR_DIR) --output-dir $(dir $@) -L $(OUTDIR) --features ssh,https,curl,openssl-sys,openssl-probe $(MINICARGO_FLAGS)
$(OUTDIR)cargo-build/libserde_json-1_0_2.rlib: $(MRUSTC) LIBS
	$(MINICARGO) $(VENDOR_DIR)/serde_json --vendor-dir $(VENDOR_DIR) --output-dir $(dir $@) -L $(OUTDIR) $(MINICARGO_FLAGS)
$(OUTDIR)cargo-build/libcurl-0_4_6.rlib: $(MRUSTC) LIBS
	$(MINICARGO) $(VENDOR_DIR)/curl --vendor-dir $(VENDOR_DIR) --output-dir $(dir $@) -L $(OUTDIR) $(MINICARGO_FLAGS)
$(OUTDIR)cargo-build/libterm-0_4_5.rlib: $(MRUSTC) LIBS
	$(MINICARGO) $(VENDOR_DIR)/term --vendor-dir $(VENDOR_DIR) --output-dir $(dir $@) -L $(OUTDIR) $(MINICARGO_FLAGS)
$(OUTDIR)cargo-build/libfailure-0_1_2.rlib: $(MRUSTC) LIBS
	$(MINICARGO) $(VENDOR_DIR)/failure --vendor-dir $(VENDOR_DIR) --output-dir $(dir $@) -L $(OUTDIR) --features std,derive,backtrace,failure_derive $(MINICARGO_FLAGS)


#
# TEST: Rust standard library and the "hello, world" run-pass test
#

HELLO_TEST := ui/hello.rs
ifeq ($(RUSTC_VERSION),1.19.0)
  HELLO_TEST := run-pass/hello.rs
else ifeq ($(RUSTC_VERSION),1.29.0)
  HELLO_TEST := run-pass/hello.rs
endif

# "hello, world" test - Invoked by the `make test` target
$(OUTDIR)rust/test_run-pass_hello: $(SRCDIR_RUST_TESTS)$(HELLO_TEST) LIBS
	@mkdir -p $(dir $@)
	@echo "--- [MRUSTC] -o $@"
	$(DBG) $(MRUSTC) $< -o $@ --cfg debug_assertions -g -O -L $(OUTDIR) > $@_dbg.txt
$(OUTDIR)rust/test_run-pass_hello_out.txt: $(OUTDIR)rust/test_run-pass_hello
	@echo "--- [$<]"
	@./$< | tee $@

# 
# RUSTC TESTS
# 

.PHONY: RUST_TESTS RUST_TESTS_run-pass
RUST_TESTS: RUST_TESTS_run-pass
RUST_TESTS_run-pass: output$(OUTDIR_SUF)/test/librust_test_helpers.a LIBS bin/testrunner$(EXESUF)
	@mkdir -p $(OUTDIR)rust_tests/run-pass
	./bin/testrunner$(EXESUF) -L $(OUTDIR) -L $(OUTDIR)test -o $(OUTDIR)rust_tests/run-pass $(SRCDIR_RUST_TESTS)run-pass --exceptions disabled_tests_run-pass.txt
$(OUTDIR)test/librust_test_helpers.a: $(OUTDIR)test/rust_test_helpers.o
	@mkdir -p $(dir $@)
	ar cur $@ $<
ifeq ($(RUSTC_VERSION),1.19.0)
RUST_TEST_HELPERS_C := $(RUSTCSRC)src/rt/rust_test_helpers.c
else
RUST_TEST_HELPERS_C := $(RUSTCSRC)src/test/auxiliary/rust_test_helpers.c
endif
output$(OUTDIR_SUF)/test/rust_test_helpers.o: $(RUST_TEST_HELPERS_C)
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

#
# MRUSTC-specific tests
# 
.PHONY: local_tests
local_tests: $(TEST_DEPS)
	@$(MAKE) -C tools/testrunner
	@mkdir -p output$(OUTDIR_SUF)/local_tests
	./bin/testrunner -o output$(OUTDIR_SUF)/local_tests -L output$(OUTDIR_SUF) samples/test

#
# Testing
#
.PHONY: rust_tests-libs

LIB_TESTS := 
LIB_TESTS += alloc
LIB_TESTS += std
LIB_TESTS += rustc_data_structures
#LIB_TESTS += rustc
rust_tests-libs: $(patsubst %,$(OUTDIR)stdtest/%-test_out.txt, $(LIB_TESTS))
rust_tests-libs: $(OUTDIR)stdtest/collectionstests_out.txt
.PRECIOUS: $(OUTDIR)stdtest/alloc-test
.PRECIOUS: $(OUTDIR)stdtest/std-test
.PRECIOUS: $(OUTDIR)stdtest/rustc_data_structures-test
#.PRECIOUS: $(OUTDIR)stdtest/rustc-test

RUNTIME_ARGS_$(OUTDIR)stdtest/alloc-test := --test-threads 1
RUNTIME_ARGS_$(OUTDIR)stdtest/alloc-test += --skip ::collections::linked_list::tests::test_fuzz
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test := --test-threads 1
# VVV Requires panic destructors (unwinding panics)
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::io::stdio::tests::panic_doesnt_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::io::buffered::tests::panic_in_write_doesnt_flush_in_drop
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mutex::tests::test_arc_condvar_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mutex::tests::test_mutex_arc_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::once::tests::poison_bad
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::once::tests::wait_for_force_to_finish
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::rwlock::tests::test_rw_arc_no_poison_rw
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::rwlock::tests::test_rw_arc_poison_wr
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::rwlock::tests::test_rw_arc_poison_ww
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sys_common::remutex::tests::poison_works
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::thread::local::tests::dtors_in_dtors_in_dtors
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::thread::local::tests::smoke_dtor
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mutex::tests::test_get_mut_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mutex::tests::test_into_inner_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mutex::tests::test_mutex_arc_access_in_unwind
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::rwlock::tests::test_get_mut_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::rwlock::tests::test_into_inner_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::rwlock::tests::test_rw_arc_access_in_unwind
#RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mpsc::sync_tests::oneshot_multi_task_recv_then_close
#RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mpsc::sync_tests::oneshot_multi_thread_recv_close_stress
#RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mpsc::sync_tests::oneshot_single_thread_recv_chan_close
# VVV Requires u128 literals
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::net::ip::tests::test_ipv6_to_int
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::net::ip::tests::test_int_to_ipv6
RUNTIME_ARGS_$(OUTDIR)stdtest/rustc_data_structures-test := --test-threads 1
RUNTIME_ARGS_$(OUTDIR)stdtest/collectionstests := --test-threads 1
# VVV Requires unwinding panics
RUNTIME_ARGS_$(OUTDIR)stdtest/collectionstests += --skip ::slice::test_box_slice_clone_panics
RUNTIME_ARGS_$(OUTDIR)stdtest/collectionstests += --skip ::slice::panic_safe
RUNTIME_ARGS_$(OUTDIR)stdtest/collectionstests += --skip ::vec::drain_filter_consumed_panic
RUNTIME_ARGS_$(OUTDIR)stdtest/collectionstests += --skip ::vec::drain_filter_unconsumed_panic
# No support for custom alignment
RUNTIME_ARGS_$(OUTDIR)stdtest/collectionstests += --skip ::vec::overaligned_allocations

#ENV_$(OUTDIR)stdtest/rustc-test := 
#ENV_$(OUTDIR)stdtest/rustc-test += CFG_COMPILER_HOST_TRIPLE=$(RUSTC_TARGET)

$(OUTDIR)stdtest/%-test: $(RUSTCSRC)src/lib%/lib.rs LIBS
	+MRUSTC_LIBDIR=$(abspath $(OUTDIR)) $(MINICARGO) --test $(RUSTCSRC)src/lib$* --vendor-dir $(VENDOR_DIR) --output-dir $(dir $@) -L $(OUTDIR)
$(OUTDIR)stdtest/collectionstests: $(OUTDIR)stdtest/alloc-test
	test -e $@
$(OUTDIR)collectionstest_out.txt: $(OUTDIR)%
$(OUTDIR)%_out.txt: $(OUTDIR)% minicargo.mk
	@echo "--- [$<]"
	$(ENV_$<) $V./$< $(RUNTIME_ARGS_$<) > $@ 2>&1 || (tail -n 1 $@; mv $@ $@_fail; false)
