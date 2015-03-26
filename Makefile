
EXESUF ?=
CXX ?= g++
V ?= @

LINKFLAGS := -g
LIBS :=
CXXFLAGS := -g -Wall -std=c++11
CPPFLAGS := -I src/include/

SHELL = bash

OBJDIR = .obj/

BIN := bin/mrustc$(EXESUF)

OBJ := main.o macros.o types.o serialise.o
OBJ += ast/ast.o ast/path.o ast/expr.o ast/pattern.o
OBJ += ast/provided_module.o
OBJ += parse/parseerror.o parse/lex.o
OBJ += parse/root.o parse/paths.o parse/types.o parse/expr.o parse/pattern.o
OBJ += dump_as_rust.o
OBJ += convert/ast_iterate.o
OBJ += convert/resolve.o convert/typecheck_bounds.o convert/typecheck_params.o convert/typecheck_expr.o
OBJ += convert/flatten.o convert/render.o
OBJ := $(addprefix $(OBJDIR),$(OBJ))


all: $(BIN)

clean:
	$(RM) -r $(BIN) $(OBJ)

output/%.ast: samples/%.rs $(BIN) 
	@mkdir -p output/
	$(DBG) $(BIN) $< --emit ast -o $@ 2>&1 | tee $@_dbg.txt | tail -n 20 ; test $${PIPESTATUS[0]} -eq 0

TEST_FILE = ../../../Source/rust/rustc-nightly/src/libcore/lib.rs
test: $(TEST_FILE) $(BIN) output/std.ast output/log.ast output/env_logger.ast output/getopts.ast
	mkdir -p output/
#	$(DBG) $(BIN) samples/1.rs --crate-path output/std.ast -o output/test.c 2>&1 | tee output/1_dbg.txt
#	$(DBG) $(BIN) ../../BinaryView2/src/main.rs --crate-path output/ -o output/test.c 2>&1 | tee output/1_dbg.txt ; test $${PIPESTATUS[0]} -eq 0
#	$(DBG) $(BIN) ../../RustPorts/LogicCircuit/src/main.rs --crate-path output/ -o output/test.c 2>&1 | tee output/1_dbg.txt ; test $${PIPESTATUS[0]} -eq 0
	$(DBG) $(BIN) $(TEST_FILE) --crate-path output/ -o output/test.c 2>&1 | tee output/1_dbg.txt | tail -n 20 ; test $${PIPESTATUS[0]} -eq 0

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ $(LINKFLAGS) $(OBJ) $(LIBS)

$(OBJDIR)%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.dep

-include $(OBJ:%=%.dep)

# vim: noexpandtab ts=4

