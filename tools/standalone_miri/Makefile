#
# Standalone MIR Interpreter
#
ifeq ($(OS),Windows_NT)
  EXESUF ?= .exe
endif
EXESUF ?=

V ?= @

OBJDIR := .obj/

BIN := ../bin/standalone_miri$(EXESUF)
OBJS := main.o debug.o mir.o lex.o value.o module_tree.o hir_sim.o miri.o

LINKFLAGS := -g -lpthread
CXXFLAGS := -Wall -std=c++14 -g -O2
CXXFLAGS += -I ../common -I ../../src/include -I .
CXXFLAGS += -Wno-misleading-indentation	# Gets REALLY confused by the TU_ARM macro

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

../bin/common_lib.a:
	make -C ../common

-include $(OBJS:%.o=%.o.dep)

