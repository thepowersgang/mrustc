# MRustC - Rust Compiler
# - By John Hodge (Mutabah/thePowersGang)
#
# Makefile
#
# - Compiles mrustc
# - Downloads rustc source to test against
# - Attempts to compile rust's libstd
#
# DEPENDENCIES
# - zlib (-dev)
# - curl (bin, for downloading libstd source)
ifeq ($(OS),Windows_NT)
  EXESUF ?= .exe
endif
EXESUF ?=
CXX ?= g++
V ?= !
GPROF ?=
ifeq ($(V),!)
  V := @
else
  V :=
endif

TAIL_COUNT ?= 10

# - Disable implicit rules
.SUFFIXES:
# - Disable deleting intermediate files
.SECONDARY:

# - Final stage for tests run as part of the rust_tests target.
#  VALID OPTIONS: parse, expand, mir, ALL
RUST_TESTS_FINAL_STAGE ?= ALL

LINKFLAGS := -g
LIBS := -lz
CXXFLAGS := -g -Wall
CXXFLAGS += -std=c++14
#CXXFLAGS += -Wextra
CXXFLAGS += -O2
CXXFLAGS += $(CXXFLAGS_EXTRA)

CPPFLAGS := -I src/include/ -I src/
CPPFLAGS += -I tools/common/

CXXFLAGS += -Wno-pessimizing-move
CXXFLAGS += -Wno-misleading-indentation
#CXXFLAGS += -Wno-unused-private-field
CXXFLAGS += -Wno-unknown-warning-option

CXXFLAGS += -Werror=return-type


# - Flags to pass to all mrustc invocations
RUST_FLAGS := --cfg debug_assertions
RUST_FLAGS += -g
RUST_FLAGS += -O
RUST_FLAGS += -L output$(OUTDIR_SUF)/
RUST_FLAGS += $(RUST_FLAGS_EXTRA)

SHELL = bash

ifeq ($(DBGTPL),)
else ifeq ($(DBGTPL),gdb)
  DBG := echo -e "r\nbt 14\nq" | gdb --args
else ifeq ($(DBGTPL),valgrind)
  DBG := valgrind --leak-check=full --num-callers=35
else ifeq ($(DBGTPL),time)
  DBG := time
else
  $(error "Unknown debug template")
endif

OBJDIR = .obj/

ifneq ($(GPROF),)
  OBJDIR := .obj-gprof/
  CXXFLAGS += -pg -no-pie
  LINKFLAGS += -pg -no-pie
  EXESUF := -gprof$(EXESUF)
endif

BIN := bin/mrustc$(EXESUF)

OBJ := main.o version.o
OBJ += span.o rc_string.o debug.o ident.o
OBJ += ast/ast.o
OBJ +=  ast/types.o ast/crate.o ast/path.o ast/expr.o ast/pattern.o
OBJ +=  ast/dump.o
OBJ += parse/parseerror.o
OBJ +=  parse/token.o parse/tokentree.o parse/interpolated_fragment.o
OBJ +=  parse/tokenstream.o parse/lex.o parse/ttstream.o
OBJ += parse/root.o parse/paths.o parse/types.o parse/expr.o parse/pattern.o
OBJ += expand/mod.o expand/macro_rules.o expand/cfg.o
OBJ +=  expand/format_args.o expand/asm.o
OBJ +=  expand/concat.o expand/stringify.o expand/file_line.o
OBJ +=  expand/derive.o expand/lang_item.o
OBJ +=  expand/std_prelude.o expand/crate_tags.o
OBJ +=  expand/include.o
OBJ +=  expand/env.o
OBJ +=  expand/test.o
OBJ +=  expand/rustc_diagnostics.o
OBJ +=  expand/proc_macro.o
OBJ +=  expand/assert.o expand/compile_error.o
OBJ +=  expand/codegen.o expand/doc.o expand/lints.o expand/misc_attrs.o expand/stability.o
OBJ +=  expand/panic.o
OBJ += expand/test_harness.o
OBJ += macro_rules/mod.o macro_rules/eval.o macro_rules/parse.o
OBJ += resolve/use.o resolve/index.o resolve/absolute.o resolve/common.o
OBJ += hir/from_ast.o hir/from_ast_expr.o
OBJ +=  hir/dump.o
OBJ +=  hir/hir.o hir/hir_ops.o hir/generic_params.o
OBJ +=  hir/crate_ptr.o hir/expr_ptr.o
OBJ +=  hir/type.o hir/path.o hir/expr.o hir/pattern.o
OBJ +=  hir/visitor.o hir/crate_post_load.o
OBJ +=  hir/inherent_cache.o
OBJ += hir_conv/expand_type.o hir_conv/constant_evaluation.o hir_conv/resolve_ufcs.o hir_conv/bind.o hir_conv/markings.o
OBJ += hir_typeck/outer.o hir_typeck/common.o hir_typeck/helpers.o hir_typeck/static.o hir_typeck/impl_ref.o
OBJ += hir_typeck/resolve_common.o
OBJ += hir_typeck/expr_visit.o
OBJ += hir_typeck/expr_cs.o hir_typeck/expr_cs__enum.o
OBJ += hir_typeck/expr_check.o
OBJ += hir_expand/annotate_value_usage.o hir_expand/closures.o
OBJ += hir_expand/ufcs_everything.o
OBJ += hir_expand/reborrow.o hir_expand/erased_types.o hir_expand/vtable.o
OBJ += hir_expand/static_borrow_constants.o
OBJ += mir/mir.o mir/mir_ptr.o
OBJ +=  mir/dump.o mir/helpers.o mir/visit_crate_mir.o
OBJ +=  mir/from_hir.o mir/from_hir_match.o mir/mir_builder.o
OBJ +=  mir/check.o mir/cleanup.o mir/optimise.o
OBJ +=  mir/check_full.o
OBJ += hir/serialise.o hir/deserialise.o hir/serialise_lowlevel.o
OBJ += trans/trans_list.o trans/mangling_v2.o
OBJ += trans/enumerate.o trans/auto_impls.o trans/monomorphise.o trans/codegen.o
OBJ += trans/codegen_c.o trans/codegen_c_structured.o trans/codegen_mmir.o
OBJ += trans/target.o trans/allocator.o

PCHS := ast/ast.hpp

OBJ := $(addprefix $(OBJDIR),$(OBJ))


all: $(BIN)

clean:
	$(RM) -r $(BIN) $(OBJ) bin/mrustc.a


PIPECMD ?= 2>&1 | tee $@_dbg.txt | tail -n $(TAIL_COUNT) ; test $${PIPESTATUS[0]} -eq 0

#RUSTC_SRC_TY ?= nightly
RUSTC_SRC_TY ?= stable
ifeq ($(RUSTC_SRC_TY),nightly)
RUSTC_SRC_DES := rust-nightly-date
RUSTCSRC := rustc-nightly-src/
else ifeq ($(RUSTC_SRC_TY),stable)
RUSTC_SRC_DES := rust-version
RUSTC_VERSION ?= $(shell cat $(RUSTC_SRC_DES))
RUSTCSRC := rustc-$(RUSTC_VERSION)-src/
else
$(error Unknown rustc channel)
endif
RUSTC_SRC_DL := $(RUSTCSRC)/dl-version

MAKE_MINICARGO = $(MAKE) -f minicargo.mk RUSTC_VERSION=$(RUSTC_VERSION) RUSTC_CHANNEL=$(RUSTC_SRC_TY) OUTDIR_SUF=$(OUTDIR_SUF)


output$(OUTDIR_SUF)/libstd.rlib: $(RUSTC_SRC_DL) $(BIN)
	$(MAKE_MINICARGO) $@
output$(OUTDIR_SUF)/libtest.rlib output$(OUTDIR_SUF)/libpanic_unwind.rlib output$(OUTDIR_SUF)/libproc_macro.rlib: output$(OUTDIR_SUF)/libstd.rlib
	$(MAKE_MINICARGO) $@
output$(OUTDIR_SUF)/rustc output$(OUTDIR_SUF)/cargo: output$(OUTDIR_SUF)/libtest.rlib
	$(MAKE_MINICARGO) $@

TEST_DEPS := output$(OUTDIR_SUF)/libstd.rlib output$(OUTDIR_SUF)/libtest.rlib output$(OUTDIR_SUF)/libpanic_unwind.rlib output$(OUTDIR_SUF)/libproc_macro.rlib

fcn_extcrate = $(patsubst %,output$(OUTDIR_SUF)/lib%.rlib,$(1))

fn_getdeps = \
  $(shell cat $1 \
  | sed -n 's/.*extern crate \([a-zA-Z_0-9][a-zA-Z_0-9]*\)\( as .*\)\{0,1\};.*/\1/p' \
  | tr '\n' ' ')


.PHONY: RUSTCSRC
RUSTCSRC: $(RUSTC_SRC_DL)

#
# rustc (with std/cargo) source download
#
# NIGHTLY:
ifeq ($(RUSTC_SRC_TY),nightly)
rustc-nightly-src.tar.gz: $(RUSTC_SRC_DES)
	@export DL_RUST_DATE=$$(cat rust-nightly-date); \
	export DISK_RUST_DATE=$$([ -f $(RUSTC_SRC_DL) ] && cat $(RUSTC_SRC_DL)); \
	echo "Rust version on disk is '$${DISK_RUST_DATE}'. Downloading $${DL_RUST_DATE}."; \
	rm -f rustc-nightly-src.tar.gz; \
	curl -sS https://static.rust-lang.org/dist/$${DL_RUST_DATE}/rustc-nightly-src.tar.gz -o rustc-nightly-src.tar.gz

$(RUSTC_SRC_DL): rust-nightly-date rustc-nightly-src.tar.gz rustc-nightly-src.patch
	@export DL_RUST_DATE=$$(cat rust-nightly-date); \
	export DISK_RUST_DATE=$$([ -f $(RUSTC_SRC_DL) ] && cat $(RUSTC_SRC_DL)); \
	if [ "$$DL_RUST_DATE" != "$$DISK_RUST_DATE" ]; then \
		rm -rf rustc-nightly-src; \
		tar -xf rustc-nightly-src.tar.gz; \
		cd $(RUSTCSRC) && patch -p0 < ../rustc-nightly-src.patch; \
	fi
	cat rust-nightly-date > $(RUSTC_SRC_DL)
else
# NAMED (Stable or beta)
RUSTC_SRC_TARBALL := rustc-$(RUSTC_VERSION)-src.tar.gz
$(RUSTC_SRC_TARBALL): $(RUSTC_SRC_DES)
	@echo [CURL] $@
	@rm -f $@
	@curl -sS https://static.rust-lang.org/dist/$@ -o $@
$(RUSTC_SRC_DL): $(RUSTC_SRC_TARBALL) rustc-$(RUSTC_VERSION)-src.patch
	tar -xf $(RUSTC_SRC_TARBALL)
	cd $(RUSTCSRC) && patch -p0 < ../rustc-$(RUSTC_VERSION)-src.patch;
	cat $(RUSTC_SRC_DES) > $(RUSTC_SRC_DL)
endif


# MRUSTC-specific tests
.PHONY: local_tests
local_tests: $(TEST_DEPS)
	@$(MAKE) -C tools/testrunner
	@mkdir -p output$(OUTDIR_SUF)/local_tests
	./bin/testrunner -o output$(OUTDIR_SUF)/local_tests -L output$(OUTDIR_SUF) samples/test

# 
# RUSTC TESTS
# 
.PHONY: rust_tests local_tests
RUST_TESTS_DIR := $(RUSTCSRC)src/test/
rust_tests: RUST_TESTS_run-pass
# rust_tests-run-fail
# rust_tests-compile-fail

.PHONY: RUST_TESTS RUST_TESTS_run-pass
RUST_TESTS: RUST_TESTS_run-pass
RUST_TESTS_run-pass: output$(OUTDIR_SUF)/test/librust_test_helpers.a
	@$(MAKE) -C tools/testrunner
	@mkdir -p output$(OUTDIR_SUF)/rust_tests/run-pass
	$(MAKE) -f minicargo.mk output$(OUTDIR_SUF)/test/libtest.so
	./bin/testrunner -L output$(OUTDIR_SUF)/test -o output$(OUTDIR_SUF)/rust_tests/run-pass $(RUST_TESTS_DIR)run-pass --exceptions disabled_tests_run-pass.txt
output$(OUTDIR_SUF)/test/librust_test_helpers.a: output$(OUTDIR_SUF)/test/rust_test_helpers.o
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
# libstd tests
# 
.PHONY: rust_tests-libs
rust_tests-libs: $(TEST_DEPS)
	$(MAKE) -f minicargo.mk $@


.PHONY: test
#
# TEST: Rust standard library and the "hello, world" run-pass test
#
test: output$(OUTDIR_SUF)/rust/test_run-pass_hello_out.txt

HELLO_TEST := ui/hello.rs
ifeq ($(RUSTC_VERSION),1.19.0)
  HELLO_TEST := run-pass/hello.rs
else ifeq ($(RUSTC_VERSION),1.29.0)
  HELLO_TEST := run-pass/hello.rs
endif

# "hello, world" test - Invoked by the `make test` target
output$(OUTDIR_SUF)/rust/test_run-pass_hello: $(RUST_TESTS_DIR)$(HELLO_TEST) $(TEST_DEPS)
	@mkdir -p $(dir $@)
	@echo "--- [MRUSTC] -o $@"
	$(DBG) $(BIN) $< -o $@ $(RUST_FLAGS) $(PIPECMD)
output$(OUTDIR_SUF)/rust/test_run-pass_hello_out.txt: output$(OUTDIR_SUF)/rust/test_run-pass_hello
	@echo "--- [$<]"
	@./$< | tee $@


# -------------------------------
# Compile rules for mrustc itself
# -------------------------------
bin/mrustc.a: $(filter-out $(OBJDIR)main.o, $(OBJ))
	@+mkdir -p $(dir $@)
	@echo [AR] -o $@
ifeq ($(shell uname -s || echo not),Darwin)
# We can use llvm-ar for having rcD available on Darwin.
# However, that is not bundled as a part of the operating system.
	$Var rc $@ $(filter-out $(OBJDIR)main.o, $(OBJ))
else
	$Var rcD $@ $(filter-out $(OBJDIR)main.o, $(OBJ))
endif

$(BIN): $(OBJDIR)main.o bin/mrustc.a bin/common_lib.a
	@+mkdir -p $(dir $@)
	@echo [CXX] -o $@
ifeq ($(OS),Windows_NT)
	$V$(CXX) -o $@ $(LINKFLAGS) $(OBJDIR)main.o -Wl,--whole-archive bin/mrustc.a bin/common_lib.a -Wl,--no-whole-archive $(LIBS)
else ifeq ($(shell uname -s || echo not),Darwin)
	$V$(CXX) -o $@ $(LINKFLAGS) $(OBJDIR)main.o -Wl,-all_load bin/mrustc.a bin/common_lib.a $(LIBS)
else
	$V$(CXX) -o $@ $(LINKFLAGS) $(OBJDIR)main.o -Wl,--whole-archive bin/mrustc.a -Wl,--no-whole-archive bin/common_lib.a $(LIBS)
	objcopy --only-keep-debug $(BIN) $(BIN).debug
	objcopy --add-gnu-debuglink=$(BIN).debug $(BIN)
	strip $(BIN)
endif

$(OBJDIR)%.o: src/%.cpp
	@+mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.dep
$(OBJDIR)version.o: $(OBJDIR)%.o: src/%.cpp $(filter-out $(OBJDIR)version.o,$(OBJ)) Makefile
	@+mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.dep -D VERSION_GIT_FULLHASH=\"$(shell git show --pretty=%H -s)\" -D VERSION_GIT_BRANCH="\"$(shell git symbolic-ref -q --short HEAD || git describe --tags --exact-match)\"" -D VERSION_GIT_SHORTHASH=\"$(shell git show -s --pretty=%h)\" -D VERSION_BUILDTIME="\"$(shell date -uR)\"" -D VERSION_GIT_ISDIRTY=$(shell git diff-index --quiet HEAD; echo $$?)

src/main.cpp: $(PCHS:%=src/%.gch)

%.hpp.gch: %.hpp
	@echo [CXX] -o $@
	$V$(CXX) -std=c++14 -o $@ $< $(CPPFLAGS) -MMD -MP -MF $@.dep

bin/common_lib.a:
	$(MAKE) -C tools/common
	
-include $(OBJ:%=%.dep)

# vim: noexpandtab ts=4

