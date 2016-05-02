
EXESUF ?=
CXX ?= g++
V ?= @

LINKFLAGS := -g
LIBS :=
CXXFLAGS := -g -Wall -std=c++14 -Werror
#CXXFLAGS += -O3
CPPFLAGS := -I src/include/ -I src/

SHELL = bash

ifeq ($(DBGTPL),)
  
else ifeq ($(DBGTPL),gdb)
  DBG := echo -e "r\nbt 7\nq" | gdb --args
else
  $(error "Unknown debug template")
endif

OBJDIR = .obj/

BIN := bin/mrustc$(EXESUF)

OBJ := main.o macros.o types.o serialise.o
OBJ += span.o
OBJ += ast/ast.o ast/crate.o ast/path.o ast/expr.o ast/pattern.o
OBJ += ast/provided_module.o
OBJ += parse/parseerror.o parse/lex.o
OBJ += parse/root.o parse/paths.o parse/types.o parse/expr.o parse/pattern.o parse/macro_rules.o
OBJ += expand/mod.o expand/macro_rules.o expand/cfg.o
OBJ +=  expand/format_args.o
OBJ +=  expand/concat.o expand/stringify.o expand/file_line.o
OBJ +=  expand/derive.o expand/lang_item.o
OBJ +=  expand/std_prelude.o
OBJ += resolve/use.o resolve/index.o resolve/absolute.o
OBJ += hir/from_ast.o
OBJ += dump_as_rust.o
OBJ += convert/ast_iterate.o
#OBJ += convert/decorators.o
#OBJ += convert/resolve.o
OBJ += convert/typecheck_bounds.o convert/typecheck_params.o convert/typecheck_expr.o

PCHS := ast/ast.hpp

OBJ := $(addprefix $(OBJDIR),$(OBJ))


all: $(BIN)

clean:
	$(RM) -r $(BIN) $(OBJ)


PIPECMD ?= 2>&1 | tee $@_dbg.txt | tail -n 40 ; test $${PIPESTATUS[0]} -eq 0

output/%.ast: samples/%.rs $(BIN) 
	@mkdir -p output/
	$(DBG) $(BIN) $< --emit ast -o $@ $(PIPECMD)

RUSTCSRC := /home/tpg/Source/rust/rustc-nightly/
output/core.ast: $(RUSTCSRC)src/libcore/lib.rs $(BIN)
	@mkdir -p output/
	$(DBG) $(BIN) $< --emit ast -o $@ $(PIPECMD)

.PHONY: rust_tests
RUST_TESTS_DIR := $(RUSTCSRC)src/test/
rust_tests: rust_tests-run-pass rust_tests-run-fail rust_tests-compile-fail

DEF_RUST_TESTS = $(sort $(patsubst $(RUST_TESTS_DIR)%.rs,output/rust/%.txt,$(wildcard $(RUST_TESTS_DIR)$1/*.rs)))
rust_tests-run-pass: $(call DEF_RUST_TESTS,run-pass)
rust_tests-run-fail: $(call DEF_RUST_TESTS,run-fail)
rust_tests-compile-fail: $(call DEF_RUST_TESTS,compile-fail)

output/rust/%.txt: $(RUST_TESTS_DIR)%.rs $(BIN)
	@mkdir -p $(dir $@)
	$(BIN) $< -o $@.o --stop-after parse > $@ 2>&1

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
	$V$(CXX) -std=c++11 -o $@ $< $(CPPFLAGS) -MMD -MP -MF $@.dep

-include $(OBJ:%=%.dep)

# vim: noexpandtab ts=4

