
EXESUF ?=
CXX ?= g++
V ?= @

LINKFLAGS := -g
LIBS :=
CXXFLAGS := -g -Wall -std=c++14 -Werror
#CXXFLAGS += -Wextra
CXXFLAGS += -O2
CPPFLAGS := -I src/include/ -I src/

#CXXFLAGS += -Wno-unused-private-field

SHELL = bash

ifeq ($(DBGTPL),)
  
else ifeq ($(DBGTPL),gdb)
  DBG := echo -e "r\nbt 12\nq" | gdb --args
else ifeq ($(DBGTPL),valgrind)
  DBG := valgrind --leak-check=full --num-callers=35
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
OBJ +=  expand/format_args.o
OBJ +=  expand/concat.o expand/stringify.o expand/file_line.o
OBJ +=  expand/derive.o expand/lang_item.o
OBJ +=  expand/std_prelude.o
OBJ += macro_rules/mod.o macro_rules/eval.o macro_rules/parse.o
OBJ += resolve/use.o resolve/index.o resolve/absolute.o
OBJ += hir/from_ast.o hir/from_ast_expr.o
OBJ +=  hir/hir.o hir/generic_params.o
OBJ +=  hir/crate_ptr.o hir/type_ptr.o hir/expr_ptr.o
OBJ +=  hir/type.o hir/path.o hir/expr.o hir/pattern.o
OBJ +=  hir/visitor.o
OBJ += hir_conv/expand_type.o hir_conv/constant_evaluation.o hir_conv/resolve_ufcs.o hir_conv/bind.o
OBJ += hir_typeck/outer.o hir_typeck/expr_context.o hir_typeck/helpers.o hir_typeck/static.o
OBJ += hir_typeck/expr_visit.o
OBJ += hir_typeck/expr_simple.o hir_typeck/expr_cs.o
OBJ += hir_typeck/expr_check.o
OBJ += hir_expand/closures.o hir_expand/ufcs_everything.o
OBJ += mir/mir_ptr.o mir/from_hir.o

PCHS := ast/ast.hpp

OBJ := $(addprefix $(OBJDIR),$(OBJ))


all: $(BIN)

clean:
	$(RM) -r $(BIN) $(OBJ)


PIPECMD ?= 2>&1 | tee $@_dbg.txt | tail -n 40 ; test $${PIPESTATUS[0]} -eq 0

output/%.ast: samples/%.rs $(BIN) 
	@mkdir -p output/
	$(DBG) $(BIN) $< --emit ast -o $@ $(PIPECMD)

RUSTCSRC := ./rustc-nightly/
output/core.ast: $(RUSTCSRC)src/libcore/lib.rs $(BIN)
	@mkdir -p output/
	$(DBG) $(BIN) $< --emit ast -o $@ $(PIPECMD)

.PHONY: UPDATE
UPDATE:
	wget -c https://static.rust-lang.org/dist/rustc-nightly-src.tar.gz
	tar -xf rustc-nightly-src.tar.gz

.PHONY: rust_tests
RUST_TESTS_DIR := $(RUSTCSRC)src/test/
rust_tests: rust_tests-run-pass rust_tests-run-fail rust_tests-compile-fail

DEF_RUST_TESTS = $(sort $(patsubst $(RUST_TESTS_DIR)%.rs,output/rust/%.o,$(wildcard $(RUST_TESTS_DIR)$1/*.rs)))
rust_tests-run-pass: $(call DEF_RUST_TESTS,run-pass)
rust_tests-run-fail: $(call DEF_RUST_TESTS,run-fail)
rust_tests-compile-fail: $(call DEF_RUST_TESTS,compile-fail)

output/rust/%.o: $(RUST_TESTS_DIR)%.rs $(BIN)
	@mkdir -p $(dir $@)
	$(BIN) $< -o $@ --stop-after parse > $@.txt 2>&1
	touch $@

test: output/core.ast $(BIN)
# output/std.ast output/log.ast output/env_logger.ast output/getopts.ast
	@mkdir -p output/
#	$(DBG) $(BIN) samples/1.rs --crate-path output/std.ast -o output/test.c 2>&1 | tee output/1_dbg.txt

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ $(LINKFLAGS) $(OBJ) $(LIBS)

$(OBJDIR)%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.dep

src/main.cpp:	$(PCHS:%=src/%.gch)

%.hpp.gch: %.hpp
	@echo [CXX] -o $@
	$V$(CXX) -std=c++14 -o $@ $< $(CPPFLAGS) -MMD -MP -MF $@.dep

-include $(OBJ:%=%.dep)

# vim: noexpandtab ts=4

