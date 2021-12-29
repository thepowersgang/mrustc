#
# dump_hirfile
# - Print the contents of crate metadata
#
ifeq ($(OS),Windows_NT)
  EXESUF ?= .exe
endif
EXESUF ?=

V ?= @

OBJDIR := .obj/

BIN := ../../bin/dump_hirfile$(EXESUF)
OBJS := main.o

LINKFLAGS := -g -lpthread -lz
CXXFLAGS := -Wall -std=c++14 -g -O2
CXXFLAGS += -I ../common -I ../../src -I ../../src/include

LIBS := ../../bin/mrustc.a ../../bin/common_lib.a

OBJS := $(OBJS:%=$(OBJDIR)%)

.PHONY: all clean

all: $(BIN)

clean:
	rm $(BIN) $(OBJS)

$(BIN): $(OBJS) $(LIBS)
	@mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ $(OBJS) $(LIBS) $(LINKFLAGS)

$(OBJDIR)%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo [CXX] $<
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) -MMD -MP -MF $@.dep

../../bin/mrustc.a: $(wildcard ../../src/*.* ../../src/*/*.*)
	$(MAKE) -C ../../ bin/mrustc.a
../../bin/common_lib.a: $(wildcard ../common/*.* ../common/Makefile)
	$(MAKE) -C ../common

-include $(OBJS:%.o=%.o.dep)


