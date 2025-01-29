# MRustC - Rust Compiler
# - By John Hodge (Mutabah/thePowersGang)
#
# Makefile
#
# - Compiles mrustc
# - Provides shortcuts to tasks done in minicargo.mk
#
# DEPENDENCIES
# - zlib (-dev)
# - curl (bin, for downloading libstd source)

ifeq ($(OS),Windows_NT)
  EXESUF ?= .exe
else
  EXESUF ?=
endif
# CXX : C++ compiler
CXX ?= g++
# V (or VERBOSE) : If set, prints all important commands
V ?= !
# GPROF : If set, enables the generation of a gprof annotated executable
GPROF ?=

OBJCOPY ?= objcopy
STRIP ?= strip

ifneq ($(VERBOSE),)
 V :=
endif
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
#CXXFLAGS += -Wno-unknown-warning-option
#CXXFLAGS += -Wno-unqualified-std-cast-call

CXXFLAGS += -Werror=return-type
CXXFLAGS += -Werror=switch

# Force the use of `bash` as the shell
SHELL = bash

OBJDIR = .obj/

ifneq ($(GPROF),)
  OBJDIR := .obj-gprof/
  CXXFLAGS += -pg -no-pie
  LINKFLAGS += -pg -no-pie
  EXESUF := -gprof$(EXESUF)
endif

LINKFLAGS += $(LINKFLAGS_EXTRA)

BIN := bin/mrustc$(EXESUF)

OBJ := main.o version.o
OBJ += span.o rc_string.o debug.o ident.o
OBJ += ast/ast.o
OBJ +=  ast/types.o ast/crate.o ast/path.o ast/expr.o ast/pattern.o
OBJ +=  ast/dump.o
OBJ += parse/parseerror.o
OBJ +=  parse/token.o parse/tokentree.o parse/interpolated_fragment.o
OBJ +=  parse/tokenstream.o parse/lex.o parse/ttstream.o
OBJ +=  parse/root.o parse/paths.o parse/types.o parse/expr.o parse/pattern.o
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
OBJ +=  expand/rustc_box.o
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
OBJ +=  hir_conv/lifetime_elision.o
OBJ += hir_typeck/outer.o hir_typeck/common.o hir_typeck/helpers.o hir_typeck/static.o hir_typeck/impl_ref.o
OBJ +=  hir_typeck/resolve_common.o
OBJ +=  hir_typeck/expr_visit.o
OBJ +=  hir_typeck/expr_cs.o hir_typeck/expr_cs__enum.o
OBJ +=  hir_typeck/expr_check.o
OBJ += hir_expand/annotate_value_usage.o hir_expand/closures.o
OBJ +=  hir_expand/ufcs_everything.o
OBJ +=  hir_expand/reborrow.o hir_expand/erased_types.o hir_expand/vtable.o
OBJ +=  hir_expand/static_borrow_constants.o
OBJ +=  hir_expand/lifetime_infer.o
OBJ += mir/mir.o mir/mir_ptr.o
OBJ +=  mir/dump.o mir/helpers.o mir/visit_crate_mir.o
OBJ +=  mir/from_hir.o mir/from_hir_match.o mir/mir_builder.o
OBJ +=  mir/check.o mir/cleanup.o mir/optimise.o
OBJ +=  mir/check_full.o
OBJ +=  mir/borrow_check.o
OBJ += hir/serialise.o hir/deserialise.o hir/serialise_lowlevel.o
OBJ += trans/trans_list.o trans/mangling_v2.o
OBJ +=  trans/enumerate.o trans/auto_impls.o trans/monomorphise.o trans/codegen.o
OBJ +=  trans/codegen_c.o trans/codegen_c_structured.o trans/codegen_mmir.o
OBJ +=  trans/target.o trans/allocator.o

# TODO is this needed? (or worth it)
PCHS := ast/ast.hpp

OBJ := $(addprefix $(OBJDIR),$(OBJ))

.PHONY: all clean

all: $(BIN)

clean:
	$(RM) -rf -- $(BIN) $(OBJ) bin/mrustc.a


#
# Defer to minicargo.mk for some common operations
#
.PHONY: rust_tests-libs
.PHONY: local_tests
.PHONY: RUSTCSRC
.PHONY: test
.PHONY: LIBS
rust_tests-libs local_tests RUSTCSRC test LIBS:
	$(MAKE) -f minicargo.mk $@

# -------------------------------
# Compile rules for mrustc itself
# -------------------------------
bin/mrustc.a: $(filter-out $(OBJDIR)main.o, $(OBJ))
	@+mkdir -p $(dir $@)
	@echo [AR] $@
	$V$(AR) crs $@ $(filter-out $(OBJDIR)main.o, $(OBJ))

$(BIN): $(OBJDIR)main.o bin/mrustc.a bin/common_lib.a
	@+mkdir -p $(dir $@)
	@echo [CXX] -o $@
ifeq ($(OS),Windows_NT)
	$V$(CXX) -o $@ $(LINKFLAGS) $(OBJDIR)main.o -Wl,--whole-archive bin/mrustc.a bin/common_lib.a -Wl,--no-whole-archive $(LIBS)
else ifeq ($(shell uname -s || echo not),Darwin)
	$V$(CXX) -o $@ $(LINKFLAGS) $(OBJDIR)main.o -Wl,-all_load bin/mrustc.a bin/common_lib.a $(LIBS)
else
	$V$(CXX) -o $@ $(LINKFLAGS) $(OBJDIR)main.o -Wl,--whole-archive bin/mrustc.a -Wl,--no-whole-archive bin/common_lib.a $(LIBS)
	$(OBJCOPY) --only-keep-debug $(BIN) $(BIN).debug
	$(OBJCOPY) --add-gnu-debuglink=$(BIN).debug $(BIN)
	$(STRIP) $(BIN)
endif

$(OBJDIR)%.o: src/%.cpp
	@+mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.dep
$(OBJDIR)version.o: $(OBJDIR)%.o: src/%.cpp $(filter-out $(OBJDIR)version.o,$(OBJ)) Makefile
	@+mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.dep -D VERSION_GIT_FULLHASH=\"$(shell git show --pretty=%H -s --no-show-signature)\" -D VERSION_GIT_BRANCH="\"$(shell git symbolic-ref -q --short HEAD || git describe --tags --exact-match)\"" -D VERSION_GIT_SHORTHASH=\"$(shell git show -s --pretty=%h --no-show-signature)\" -D VERSION_BUILDTIME="\"$(shell env LC_TIME=C date -u +"%a, %e %b %Y %T +0000")\"" -D VERSION_GIT_ISDIRTY=$(shell git diff-index --quiet HEAD; echo $$?)

src/main.cpp: $(PCHS:%=src/%.gch)

%.hpp.gch: %.hpp
	@echo [CXX] -o $@
	$V$(CXX) -std=c++14 -o $@ $< $(CPPFLAGS) -MMD -MP -MF $@.dep

bin/common_lib.a: $(wildcard tools/common/*)
	$(MAKE) -C tools/common
	
-include $(OBJ:%=%.dep)

# vim: noexpandtab ts=4

