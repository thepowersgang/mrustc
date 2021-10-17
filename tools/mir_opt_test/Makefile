#
# MIR Optimisation test framework
#
ifeq ($(OS),Windows_NT)
  EXESUF ?= .exe
endif
EXESUF ?=

V ?= @

OBJDIR := .obj/

BIN := ../../bin/mir_opt_test$(EXESUF)
OBJS := main.o parser.o
LIBS := ../../bin/mrustc.a ../../bin/common_lib.a

LINKFLAGS := -g -lpthread -lz
CXXFLAGS := -Wall -std=c++14 -g -O2
CXXFLAGS += -I ../common -I ../../src/include -I ../../src -I .
CXXFLAGS += -Wno-misleading-indentation	# Gets REALLY confused by the TU_ARM macro

OBJS := $(OBJS:%=$(OBJDIR)%)

.PHONY: all clean

all: $(BIN)

clean:
	rm $(BIN) $(OBJS)

.PHONY: run
run: $(BIN)
	$(BIN) tests > tests.log

$(BIN): $(OBJS) $(LIBS)
	@+mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ $(OBJS) $(LIBS) $(LINKFLAGS)

$(OBJDIR)%.o: %.cpp
	@+mkdir -p $(dir $@)
	@echo [CXX] $<
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) -MMD -MP -MF $@.dep
../../bin/mrustc.a:
	$(MAKE) -C ../../ bin/mrustc.a
../../bin/common_lib.a:
	$(MAKE) -C ../common

-include $(OBJS:%.o=%.o.dep)

