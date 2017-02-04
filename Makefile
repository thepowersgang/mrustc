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

EXESUF ?=
CXX ?= g++
V ?= @

TARGET_CC ?= clang

TAIL_COUNT ?= 45

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
# - Only turn on -Werror when running as `tpg` (i.e. me)
ifeq ($(shell whoami),tpg)
  CXXFLAGS += -Werror
endif
CXXFLAGS += -std=c++14
#CXXFLAGS += -Wextra
CXXFLAGS += -O2
CPPFLAGS := -I src/include/ -I src/

CXXFLAGS += -Wno-pessimizing-move
CXXFLAGS += -Wno-misleading-indentation
#CXXFLAGS += -Wno-unused-private-field

SHELL = bash

ifeq ($(DBGTPL),)
else ifeq ($(DBGTPL),gdb)
  DBG := echo -e "r\nbt 12\nq" | gdb --args
else ifeq ($(DBGTPL),valgrind)
  DBG := valgrind --leak-check=full --num-callers=35
else ifeq ($(DBGTPL),time)
  DBG := time
else
  $(error "Unknown debug template")
endif

OBJDIR = .obj/

BIN := bin/mrustc$(EXESUF)

OBJ := main.o serialise.o
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
OBJ += macro_rules/mod.o macro_rules/eval.o macro_rules/parse.o
OBJ += resolve/use.o resolve/index.o resolve/absolute.o
OBJ += hir/from_ast.o hir/from_ast_expr.o
OBJ +=  hir/dump.o
OBJ +=  hir/hir.o hir/generic_params.o
OBJ +=  hir/crate_ptr.o hir/type_ptr.o hir/expr_ptr.o
OBJ +=  hir/type.o hir/path.o hir/expr.o hir/pattern.o
OBJ +=  hir/visitor.o hir/crate_post_load.o
OBJ += hir_conv/expand_type.o hir_conv/constant_evaluation.o hir_conv/resolve_ufcs.o hir_conv/bind.o hir_conv/markings.o
OBJ += hir_typeck/outer.o hir_typeck/common.o hir_typeck/helpers.o hir_typeck/static.o hir_typeck/impl_ref.o
OBJ += hir_typeck/expr_visit.o
OBJ += hir_typeck/expr_cs.o
OBJ += hir_typeck/expr_check.o
OBJ += hir_expand/annotate_value_usage.o hir_expand/closures.o hir_expand/ufcs_everything.o
OBJ += hir_expand/reborrow.o hir_expand/erased_types.o hir_expand/vtable.o
OBJ += hir_expand/const_eval_full.o
OBJ += mir/mir.o mir/mir_ptr.o
OBJ +=  mir/dump.o mir/helpers.o mir/visit_crate_mir.o
OBJ +=  mir/from_hir.o mir/from_hir_match.o mir/mir_builder.o
OBJ +=  mir/check.o mir/cleanup.o mir/optimise.o
OBJ += hir/serialise.o hir/deserialise.o hir/serialise_lowlevel.o
OBJ += trans/trans_list.o trans/mangling.o
OBJ += trans/enumerate.o trans/monomorphise.o trans/codegen.o trans/codegen_c.o

PCHS := ast/ast.hpp

OBJ := $(addprefix $(OBJDIR),$(OBJ))


all: $(BIN)

clean:
	$(RM) -r $(BIN) $(OBJ)


PIPECMD ?= 2>&1 | tee $@_dbg.txt | tail -n $(TAIL_COUNT) ; test $${PIPESTATUS[0]} -eq 0

output/%.ast: samples/%.rs $(BIN)
	@mkdir -p output/
	$(DBG) $(BIN) $< -o $@ $(PIPECMD)

RUSTCSRC := rustc-nightly/
RUSTC_SRC_DL := $(RUSTCSRC)/dl-version

output/lib%.hir: $(RUSTCSRC)src/lib%/lib.rs $(RUSTCSRC) $(BIN)
	@echo "--- [MRUSTC] $@"
	@mkdir -p output/
	@rm -f $@
	$(DBG) $(ENV_$@) $(BIN) $< -o $@ $(ARGS_$@) $(PIPECMD)
#	# HACK: Work around gdb returning success even if the program crashed
	@test -e $@
output/lib%.hir: $(RUSTCSRC)src/lib%/src/lib.rs $(RUSTCSRC) $(BIN)
	@echo "--- [MRUSTC] $@"
	@mkdir -p output/
	@rm -f $@
	$(DBG) $(BIN) $< -o $@ $(PIPECMD)
#	# HACK: Work around gdb returning success even if the program crashed
	@test -e $@

fcn_extcrate = $(patsubst %,output/lib%.hir,$(1))

fn_getdeps = \
  $(shell cat $1 \
  | sed -n 's/.*extern crate \([a-zA-Z_0-9][a-zA-Z_0-9]*\)\( as .*\)\{0,1\};.*/\1/p' \
  | tr '\n' ' ')


# --- rustc: librustc_llvm ---
RUSTC_TARGET := x86_64-unknown-linux-gnu
LLVM_LINKAGE_FILE := $(abspath rustc-nightly/$(RUSTC_TARGET)/rt/llvmdeps.rs)
output/librustc_llvm.hir: $(LLVM_LINKAGE_FILE)

ifeq ($(USE_BUILD_SCRIPT),yes)
output/librustc_llvm_build: rustc-nightly/src/librustc_llvm/build.rs output/libstd.hir output/libgcc.hir output/libbuild_helper.hir
	@echo "--- [MRUSTC] $@"
	$(BIN) $< -o $@ $(PIPECMD)
output/libgcc.hir: crates.io/gcc-0.3.28/src/lib.rs $(BIN) output/libstd.hir
	@echo "--- [MRUSTC] $@"
	$(BIN) $< -o $@ --crate-type rlib --crate-name gcc $(PIPECMD)
output/libbuild_helper.hir: rustc-nightly/src/build_helper/lib.rs $(BIN) output/libstd.hir
	@echo "--- [MRUSTC] $@"
	$(BIN) $< -o $@ --crate-type rlib --crate-name build_helper $(PIPECMD)

crates.io/%/src/lib.rs: crates.io/%.tar.gz
	tar -xf $< -C crates.io/
crates.io/gcc-0.3.28.tar.gz:
	@mkdir -p $(dir $@)
	curl -s https://crates.io/api/v1/crates/gcc/0.3.28/download -o $@

$(LLVM_LINKAGE_FILE): output/librustc_llvm_build
	@mkdir -p rustc-nightly/$(RUSTC_TARGET)/cargo_out
	cd rustc-nightly/src/librustc_llvm && (export OUT_DIR=rustc-nightly/$(RUSTC_TARGET)/cargo_out OPT_LEVEL=1 PROFILE=release TARGET=$(RUSTC_TARGET) HOST=$(shell $(CC) --verbose 2>&1 | grep 'Target' | awk '{print $$2}') ; $(DBGTPL) ../../../output/librustc_llvm_build)
else
$(LLVM_LINKAGE_FILE):
	mkdir -p $(dir $@)
	echo > $@
endif

ARGS_output/librustc_llvm.hir := --cfg llvm_component=x86
ENV_output/librustc_llvm.hir := CFG_LLVM_LINKAGE_FILE=$(LLVM_LINKAGE_FILE)

output/libarena.hir: output/libstd.hir
output/liballoc.hir: output/libcore.hir
output/libstd_unicode.hir: output/libcore.hir
output/libcollections.hir: output/libcore.hir output/liballoc.hir output/libstd_unicode.hir
output/librand.hir: output/libcore.hir
output/liblibc.hir: output/libcore.hir
output/libcompiler_builtins.hir: output/libcore.hir
output/libstd.hir: $(call fcn_extcrate, core collections rand libc unwind compiler_builtins)
output/libunwind.hir: $(call fcn_extcrate, core libc)

output/libterm.hir: $(call fcn_extcrate, std)
output/libpanic_unwind.hir: $(call fcn_extcrate, core alloc libc unwind)
output/libpanic_abort.hir: $(call fcn_extcrate, core $(call fn_getdeps, $(RUSTCSRC)src/libpanic_abort/lib.rs))
output/libtest.hir: $(call fcn_extcrate, std getopts term panic_unwind)
output/libgetopts.hir: output/libstd.hir
output/libflate.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/libflate/lib.rs))
output/liblog.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/libflate/lib.rs))

output/liballoc_system.hir: $(call fcn_extcrate, core libc)
output/liballoc_jemalloc.hir: $(call fcn_extcrate, core libc)

output/libserialize.hir: $(call fcn_extcrate, std log rustc_i128)
output/librustc_llvm.hir: $(call fcn_extcrate, std rustc_bitflags)
output/librustc_errors.hir: $(call fcn_extcrate, std syntax_pos term)
output/libsyntax.hir: $(call fcn_extcrate, std core serialize term libc log rustc_bitflags rustc_errors syntax_pos rustc_data_structures)
output/librustc_back.hir: $(call fcn_extcrate, std syntax)
output/librustc_data_structures.hir: $(call fcn_extcrate, std log serialize libc)
output/librustc_const_math.hir: $(call fcn_extcrate, std log syntax serialize)
output/libfmt_macros.hir: $(call fcn_extcrate, std)
output/libproc_macro.hir: $(call fcn_extcrate, std syntax)
output/libsyntax_ext.hir: $(call fcn_extcrate, std fmt_macros log syntax syntax_pos proc_macro rustc_errors)
output/librustc_metadata.hir: $(call fcn_extcrate, std log syntax syntax_pos flate serialize rustc_errors syntax_ext proc_macro rustc rustc_back rustc_const_math rustc_data_structures rustc_llvm)
output/librustc_borrowck.hir: $(call fcn_extcrate, std log syntax syntax_pos rustc_errors graphviz rustc rustc_data_structures rustc_mir core)
output/librustc_mir.hir: $(call fcn_extcrate, std log graphviz rustc rustc_data_structures rustc_back rustc_bitflags syntax syntax_pos rustc_const_math rustc_const_eval)
output/librustc_const_eval.hir: $(call fcn_extcrate, std arena syntax log rustc rustc_back rustc_const_math rustc_data_structures rustc_errors graphviz syntax_pos serialize)
output/libgraphviz.hir: $(call fcn_extcrate, std)

output/libsyntax_pos.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/libsyntax_pos/lib.rs))

output/librustc_i128.hir: output/libcore.hir
output/librustc_plugin.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc_plugin/lib.rs))
output/librustc_save_analysis.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc_save_analysis/lib.rs))
output/librustc_resolve.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc_resolve/lib.rs))
output/librustc_plugin.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc_plugin/lib.rs))
output/librustc.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc/lib.rs))
output/librustc_trans.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc_trans/lib.rs))
output/librustc_lint.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc_lint/lib.rs))
output/librustc_passes.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc_passes/lib.rs))
output/librustc_incremental.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc_incremental/lib.rs))
output/librustc_typeck.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc_trans/lib.rs))
output/librustc_driver.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc_driver/lib.rs))
output/librustc_bitflags.hir: $(call fcn_extcrate, core $(call fn_getdeps, $(RUSTCSRC)src/librustc_bitflags/lib.rs))
output/librustc_privacy.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc_privacy/lib.rs))
output/librustc_platform_intrinsics.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/librustc_platform_intrinsics/lib.rs))

output/rustc: $(RUSTCSRC)src/rustc/rustc.rs output/librustc.hir output/librustc_driver.hir
	@echo "--- [MRUSTC] $@"
	@mkdir -p output/
	@rm -f $@
	$(DBG) $(BIN) $< -o $@ $(PIPECMD)
#	# HACK: Work around gdb returning success even if the program crashed
	@test -e $@

$(RUSTCSRC): rust-nightly-date
	@export DL_RUST_DATE=$$(cat rust-nightly-date); \
	export DISK_RUST_DATE=$$([ -f $(RUSTC_SRC_DL) ] && cat $(RUSTC_SRC_DL)); \
	if [ "$$DL_RUST_DATE" != "$$DISK_RUST_DATE" ]; then \
		echo "Rust version on disk is '$${DISK_RUST_DATE}'. Downloading $${DL_RUST_DATE}."; \
		rm -f rustc-nightly-src.tar.gz; \
		rm -rf rustc-nightly; \
		curl -s https://static.rust-lang.org/dist/$${DL_RUST_DATE}/rustc-nightly-src.tar.gz -o rustc-nightly-src.tar.gz; \
		tar -xf rustc-nightly-src.tar.gz --transform 's~^rustc-nightly-src~rustc-nightly~'; \
		echo "$$DL_RUST_DATE" > $(RUSTC_SRC_DL); \
	fi


# 
# RUSTC TESTS
# 
.PHONY: rust_tests
RUST_TESTS_DIR := $(RUSTCSRC)src/test/
rust_tests: rust_tests-run-pass rust_tests-run-fail
# rust_tests-compile-fail

# - Require external symbols that aren't generated.
DISABLED_TESTS = run-pass/abi-sysv64-arg-passing run-pass/abi-sysv64-register-usage run-pass/anon-extern-mod run-pass/anon-extern-mod-cross-crate-2
# - NOT A TEST
DISABLED_TESTS += run-pass/backtrace-debuginfo-aux
# - asm! is hard to trnaslate
DISABLED_TESTS += run-pass/asm-in-out-operand run-pass/asm-indirect-memory run-pass/asm-out-assign
# - Requires jemalloc
DISABLED_TESTS += run-pass/allocator-default run-pass/allocator-override
# - Bug in inferrence order.
DISABLED_TESTS += run-pass/associated-types-conditional-dispatch
# - Lazy.
DISABLED_TESTS += run-pass/associated-types-projection-in-where-clause run-pass/autoderef-privacy
# - Line information that isn't avaliable due to codegen
DISABLED_TESTS += run-pass/backtrace-debuginfo run-pass/backtrace
# - No unwind catching support
DISABLED_TESTS += run-pass/binary-heap-panic-safe run-pass/box-of-array-of-drop-1 run-pass/box-of-array-of-drop-2

DEF_RUST_TESTS = $(sort $(patsubst $(RUST_TESTS_DIR)%.rs,output/rust/%_out.txt,$(wildcard $(RUST_TESTS_DIR)$1/*.rs)))
rust_tests-run-pass: $(filter-out $(patsubst %,output/rust/%_out.txt,$(DISABLED_TESTS)), $(call DEF_RUST_TESTS,run-pass))
rust_tests-run-fail: $(call DEF_RUST_TESTS,run-fail)
#rust_tests-compile-fail: $(call DEF_RUST_TESTS,compile-fail)

output/rust/test_run-pass_hello: $(RUST_TESTS_DIR)run-pass/hello.rs output/libstd.hir $(BIN) output/liballoc_system.hir output/libpanic_abort.hir
	@mkdir -p $(dir $@)
	@echo "--- [MRUSTC] -o $@"
	$(DBG) $(BIN) $< -o $@ $(PIPECMD)
	@echo "--- [$@]"
	@./$@

TEST_ARGS_run-pass/cfgs-on-items := --cfg fooA --cfg fooB

output/rust/%: $(RUST_TESTS_DIR)%.rs $(RUSTCSRC) $(BIN) output/libstd.hir output/libtest.hir
	@mkdir -p $(dir $@)
	@echo "=== TEST $(patsubst output/rust/%,%,$@)"
	@echo "--- [MRUSTC] -o $@"
	$V$(BIN) $< -o $@ --stop-after $(RUST_TESTS_FINAL_STAGE) $(TEST_ARGS_$*) > $@.txt 2>&1 || (tail -n 1 $@.txt; false)
output/rust/%_out.txt: output/rust/%
	@echo "--- [$<]"
	@./$< > $@ || (tail -n 1 $@; false)

output/rust/run-pass/allocator-default.o: output/libstd.hir output/liballoc_jemalloc.hir
output/rust/run-pass/allocator-system.o: output/liballoc_system.hir
output/rust/run-pass/anon-extern-mod-cross-crate-2.o: output/test_deps/libanonexternmod.hir
output/test_deps/libsvh_b.hir: output/test_deps/libsvh_a_base.hir

output/test_deps/libanonexternmod.hir: $(RUST_TESTS_DIR)run-pass/auxiliary/anon-extern-mod-cross-crate-1.rs
	$(BIN) $< --crate-type rlib --out-dir output/test_deps > $@.txt 2>&1
output/test_deps/libunion.hir: $(RUST_TESTS_DIR)run-pass/union/auxiliary/union.rs
	mkdir -p $(dir $@)
	$(BIN) $< --crate-type rlib --out-dir output/test_deps > $@.txt 2>&1

test_deps_run-pass.mk: Makefile $(wildcard $(RUST_TESTS_DIR)run_pass/*.rs)
	@echo "--- Generating test dependencies: $@"
	@grep 'aux-build:' rustc-nightly/src/test/run-pass/{*.rs,union/*.rs} | awk -F : '{a=gensub(/.+run-pass\/(.*)\.rs$$/, "\\1", "g", $$1); b=gensub(/(.*)\.rs/,"\\1","g",$$3); gsub(/-/,"_",b); print "output/rust/run-pass/" a ": " "output/test_deps/lib" b ".hir" }' > $@.tmp
	@grep 'aux-build:' rustc-nightly/src/test/run-pass/*.rs | awk -F : '{ print $$3 }' | sort | uniq | awk '{ b=gensub(/(.*)\.rs/,"\\1","g",$$1); gsub(/-/,"_",b); print "output/test_deps/lib" b ".hir: $$(RUST_TESTS_DIR)run-pass/auxiliary/" $$1 " output/libstd.hir" ; print "\t@mkdir -p $$(dir $$@)" ; print "\t@echo \"--- [MRUSTC] $$@\"" ; print "\t@$$(DBG) $$(BIN) $$< --crate-type rlib --out-dir output/test_deps > $$@.txt 2>&1" ; print "\t@touch $$@" }' >> $@.tmp
	@mv $@.tmp $@


-include test_deps_run-pass.mk


.PHONY: test test_rustos
#
# TEST: Rust standard library and the "hello, world" run-pass test
#
test: $(RUSTCSRC) output/libcore.hir output/liballoc.hir output/libcollections.hir output/libstd.hir output/rust/test_run-pass_hello $(BIN)

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
	export $(RUSTOS_ENV) ; $(DBG) $(BIN) $< -o $@ --cfg arch=amd64 $(PIPECMD)
output/libstack_dst.hir: ../rust_os/externals/crates.io/stack_dst/src/lib.rs $(BIN)
	@mkdir -p $(dir $@)
	$(DBG) $(BIN) $< -o $@ --cfg feature=no_std $(PIPECMD)


# -------------------------------
# Compile rules for mrustc itself
# -------------------------------
$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ $(LINKFLAGS) $(OBJ) $(LIBS)

$(OBJDIR)%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.dep

src/main.cpp: $(PCHS:%=src/%.gch)

%.hpp.gch: %.hpp
	@echo [CXX] -o $@
	$V$(CXX) -std=c++14 -o $@ $< $(CPPFLAGS) -MMD -MP -MF $@.dep

-include $(OBJ:%=%.dep)

# vim: noexpandtab ts=4

