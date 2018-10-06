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
V ?= @

TARGET_CC ?= clang

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

CPPFLAGS := -I src/include/ -I src/
CPPFLAGS += -I tools/common/

CXXFLAGS += -Wno-pessimizing-move
CXXFLAGS += -Wno-misleading-indentation
#CXXFLAGS += -Wno-unused-private-field
CXXFLAGS += -Wno-unknown-warning-option


# - Flags to pass to all mrustc invocations
RUST_FLAGS := --cfg debug_assertions
RUST_FLAGS += -g
RUST_FLAGS += -O
RUST_FLAGS += -L output/
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
OBJ += expand/test_harness.o
OBJ += macro_rules/mod.o macro_rules/eval.o macro_rules/parse.o
OBJ += resolve/use.o resolve/index.o resolve/absolute.o
OBJ += hir/from_ast.o hir/from_ast_expr.o
OBJ +=  hir/dump.o
OBJ +=  hir/hir.o hir/generic_params.o
OBJ +=  hir/crate_ptr.o hir/expr_ptr.o
OBJ +=  hir/type.o hir/path.o hir/expr.o hir/pattern.o
OBJ +=  hir/visitor.o hir/crate_post_load.o
OBJ += hir_conv/expand_type.o hir_conv/constant_evaluation.o hir_conv/resolve_ufcs.o hir_conv/bind.o hir_conv/markings.o
OBJ += hir_typeck/outer.o hir_typeck/common.o hir_typeck/helpers.o hir_typeck/static.o hir_typeck/impl_ref.o
OBJ += hir_typeck/expr_visit.o
OBJ += hir_typeck/expr_cs.o
OBJ += hir_typeck/expr_check.o
OBJ += hir_expand/annotate_value_usage.o hir_expand/closures.o
OBJ += hir_expand/ufcs_everything.o
OBJ += hir_expand/reborrow.o hir_expand/erased_types.o hir_expand/vtable.o
OBJ += mir/mir.o mir/mir_ptr.o
OBJ +=  mir/dump.o mir/helpers.o mir/visit_crate_mir.o
OBJ +=  mir/from_hir.o mir/from_hir_match.o mir/mir_builder.o
OBJ +=  mir/check.o mir/cleanup.o mir/optimise.o
OBJ +=  mir/check_full.o
OBJ += hir/serialise.o hir/deserialise.o hir/serialise_lowlevel.o
OBJ += trans/trans_list.o trans/mangling.o
OBJ += trans/enumerate.o trans/monomorphise.o trans/codegen.o
OBJ += trans/codegen_c.o trans/codegen_c_structured.o trans/codegen_mmir.o
OBJ += trans/target.o trans/allocator.o

PCHS := ast/ast.hpp

OBJ := $(addprefix $(OBJDIR),$(OBJ))


all: $(BIN)

clean:
	$(RM) -r $(BIN) $(OBJ)


PIPECMD ?= 2>&1 | tee $@_dbg.txt | tail -n $(TAIL_COUNT) ; test $${PIPESTATUS[0]} -eq 0

#RUSTC_SRC_TY ?= nightly
RUSTC_SRC_TY ?= stable
ifeq ($(RUSTC_SRC_TY),nightly)
RUSTC_SRC_DES := rust-nightly-date
RUSTCSRC := rustc-nightly-src/
else ifeq ($(RUSTC_SRC_TY),stable)
RUSTC_SRC_DES := rust-version
RUSTCSRC := rustc-$(shell cat $(RUSTC_SRC_DES))-src/
else
$(error Unknown rustc channel)
endif
RUSTC_SRC_DL := $(RUSTCSRC)/dl-version

MAKE_MINICARGO = $(MAKE) -f minicargo.mk RUSTC_VERSION=$(shell cat $(RUSTC_SRC_DES)) RUSTC_CHANNEL=$(RUSTC_SRC_TY)


output/libstd.hir: $(BIN)
	$(MAKE_MINICARGO) $@
output/libtest.hir output/libpanic_unwind.hir output/libproc_macro.hir: output/libstd.hir
	$(MAKE_MINICARGO) $@
output/rustc output/cargo: output/libtest.hir
	$(MAKE_MINICARGO) $@

TEST_DEPS := output/libstd.hir output/libtest.hir output/libpanic_unwind.hir output/librust_test_helpers.a

fcn_extcrate = $(patsubst %,output/lib%.hir,$(1))

fn_getdeps = \
  $(shell cat $1 \
  | sed -n 's/.*extern crate \([a-zA-Z_0-9][a-zA-Z_0-9]*\)\( as .*\)\{0,1\};.*/\1/p' \
  | tr '\n' ' ')


.PHONY: RUSTCSRC
RUSTCSRC: $(RUSTC_SRC_DL)

ifeq ($(RUSTC_SRC_TY),nightly)
rustc-nightly-src.tar.gz: $(RUSTC_SRC_DES)
	@export DL_RUST_DATE=$$(cat rust-nightly-date); \
	export DISK_RUST_DATE=$$([ -f $(RUSTC_SRC_DL) ] && cat $(RUSTC_SRC_DL)); \
	echo "Rust version on disk is '$${DISK_RUST_DATE}'. Downloading $${DL_RUST_DATE}."; \
	rm -f rustc-nightly-src.tar.gz; \
	curl -sS https://static.rust-lang.org/dist/$${DL_RUST_DATE}/rustc-nightly-src.tar.gz -o rustc-nightly-src.tar.gz

# TODO: Handle non-nightly download
$(RUSTC_SRC_DL): rust-nightly-date rustc-nightly-src.tar.gz rust_src.patch
	@export DL_RUST_DATE=$$(cat rust-nightly-date); \
	export DISK_RUST_DATE=$$([ -f $(RUSTC_SRC_DL) ] && cat $(RUSTC_SRC_DL)); \
	if [ "$$DL_RUST_DATE" != "$$DISK_RUST_DATE" ]; then \
		rm -rf rustc-nightly-src; \
		tar -xf rustc-nightly-src.tar.gz; \
		cd $(RUSTSRC) && patch -p0 < ../rust_src.patch; \
	fi
	cat rust-nightly-date > $(RUSTC_SRC_DL)
else
RUSTC_SRC_TARBALL := rustc-$(shell cat $(RUSTC_SRC_DES))-src.tar.gz
$(RUSTC_SRC_TARBALL): $(RUSTC_SRC_DES)
	@echo [CURL] $@
	@rm -f $@
	@curl -sS https://static.rust-lang.org/dist/$@ -o $@
$(RUSTC_SRC_DL): $(RUSTC_SRC_TARBALL) rust_src.patch
	tar -xf $(RUSTC_SRC_TARBALL)
	cd $(RUSTCSRC) && patch -p0 < ../rust_src.patch;
	cat $(RUSTC_SRC_DES) > $(RUSTC_SRC_DL)
endif


# MRUSTC-specific tests
.PHONY: local_tests
local_tests:
	@$(MAKE) -C tools/testrunner
	@mkdir -p output/local_tests
	./tools/bin/testrunner -o output/local_tests samples/test

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
RUST_TESTS_run-pass: output/librust_test_helpers.a
	@$(MAKE) -C tools/testrunner
	@mkdir -p output/rust_tests/run-pass
	./tools/bin/testrunner -o output/rust_tests/run-pass $(RUST_TESTS_DIR)run-pass --exceptions disabled_tests_run-pass.txt
output/librust_test_helpers.a: output/rust_test_helpers.o
	@mkdir -p $(dir $@)
	ar cur $@ $<
output/rust_test_helpers.o: $(RUSTCSRC)src/rt/rust_test_helpers.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

# 
# libstd tests
# 
.PHONY: rust_tests-libs

LIB_TESTS := collections #std
#LIB_TESTS += rustc_data_structures
rust_tests-libs: $(patsubst %,output/lib%-test_out.txt, $(LIB_TESTS))

RUNTIME_ARGS_output/libcollections-test := --test-threads 1
#RUNTIME_ARGS_output/libcore-test := --test-threads 1
RUNTIME_ARGS_output/libstd-test := --test-threads 1
RUNTIME_ARGS_output/libstd-test += --skip ::collections::hash::map::test_map::test_index_nonexistent
RUNTIME_ARGS_output/libstd-test += --skip ::collections::hash::map::test_map::test_drops
RUNTIME_ARGS_output/libstd-test += --skip ::collections::hash::map::test_map::test_placement_drop
RUNTIME_ARGS_output/libstd-test += --skip ::collections::hash::map::test_map::test_placement_panic
RUNTIME_ARGS_output/libstd-test += --skip ::io::stdio::tests::panic_doesnt_poison	# Unbounded execution

output/libcore-test: $(RUSTCSRC)src/libcore/tests/lib.rs $(TEST_DEPS)
	@echo "--- [MRUSTC] --test -o $@"
	@mkdir -p output/
	@rm -f $@
	$(DBG) $(ENV_$@) $(BIN) --test $< -o $@ $(RUST_FLAGS) $(ARGS_$@) $(PIPECMD)
#	# HACK: Work around gdb returning success even if the program crashed
	@test -e $@
output/lib%-test: $(RUSTCSRC)src/lib%/lib.rs $(TEST_DEPS)
	@echo "--- [MRUSTC] --test -o $@"
	@mkdir -p output/
	@rm -f $@
	$(DBG) $(ENV_$@) $(BIN) --test $< -o $@ $(RUST_FLAGS) $(ARGS_$@) $(PIPECMD)
#	# HACK: Work around gdb returning success even if the program crashed
	@test -e $@
output/lib%-test: $(RUSTCSRC)src/lib%/src/lib.rs $(TEST_DEPS)
	@echo "--- [MRUSTC] $@"
	@mkdir -p output/
	@rm -f $@
	$(DBG) $(ENV_$@) $(BIN) --test $< -o $@ $(RUST_FLAGS) $(ARGS_$@) $(PIPECMD)
#	# HACK: Work around gdb returning success even if the program crashed
	@test -e $@
output/%_out.txt: output/%
	@echo "--- [$<]"
	$V./$< $(RUNTIME_ARGS_$<) > $@ || (tail -n 1 $@; mv $@ $@_fail; false)

# "hello, world" test - Invoked by the `make test` target
output/rust/test_run-pass_hello: $(RUST_TESTS_DIR)run-pass/hello.rs $(TEST_DEPS)
	@mkdir -p $(dir $@)
	@echo "--- [MRUSTC] -o $@"
	$(DBG) $(BIN) $< -o $@ $(RUST_FLAGS) $(PIPECMD)
output/rust/test_run-pass_hello_out.txt: output/rust/test_run-pass_hello
	@echo "--- [$<]"
	@./$< | tee $@



.PHONY: test test_rustos
#
# TEST: Rust standard library and the "hello, world" run-pass test
#
test: output/libstd.hir output/rust/test_run-pass_hello_out.txt $(BIN) TEST_targetsaveback

.PHONY: TEST_targetsaveback
TEST_targetsaveback:
	$(BIN) --target ./samples/target_stress_test.toml --dump-target-spec TMP-dump-target_stress_test.toml
	$(BIN) --target ./TMP-dump-target_stress_test.toml --dump-target-spec TMP-dump-target_stress_test-2.toml
	diff ./samples/target_stress_test.toml TMP-dump-target_stress_test.toml
	diff TMP-dump-target_stress_test.toml TMP-dump-target_stress_test-2.toml

#
# TEST: Attempt to compile rust_os (Tifflin) from ../rust_os
#
test_rustos: $(addprefix output/rust_os/,libkernel.hir)

RUSTOS_ENV := RUST_VERSION="mrustc 0.1"
RUSTOS_ENV += TK_GITSPEC="unknown"
RUSTOS_ENV += TK_VERSION="0.1"
RUSTOS_ENV += TK_BUILD="mrustc:0"

output/rust_os/libkernel.hir: ../rust_os/Kernel/Core/main.rs output/libcore.hir output/libstack_dst.hir $(BIN)
	@mkdir -p $(dir $@)
	export $(RUSTOS_ENV) ; $(DBG) $(BIN) $(RUST_FLAGS) $< -o $@ --cfg arch=amd64 $(PIPECMD)
output/libstack_dst.hir: ../rust_os/externals/crates.io/stack_dst/src/lib.rs $(BIN)
	@mkdir -p $(dir $@)
	$(DBG) $(BIN) $(RUST_FLAGS) $< -o $@ --cfg feature=no_std $(PIPECMD)


# -------------------------------
# Compile rules for mrustc itself
# -------------------------------
$(BIN): $(OBJ) tools/bin/common_lib.a
	@mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ $(LINKFLAGS) $(OBJ) tools/bin/common_lib.a $(LIBS)
ifeq ($(OS),Windows_NT)
else ifeq ($(shell uname -s || echo not),Darwin)
else
	objcopy --only-keep-debug $(BIN) $(BIN).debug
	objcopy --add-gnu-debuglink=$(BIN).debug $(BIN)
	strip $(BIN)
endif

$(OBJDIR)%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.dep
$(OBJDIR)version.o: $(OBJDIR)%.o: src/%.cpp $(filter-out $(OBJDIR)version.o,$(OBJ)) Makefile
	@mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.dep -D VERSION_GIT_FULLHASH=\"$(shell git show --pretty=%H -s)\" -D VERSION_GIT_BRANCH="\"$(shell git symbolic-ref -q --short HEAD || git describe --tags --exact-match)\"" -D VERSION_GIT_SHORTHASH=\"$(shell git show -s --pretty=%h)\" -D VERSION_BUILDTIME="\"$(shell date -uR)\"" -D VERSION_GIT_ISDIRTY=$(shell git diff-index --quiet HEAD; echo $$?)

src/main.cpp: $(PCHS:%=src/%.gch)

%.hpp.gch: %.hpp
	@echo [CXX] -o $@
	$V$(CXX) -std=c++14 -o $@ $< $(CPPFLAGS) -MMD -MP -MF $@.dep

tools/bin/common_lib.a:
	$(MAKE) -C tools/common
	
-include $(OBJ:%=%.dep)

# vim: noexpandtab ts=4

