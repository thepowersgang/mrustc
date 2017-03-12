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


# - Flags to pass to all mrustc invocations
RUST_FLAGS := --cfg debug_assertions
RUST_FLAGS += -g

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
OBJ +=  mir/check_full.o
OBJ += hir/serialise.o hir/deserialise.o hir/serialise_lowlevel.o
OBJ += trans/trans_list.o trans/mangling.o
OBJ += trans/enumerate.o trans/monomorphise.o trans/codegen.o trans/codegen_c.o
OBJ += trans/target.o

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
	$(DBG) $(ENV_$@) $(BIN) $< -o $@ $(RUST_FLAGS) $(ARGS_$@) $(PIPECMD)
#	# HACK: Work around gdb returning success even if the program crashed
	@test -e $@
output/lib%.hir: $(RUSTCSRC)src/lib%/src/lib.rs $(RUSTCSRC) $(BIN)
	@echo "--- [MRUSTC] $@"
	@mkdir -p output/
	@rm -f $@
	$(DBG) $(ENV_$@) $(BIN) $< -o $@ $(RUST_FLAGS) $(ARGS_$@) $(PIPECMD)
#	# HACK: Work around gdb returning success even if the program crashed
	@test -e $@

fcn_extcrate = $(patsubst %,output/lib%.hir,$(1))

fn_getdeps = \
  $(shell cat $1 \
  | sed -n 's/.*extern crate \([a-zA-Z_0-9][a-zA-Z_0-9]*\)\( as .*\)\{0,1\};.*/\1/p' \
  | tr '\n' ' ')


# --- rustc: librustc_llvm ---
RUSTC_TARGET := x86_64-unknown-linux-gnu
RUSTC_HOST := $(shell $(CC) --verbose 2>&1 | grep 'Target' | awk '{print $$2}')
ifeq ($(RUSTC_HOST),x86_64-linux-gnu)
	RUSTC_HOST := x86_64-unknown-linux-gnu
endif
LLVM_LINKAGE_FILE := $(abspath rustc-nightly/$(RUSTC_TARGET)/rt/llvmdeps.rs)
LLVM_CONFIG := $(RUSTCSRC)build/bin/llvm-config

output/librustc_llvm.hir: $(LLVM_LINKAGE_FILE)

#
# librustc_llvm build script
#
RUSTC_LLVM_LINKAGE: $(LLVM_LINKAGE_FILE)
output/librustc_llvm_build: rustc-nightly/src/librustc_llvm/build.rs  $(call fcn_extcrate, std gcc build_helper alloc_system panic_abort)
	@echo "--- [MRUSTC] $@"
	$(BIN) $< -o $@ -L output/libs $(RUST_FLAGS) $(PIPECMD)
output/libgcc.hir: crates.io/gcc-0.3.28/src/lib.rs $(BIN) output/libstd.hir
	@echo "--- [MRUSTC] $@"
	$(BIN) $< -o $@ --crate-type rlib --crate-name gcc $(RUST_FLAGS) $(PIPECMD)
output/libbuild_helper.hir: rustc-nightly/src/build_helper/lib.rs $(BIN) output/libstd.hir
	@echo "--- [MRUSTC] $@"
	$(BIN) $< -o $@ --crate-type rlib --crate-name build_helper $(RUST_FLAGS) $(PIPECMD)

crates.io/%/src/lib.rs: crates.io/%.tar.gz
	tar -xf $< -C crates.io/
	@test -e $@ && touch $@
crates.io/gcc-0.3.28.tar.gz:
	@mkdir -p $(dir $@)
	curl -LsS https://crates.io/api/v1/crates/gcc/0.3.28/download -o $@

output/rustc_link_opts.txt: $(LLVM_LINKAGE_FILE)
	@
$(LLVM_LINKAGE_FILE): output/librustc_llvm_build $(LLVM_CONFIG)
	@mkdir -p $(dir $@)
	@mkdir -p rustc-nightly/$(RUSTC_TARGET)/cargo_out
	@echo "--- [rustc-nightly/src/librustc_llvm]"
	$Vcd rustc-nightly/src/librustc_llvm && (export OUT_DIR=$(abspath rustc-nightly/$(RUSTC_TARGET)/cargo_out) OPT_LEVEL=1 PROFILE=release TARGET=$(RUSTC_TARGET) HOST=$(RUSTC_HOST) LLVM_CONFIG=$(abspath $(LLVM_CONFIG)); $(DBG) ../../../output/librustc_llvm_build > ../../../output/librustc_llvm_build-output.txt)
	$Vcat output/librustc_llvm_build-output.txt | grep '^cargo:' > output/librustc_llvm_build-output_cargo.txt
	$Vcat output/librustc_llvm_build-output_cargo.txt | grep 'cargo:rustc-link-lib=.*=' | grep -v =rustllvm | awk -F = '{ print "-l" $$3 }' > output/rustc_link_opts.txt
	$Vcat output/librustc_llvm_build-output_cargo.txt | grep 'cargo:rustc-link-search=native=' | awk -F = '{ print "-L " $$3 }' >> output/rustc_link_opts.txt
	@touch $@

output/cargo_libflate/libminiz.a: output/libflate_build
	@echo "--- $<"
	$Vmkdir -p $(abspath output/cargo_libflate)
	$Vcd rustc-nightly/src/libflate && (export OUT_DIR=$(abspath output/cargo_libflate) OPT_LEVEL=1 PROFILE=release TARGET=$(RUSTC_TARGET) HOST=$(RUSTC_HOST); $(DBG) ../../../$< > ../../../$<-output.txt)
	$Vcat $<-output.txt | grep '^cargo:' > $<-output_cargo.txt
	$Vcat $<-output_cargo.txt | grep 'cargo:rustc-link-search=native=' | awk -F = '{ print "-L " $$3 }' > output/rustc_link_opts-libflate.txt

output/libflate_build: rustc-nightly/src/libflate/build.rs $(call fcn_extcrate, std gcc alloc_system panic_abort)
	@echo "--- [MRUSTC] $@"
	$(BIN) $< -o $@ -L output/libs $(RUST_FLAGS) $(PIPECMD)

ARGS_output/librustc_llvm.hir := --cfg llvm_component=x86 --cfg cargobuild
ENV_output/librustc_llvm.hir := CFG_LLVM_LINKAGE_FILE=$(LLVM_LINKAGE_FILE)

ENV_output/librustc.hir := CFG_COMPILER_HOST_TRIPLE=$(RUSTC_HOST)

# Optional: linux only
output/libstd.hir: output/libs/libbacktrace.a

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
output/libflate.hir: $(call fcn_extcrate, std $(call fn_getdeps, $(RUSTCSRC)src/libflate/lib.rs)) output/cargo_libflate/libminiz.a
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

output/rustc: $(RUSTCSRC)src/rustc/rustc.rs output/librustc_driver.hir output/rustc_link_opts.txt
	@echo "--- [MRUSTC] $@"
	@mkdir -p output/
	@rm -f $@
	$V$(DBG) $(BIN) $< -o $@ -L output/libs $$(cat output/rustc_link_opts.txt output/rustc_link_opts-libflate.txt) -l stdc++ $(RUST_FLAGS) $(PIPECMD)
#	# HACK: Work around gdb returning success even if the program crashed
	@test -e $@

.PHONY: RUSTCSRC
RUSTCSRC: $(RUSTCSRC)

$(RUSTCSRC): rust-nightly-date
	@export DL_RUST_DATE=$$(cat rust-nightly-date); \
	export DISK_RUST_DATE=$$([ -f $(RUSTC_SRC_DL) ] && cat $(RUSTC_SRC_DL)); \
	if [ "$$DL_RUST_DATE" != "$$DISK_RUST_DATE" ]; then \
		echo "Rust version on disk is '$${DISK_RUST_DATE}'. Downloading $${DL_RUST_DATE}."; \
		rm -f rustc-nightly-src.tar.gz; \
		rm -rf rustc-nightly; \
		curl -sS https://static.rust-lang.org/dist/$${DL_RUST_DATE}/rustc-nightly-src.tar.gz -o rustc-nightly-src.tar.gz; \
		tar -xf rustc-nightly-src.tar.gz --transform 's~^rustc-nightly-src~rustc-nightly~'; \
		echo "$$DL_RUST_DATE" > $(RUSTC_SRC_DL); \
	fi

# - libbacktrace, needed for libstd on linux
output/libs/libbacktrace.a: $(RUSTCSRC)src/libbacktrace/Makefile
	@mkdir -p $(dir $@)
	@cd $(RUSTCSRC)src/libbacktrace && $(MAKE) INCDIR=.
	@cp $(RUSTCSRC)src/libbacktrace/.libs/libbacktrace.a $@
$(RUSTCSRC)src/libbacktrace/Makefile:
	@echo "[configure] $(RUSTCSRC)src/libbacktrace"
	@cd $(RUSTCSRC)src/libbacktrace && ./configure --target=$(RUSTC_HOST) --host=$(RUSTC_HOST) --build=$(RUSTC_HOST)


LLVM_CMAKE_OPTS := LLVM_TARGET_ARCH=$(firstword $(subst -, ,$(RUSTC_TARGET))) LLVM_DEFAULT_TARGET_TRIPLE=$(RUSTC_TARGET)
LLVM_CMAKE_OPTS += LLVM_TARGETS_TO_BUILD=X86#;ARM;AArch64;Mips;PowerPC;SystemZ;JSBackend;MSP430;Sparc;NVPTX
LLVM_CMAKE_OPTS += LLVM_ENABLE_ASSERTIONS=OFF
LLVM_CMAKE_OPTS += LLVM_INCLUDE_EXAMPLES=OFF LLVM_INCLUDE_TESTS=OFF LLVM_INCLUDE_DOCS=OFF
LLVM_CMAKE_OPTS += LLVM_ENABLE_ZLIB=OFF LLVM_ENABLE_TERMINFO=OFF LLVM_ENABLE_LIBEDIT=OFF WITH_POLLY=OFF
LLVM_CMAKE_OPTS += CMAKE_CXX_COMPILER="g++" CMAKE_C_COMPILER="gcc"

$(LLVM_CONFIG): $(RUSTCSRC)build/Makefile
		$Vcd $(RUSTCSRC)build && $(MAKE)
$(RUSTCSRC)build/Makefile: $(RUSTCSRC)src/llvm/CMakeLists.txt
		@mkdir -p $(RUSTCSRC)build
		$Vcd $(RUSTCSRC)build && cmake $(addprefix -D , $(LLVM_CMAKE_OPTS)) ../src/llvm


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
DISABLED_TESTS += run-pass/i128
DISABLED_TESTS += run-pass/issue-14936
# - Requires jemalloc
DISABLED_TESTS += run-pass/allocator-default run-pass/allocator-override
# - Bug in inferrence order.
DISABLED_TESTS += run-pass/associated-types-conditional-dispatch
# - Lazy.
DISABLED_TESTS += run-pass/associated-types-projection-in-where-clause run-pass/autoderef-privacy
DISABLED_TESTS += run-pass/builtin-superkinds-self-type
DISABLED_TESTS += run-pass/byte-literals
DISABLED_TESTS += run-pass/c-stack-as-value run-pass/cabi-int-widening
DISABLED_TESTS += run-pass/cast-rfc0401-vtable-kinds run-pass/cast-rfc0401
DISABLED_TESTS += run-pass/cast-in-array-size
DISABLED_TESTS += run-pass/cast
DISABLED_TESTS += run-pass/cfg-in-crate-1
DISABLED_TESTS += run-pass/coerce-expect-unsized
DISABLED_TESTS += run-pass/coerce-overloaded-autoderef
DISABLED_TESTS += run-pass/coerce-unify-return
DISABLED_TESTS += run-pass/concat
DISABLED_TESTS += run-pass/const-autoderef
DISABLED_TESTS += run-pass/const-block-cross-crate-fn
DISABLED_TESTS += run-pass/const-block-item
DISABLED_TESTS += run-pass/const-block
DISABLED_TESTS += run-pass/const-bound
DISABLED_TESTS += run-pass/const-cast
DISABLED_TESTS += run-pass/discrim-explicit-23030
DISABLED_TESTS += run-pass/dst-coerce-rc
DISABLED_TESTS += run-pass/dst-coercions
DISABLED_TESTS += run-pass/dst-field-align
DISABLED_TESTS += run-pass/dst-irrefutable-bind
DISABLED_TESTS += run-pass/dst-raw
DISABLED_TESTS += run-pass/dst-struct-sole
DISABLED_TESTS += run-pass/dst-struct
DISABLED_TESTS += run-pass/dst-trait
DISABLED_TESTS += run-pass/empty-struct-braces
DISABLED_TESTS += run-pass/explicit-self-generic
DISABLED_TESTS += run-pass/extern-compare-with-return-type
DISABLED_TESTS += run-pass/fat-ptr-cast
DISABLED_TESTS += run-pass/intrinsic-move-val
DISABLED_TESTS += run-pass/issue-11205
DISABLED_TESTS += run-pass/issue-13902
DISABLED_TESTS += run-pass/issue-14399
DISABLED_TESTS += run-pass/issue-15221
DISABLED_TESTS += run-pass/issue-20575
DISABLED_TESTS += run-pass/issue-20797
# - Lazy (MIR)
DISABLED_TESTS += run-pass/if-ret
DISABLED_TESTS += run-pass/intrinsics-integer
DISABLED_TESTS += run-pass/issue-11940
DISABLED_TESTS += run-pass/issue-13620
DISABLED_TESTS += run-pass/issue-13867
DISABLED_TESTS += run-pass/issue-15080
DISABLED_TESTS += run-pass/issue-15104
DISABLED_TESTS += run-pass/issue-15763
DISABLED_TESTS += run-pass/issue-17877	# - SplitSlice + array
DISABLED_TESTS += run-pass/issue-18060	# - Overlapping value ranges
DISABLED_TESTS += run-pass/issue-18110	# - Missing value
DISABLED_TESTS += run-pass/issue-18352	# - Match+const
# - Lazy (misg)
DISABLED_TESTS += run-pass/issue-13494
# - Overly-restrictive consteval
DISABLED_TESTS += run-pass/check-static-mut-slices run-pass/check-static-slice
DISABLED_TESTS += run-pass/const-binops
DISABLED_TESTS += run-pass/const-contents
DISABLED_TESTS += run-pass/const-deref
DISABLED_TESTS += run-pass/const-enum-cast
DISABLED_TESTS += run-pass/const-err
DISABLED_TESTS += run-pass/const-fields-and-indexing
DISABLED_TESTS += run-pass/const-fn-method
DISABLED_TESTS += run-pass/const-fn
DISABLED_TESTS += run-pass/const-str-ptr
DISABLED_TESTS += run-pass/const-vec-of-fns
DISABLED_TESTS += run-pass/diverging-fn-tail-35849
DISABLED_TESTS += run-pass/enum-vec-initializer
DISABLED_TESTS += run-pass/huge-largest-array
DISABLED_TESTS += run-pass/issue-17233
DISABLED_TESTS += run-pass/issue-19244	# Missing type info
# - Type defaults not supported
DISABLED_TESTS += run-pass/default-associated-types
DISABLED_TESTS += run-pass/default_ty_param_default_dependent_associated_type
DISABLED_TESTS += run-pass/default_ty_param_dependent_defaults
DISABLED_TESTS += run-pass/default_ty_param_method_call_test
DISABLED_TESTS += run-pass/default_ty_param_struct_and_type_alias
DISABLED_TESTS += run-pass/default_ty_param_struct
DISABLED_TESTS += run-pass/default_ty_param_trait_impl
DISABLED_TESTS += run-pass/default_ty_param_trait_impl_simple
DISABLED_TESTS += run-pass/default_ty_param_type_alias
DISABLED_TESTS += run-pass/generic-default-type-params-cross-crate
DISABLED_TESTS += run-pass/generic-default-type-params
# - ERROR: Function pointers in consants/statics don't trigger calls
DISABLED_TESTS += run-pass/issue-17718
# - Quirks
DISABLED_TESTS += run-pass/fn-item-type-zero-sized	# fn() items are not ZSTs
DISABLED_TESTS += run-pass/int-abs-overflow	# No overflow checks
DISABLED_TESTS += run-pass/issue-18859	# module_path output is differend
# - BUG-USE AFTER FREE (scoping)
DISABLED_TESTS += run-pass/cleanup-rvalue-scopes
# - BUG-Expand: Copy,Clone calls Clone for inner values instead of copying
DISABLED_TESTS += run-pass/deriving-copyclone
# - BUG-Expand: format_args!
DISABLED_TESTS += run-pass/fmt-pointer-trait
DISABLED_TESTS += run-pass/format-ref-cell
DISABLED_TESTS += run-pass/ifmt
# - BUG-Expand: #[main] and cfg
DISABLED_TESTS += run-pass/intrinsic-alignment
# - BUG-Expand: No cfg on enum vars
DISABLED_TESTS += run-pass/issue-11085
# - BUG-Parse: `use *`
DISABLED_TESTS += run-pass/import-glob-crate
DISABLED_TESTS += run-pass/import-prefix-macro
# - BUG-CODEGEN: Missing symbol
DISABLED_TESTS += run-pass/const-enum-ptr
DISABLED_TESTS += run-pass/const-enum-vec-ptr
DISABLED_TESTS += run-pass/const-vecs-and-slices
# - BUG: Codegen drops
DISABLED_TESTS += run-pass/extern_fat_drop
# - BUG: Enum variants not getting correct integer values
DISABLED_TESTS += run-pass/discriminant_value
DISABLED_TESTS += run-pass/const-nullary-univariant-enum
DISABLED_TESTS += run-pass/enum-discr
DISABLED_TESTS += run-pass/enum-disr-val-pretty
DISABLED_TESTS += run-pass/enum-univariant-repr
DISABLED_TESTS += run-pass/issue-15523-big
DISABLED_TESTS += run-pass/issue-15523
# - BUG: Null pointer opt not fully correct
DISABLED_TESTS += run-pass/enum-null-pointer-opt
# - BUG: Incorrect enum sizing
DISABLED_TESTS += run-pass/enum-discrim-autosizing
DISABLED_TESTS += run-pass/enum-discrim-manual-sizing
DISABLED_TESTS += run-pass/enum-discrim-width-stuff
# - BUG: Leaking contents of boxed trait objects
DISABLED_TESTS += run-pass/drop-struct-as-object
DISABLED_TESTS += run-pass/dynamic-drop
DISABLED_TESTS += run-pass/issue-10802
# - BUG: Bad floats
DISABLED_TESTS += run-pass/float-nan
DISABLED_TESTS += run-pass/float_math
DISABLED_TESTS += run-pass/floatlits
DISABLED_TESTS += run-pass/intrinsics-math
# - BUG: Hygine
DISABLED_TESTS += run-pass/hygiene
DISABLED_TESTS += run-pass/hygienic-labels-in-let
DISABLED_TESTS += run-pass/hygienic-labels
# - ?? Is this valid
DISABLED_TESTS += run-pass/const-enum-vec-index
# - Line information that isn't avaliable due to codegen
DISABLED_TESTS += run-pass/backtrace-debuginfo run-pass/backtrace
# - No unwind catching support
DISABLED_TESTS += run-pass/binary-heap-panic-safe run-pass/box-of-array-of-drop-1 run-pass/box-of-array-of-drop-2
DISABLED_TESTS += run-pass/cleanup-rvalue-temp-during-incomplete-alloc
DISABLED_TESTS += run-pass/drop-trait-enum
DISABLED_TESTS += run-pass/intrinsic-move-val-cleanups
DISABLED_TESTS += run-pass/issue-14875
# - Test framework required
DISABLED_TESTS += run-pass/core-run-destroy
DISABLED_TESTS += run-pass/exec-env
DISABLED_TESTS += run-pass/issue-16597-empty
DISABLED_TESTS += run-pass/issue-16597	# NOTE: Crashes in resolve
DISABLED_TESTS += run-pass/issue-20823
# - Makefile test framework quirks
DISABLED_TESTS += run-pass/issue-18913
# - Target Features
DISABLED_TESTS += run-pass/crt-static-on-works
# - Infinite loops
DISABLED_TESTS += run-pass/issue-16671

DEF_RUST_TESTS = $(sort $(patsubst $(RUST_TESTS_DIR)%.rs,output/rust/%_out.txt,$(wildcard $(RUST_TESTS_DIR)$1/*.rs)))
rust_tests-run-pass: $(filter-out $(patsubst %,output/rust/%_out.txt,$(DISABLED_TESTS)), $(call DEF_RUST_TESTS,run-pass))
rust_tests-run-fail: $(call DEF_RUST_TESTS,run-fail)
#rust_tests-compile-fail: $(call DEF_RUST_TESTS,compile-fail)

output/rust/test_run-pass_hello: $(RUST_TESTS_DIR)run-pass/hello.rs output/libstd.hir $(BIN) output/liballoc_system.hir output/libpanic_abort.hir
	@mkdir -p $(dir $@)
	@echo "--- [MRUSTC] -o $@"
	$(DBG) $(BIN) $< -L output/libs -o $@ $(RUST_FLAGS) $(PIPECMD)
	@echo "--- [$@]"
	@./$@

TEST_ARGS_run-pass/cfgs-on-items := --cfg fooA --cfg fooB
TEST_ARGS_run-pass/cfg-macros-foo := --cfg foo
TEST_ARGS_run-pass/cfg_attr := --cfg set1 --cfg set2
TEST_ARGS_run-pass/issue-11085 := --cfg foo
TEST_ARGS_run-pass/issue-15881-model-lexer-dotdotdot := -g

output/rust/%: $(RUST_TESTS_DIR)%.rs $(RUSTCSRC) $(BIN) output/libstd.hir output/libtest.hir output/test_deps/librust_test_helpers.a
	@mkdir -p $(dir $@)
	@echo "=== TEST $(patsubst output/rust/%,%,$@)"
	@echo "--- [MRUSTC] -o $@"
	$V$(BIN) $< -o $@ -L output/libs -L output/test_deps --stop-after $(RUST_TESTS_FINAL_STAGE) $(TEST_ARGS_$*) > $@.txt 2>&1 || (tail -n 1 $@.txt; false)
output/rust/%_out.txt: output/rust/%
	@echo "--- [$<]"
	@./$< > $@ || (tail -n 1 $@; mv $@ $@_fail; false)

output/test_deps/librust_test_helpers.a: output/test_deps/rust_test_helpers.o
	ar cur $@ $<
output/test_deps/rust_test_helpers.o: $(RUSTCSRC)src/rt/rust_test_helpers.c
	$(CC) -c $< -o $@

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
	objcopy --only-keep-debug $(BIN) $(BIN).debug
	objcopy --add-gnu-debuglink=$(BIN).debug $(BIN)
	strip $(BIN)

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

