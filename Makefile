# MRustC - Rust Compiler
# - By John Hodge (Mutabah/thePowersGang)
# 
# Makefile
# 
# - Compiles mrustc
# - Downloads rustc source to test against
# - Attempts to compile rust's libstd

EXESUF ?=
CXX ?= g++
V ?= @

LINKFLAGS := -g
LIBS := -lboost_iostreams
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
OBJ += span.o rc_string.o debug.o
OBJ += ast/ast.o
OBJ +=  ast/types.o ast/crate.o ast/path.o ast/expr.o ast/pattern.o
OBJ += parse/parseerror.o
OBJ +=  parse/lex.o parse/token.o
OBJ +=  parse/interpolated_fragment.o
OBJ += parse/root.o parse/paths.o parse/types.o parse/expr.o parse/pattern.o
OBJ += dump_as_rust.o
OBJ += expand/mod.o expand/macro_rules.o expand/cfg.o
OBJ +=  expand/format_args.o expand/asm.o
OBJ +=  expand/concat.o expand/stringify.o expand/file_line.o
OBJ +=  expand/derive.o expand/lang_item.o
OBJ +=  expand/std_prelude.o expand/crate_tags.o
OBJ +=  expand/include.o
OBJ += macro_rules/mod.o macro_rules/eval.o macro_rules/parse.o
OBJ += resolve/use.o resolve/index.o resolve/absolute.o
OBJ += hir/from_ast.o hir/from_ast_expr.o
OBJ +=  hir/dump.o
OBJ +=  hir/hir.o hir/generic_params.o
OBJ +=  hir/crate_ptr.o hir/type_ptr.o hir/expr_ptr.o
OBJ +=  hir/type.o hir/path.o hir/expr.o hir/pattern.o
OBJ +=  hir/visitor.o hir/crate_post_load.o
OBJ += hir_conv/expand_type.o hir_conv/constant_evaluation.o hir_conv/resolve_ufcs.o hir_conv/bind.o
OBJ += hir_typeck/outer.o hir_typeck/common.o hir_typeck/helpers.o hir_typeck/static.o hir_typeck/impl_ref.o
OBJ += hir_typeck/expr_visit.o
OBJ += hir_typeck/expr_cs.o
OBJ += hir_typeck/expr_check.o
OBJ += hir_expand/annotate_value_usage.o hir_expand/closures.o hir_expand/ufcs_everything.o
OBJ += hir_expand/reborrow.o
OBJ += mir/mir.o mir/mir_ptr.o
OBJ +=  mir/dump.o
OBJ +=  mir/from_hir.o mir/from_hir_match.o mir/mir_builder.o
OBJ +=  mir/check.o
OBJ += hir/serialise.o hir/deserialise.o hir/serialise_lowlevel.o

PCHS := ast/ast.hpp

OBJ := $(addprefix $(OBJDIR),$(OBJ))


all: $(BIN)

clean:
	$(RM) -r $(BIN) $(OBJ)


PIPECMD ?= 2>&1 | tee $@_dbg.txt | tail -n 45 ; test $${PIPESTATUS[0]} -eq 0

output/%.ast: samples/%.rs $(BIN) 
	@mkdir -p output/
	$(DBG) $(BIN) $< -o $@ $(PIPECMD)

RUSTCSRC := ./rustc-nightly/
RUSTC_SRC_DL := $(RUSTCSRC)/dl-version

output/lib%.hir: $(RUSTCSRC)src/lib%/lib.rs $(RUSTCSRC) $(BIN)
	@echo "--- [MRUSTC] $@"
	@mkdir -p output/
	@rm -f $@
	$(DBG) $(BIN) $< -o $@ $(PIPECMD)
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

output/liballoc.hir: output/libcore.hir
output/librustc_unicode.hir: output/libcore.hir
output/libcollections.hir: output/libcore.hir output/liballoc.hir output/librustc_unicode.hir
output/librand.hir: output/libcore.hir
output/liblibc.hir: output/libcore.hir
output/libstd.hir: $(call fcn_extcrate, core collections rand libc unwind)
output/libunwind.hir: $(call fcn_extcrate, core libc)

output/libtest.hir: $(call fcn_extcrate, std getopts term panic_unwind)
output/libgetopts.hir: output/libstd.hir

$(RUSTCSRC): rust-nightly-date
	@export DL_RUST_DATE=$$(cat rust-nightly-date); \
	export DISK_RUST_DATE=$$([ -f $(RUSTC_SRC_DL) ] && cat $(RUSTC_SRC_DL)); \
	if [ "$$DL_RUST_DATE" != "$$DISK_RUST_DATE" ]; then \
		echo "Rust version on disk is '$${DISK_RUST_DATE}'. Downloading $${DL_RUST_DATE}."; \
		rm rustc-nightly-src.tar.gz; \
		rm -rf rustc-nightly; \
		wget https://static.rust-lang.org/dist/$${DL_RUST_DATE}/rustc-nightly-src.tar.gz; \
		tar -xf rustc-nightly-src.tar.gz; \
		echo "$$DL_RUST_DATE" > $(RUSTC_SRC_DL); \
	fi

.PHONY: rust_tests
RUST_TESTS_DIR := $(RUSTCSRC)src/test/
rust_tests: rust_tests-run-pass rust_tests-run-fail
# rust_tests-compile-fail

DEF_RUST_TESTS = $(sort $(patsubst $(RUST_TESTS_DIR)%.rs,output/rust/%.o,$(wildcard $(RUST_TESTS_DIR)$1/*.rs)))
rust_tests-run-pass: $(call DEF_RUST_TESTS,run-pass)
rust_tests-run-fail: $(call DEF_RUST_TESTS,run-fail)
#rust_tests-compile-fail: $(call DEF_RUST_TESTS,compile-fail)

output/rust/test_run-pass_hello: $(RUST_TESTS_DIR)run-pass/hello.rs output/libstd.hir $(BIN)
	$(DBG) $(BIN) $< -o $@ $(PIPECMD)

output/rust/%.o: $(RUST_TESTS_DIR)%.rs $(RUSTCSRC) $(BIN) output/libstd.hir output/libtest.hir
	@mkdir -p $(dir $@)
	@echo "--- TEST $(patsubst output/rust/%.o,%,$@)"
	@$(BIN) $< -o $@ --stop-after resolve > $@.txt 2>&1
	@touch $@

output/rust/run-pass/allocator-default.o: output/libstd.hir output/liballoc_jemalloc.hir output/liballocator_dummy.hir
output/rust/run-pass/allocator-system.o: output/liballoc_system.hir

test_deps_run-pass.mk: Makefile $(wildcard $(RUST_TESTS_DIR)run_pass/*.rs)
	@echo "--- Generating test dependencies: $@"
	@grep 'aux-build:' rustc-nightly/src/test/run-pass/*.rs | awk -F : '{a=gensub(/.+run-pass\/(.*)\.rs$$/, "\\1", "g", $$1); b=gensub(/(.*)\.rs/,"\\1","g",$$3); gsub(/-/,"_",b); print "output/rust/run-pass/" a ".o: " "output/test_deps/lib" b ".hir" }' > $@.tmp
	@grep 'aux-build:' rustc-nightly/src/test/run-pass/*.rs | awk -F : '{ print $$3 }' | sort | uniq | awk '{ b=gensub(/(.*)\.rs/,"\\1","g",$$1); gsub(/-/,"_",b); print "output/test_deps/lib" b ".hir: $$(RUST_TESTS_DIR)run-pass/auxiliary/" $$1 " output/libstd.hir" ; print "\t@mkdir -p $$(dir $$@)" ; print "\t@echo \"--- [MRUSTC] $$@\"" ; print "\t@$$(DBG) $$(BIN) $$< --crate-type rlib -o $$@ > $$@.txt 2>&1" }' >> $@.tmp
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

output/rust_os/libkernel.hir: ../rust_os/Kernel/Core/main.rs output/libstack_dst.hir $(BIN)
	@mkdir -p $(dir $@)
	$(DBG) $(BIN) $< -o $@ $(PIPECMD)
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

