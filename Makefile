
EXESUF ?=
CXX ?= g++
V ?= @

LINKFLAGS :=
LIBS :=
CXXFLAGS := -Wall -std=c++11
CPPFLAGS := -I src/include/


OBJDIR = .obj/

BIN := bin/mrustc$(EXESUF)

OBJ := main.o macros.o types.o serialise.o
OBJ += ast/ast.o ast/path.o ast/expr.o
OBJ += parse/parseerror.o parse/lex.o parse/preproc.o parse/root.o parse/expr.o
OBJ += convert/ast_iterate.o convert/flatten.o convert/resolve.o convert/render.o
OBJ := $(addprefix $(OBJDIR),$(OBJ))


all: $(BIN)

clean:
	$(RM) -r $(BIN) $(OBJ)

test: $(BIN) samples/1.rs
	mkdir -p output/
	time $(BIN) samples/std.rs --emit ast -o output/std.ast
	time $(BIN) samples/1.rs --crate-path output/std.ast

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

