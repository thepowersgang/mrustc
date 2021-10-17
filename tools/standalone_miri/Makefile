#
# Standalone MIR Interpreter
#
ifeq ($(OS),Windows_NT)
  EXESUF ?= .exe
endif
EXESUF ?=

V ?= @

OBJDIR := .obj/

BIN := ../../bin/standalone_miri$(EXESUF)
OBJS := main.o debug.o mir.o lex.o value.o module_tree.o hir_sim.o rc_string.o
OBJS += miri.o miri_extern.o

LINKFLAGS := -g -lpthread
CXXFLAGS := -Wall -std=c++14 -g -O2
CXXFLAGS += -I ../common -I ../../src/include -I .
CXXFLAGS += -Wno-misleading-indentation	# Gets REALLY confused by the TU_ARM macro

OBJS := $(OBJS:%=$(OBJDIR)%)

COMMON_LIB := ../../bin/common_lib.a

.PHONY: all clean

all: $(BIN)

clean:
	rm $(BIN) $(OBJS)

$(BIN): $(OBJS) $(COMMON_LIB)
	@+mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ $(OBJS) $(COMMON_LIB) $(LINKFLAGS)

$(OBJDIR)%.o: %.cpp
	@+mkdir -p $(dir $@)
	@echo [CXX] $<
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) -MMD -MP -MF $@.dep

$(OBJDIR)%.o: ../../src/%.cpp
	@+mkdir -p $(dir $@)
	@echo [CXX] $<
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) -MMD -MP -MF $@.dep

$(COMMON_LIB):
	$(MAKE) -C ../common

-include $(OBJS:%.o=%.o.dep)

