
EXESUF ?=
CXX ?= g++

LINKFLAGS :=
LIBS :=
CXXFLAGS := -Wall -std=c++11
CPPFLAGS :=


OBJDIR = .obj/

BIN := bin/mrustc$(EXESUF)

OBJ := main.o macros.o types.o ast/ast.o 
OBJ += parse/parseerror.o parse/lex.o parse/preproc.o parse/root.o parse/expr.o
OBJ += convert/flatten.o convert/resolve.o convert/render.o
OBJ := $(addprefix $(OBJDIR),$(OBJ))

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $(LINKFLAGS) $(OBJ) $(LIBS)

$(OBJDIR)%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -o $@ -c $< $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.dep

-include $(OBJ:%=%.dep)

