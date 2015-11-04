
EXESUF ?=
CXX ?= g++
V ?= @

LINKFLAGS := -g
LIBS :=
CXXFLAGS := -g -Wall -std=c++11 -Werror
#CXXFLAGS += -O3
CPPFLAGS := -I src/include/

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
OBJ += ast/ast.o ast/path.o ast/expr.o ast/pattern.o
OBJ += ast/provided_module.o
OBJ += parse/parseerror.o parse/lex.o
OBJ += parse/root.o parse/paths.o parse/types.o parse/expr.o parse/pattern.o
OBJ += dump_as_rust.o
OBJ += convert/ast_iterate.o
OBJ += convert/decorators.o
OBJ += convert/resolve.o convert/typecheck_bounds.o convert/typecheck_params.o convert/typecheck_expr.o
OBJ += convert/flatten.o convert/render.o
OBJ += synexts/derive.o synexts/lang_item.o

PCHS := ast/ast.hpp

OBJ := $(addprefix $(OBJDIR),$(OBJ))


all: $(BIN)

clean:
	$(RM) -r $(BIN) $(OBJ)


PIPECMD ?= 2>&1 | tee $@_dbg.txt | tail -n 40 ; test $${PIPESTATUS[0]} -eq 0

output/%.ast: samples/%.rs $(BIN) 
	@mkdir -p output/
	$(DBG) $(BIN) $< --emit ast -o $@ $(PIPECMD)

RUSTCSRC := ~/Source/rust/rustc-nightly/
output/core.ast: $(RUSTCSRC)src/libcore/lib.rs $(BIN)
	@mkdir -p output/
	$(DBG) $(BIN) $< --emit ast -o $@ $(PIPECMD)
	

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

