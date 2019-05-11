#
ifeq ($(OS),Windows_NT)
  EXESUF ?= .exe
endif
EXESUF ?=

V ?= @

OBJDIR := .obj/

BIN := ../bin/dump_hirfile$(EXESUF)
OBJS := main.o
OBJS += debug.o rc_string.o span.o ident.o
OBJS += parse/parseerror.o	# Why is this needed? ast/path.cpp uses it in binding
OBJS += hir/hir.o hir/type.o hir/deserialise.o hir/serialise_lowlevel.o
OBJS += hir/crate_ptr.o hir/generic_params.o hir/path.o hir/pattern.o hir/expr_ptr.o
OBJS += hir/expr.o	# Why is this needed?
OBJS += parse/token.o parse/tokentree.o parse/tokenstream.o
OBJS += ast/ast.o ast/expr.o ast/path.o ast/types.o ast/pattern.o
OBJS += mir/mir.o mir/mir_ptr.o mir/dump.o hir/visitor.o
OBJS += macro_rules/mod.o

LINKFLAGS := -g -lpthread -lz
CXXFLAGS := -Wall -std=c++14 -g -O2
CXXFLAGS += -I ../common -I ../../src -I ../../src/include

OBJS := $(OBJS:%=$(OBJDIR)%)

.PHONY: all clean

all: $(BIN)

clean:
	rm $(BIN) $(OBJS)

$(BIN): $(OBJS) ../bin/common_lib.a
	@mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ $(OBJS) ../bin/common_lib.a $(LINKFLAGS)

$(OBJDIR)%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo [CXX] $<
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) -MMD -MP -MF $@.dep

$(OBJDIR)%.o: ../../src/%.cpp
	@mkdir -p $(dir $@)
	@echo [CXX] $<
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) -MMD -MP -MF $@.dep

../bin/common_lib.a: $(wildcard ../common/*.* ../common/Makefile)
	make -C ../common

-include $(OBJS:%.o=%.o.dep)


