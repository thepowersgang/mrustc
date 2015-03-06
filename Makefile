
EXESUF ?=
CXX ?= g++
V ?= @

LINKFLAGS := -g
LIBS :=
CXXFLAGS := -g -Wall -std=c++11
CPPFLAGS := -I src/include/


OBJDIR = .obj/

BIN := bin/mrustc$(EXESUF)

OBJ := main.o macros.o types.o serialise.o
OBJ += ast/ast.o ast/path.o ast/expr.o
OBJ += parse/parseerror.o parse/lex.o parse/preproc.o parse/root.o parse/expr.o
OBJ += dump_as_rust.o
OBJ += convert/ast_iterate.o
OBJ += convert/resolve.o convert/typecheck_bounds.o convert/typecheck_params.o convert/typecheck_expr.o
OBJ += convert/flatten.o convert/render.o
OBJ := $(addprefix $(OBJDIR),$(OBJ))


all: $(BIN)

clean:
	$(RM) -r $(BIN) $(OBJ)

test: $(BIN) samples/1.rs
	mkdir -p output/
	$(DBG) $(BIN) samples/std.rs --emit ast -o output/std.ast 2>&1 | tee output/ast_dbg.txt
	$(DBG) $(BIN) samples/1.rs --crate-path output/std.ast -o output/test.c 2>&1 | tee output/1_dbg.txt

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

