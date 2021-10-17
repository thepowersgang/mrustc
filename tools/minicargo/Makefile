#
# Mini version of cargo
# - Interprets Cargo.toml files and emits makefiles
# - Supports overriding build script output
#
ifeq ($(OS),Windows_NT)
  EXESUF ?= .exe
endif
EXESUF ?=

V ?= @

OBJDIR := .obj/

BIN := ../../bin/minicargo$(EXESUF)
OBJS := main.o build.o manifest.o repository.o cfg.o

LINKFLAGS := -g -lpthread
CXXFLAGS := -Wall -std=c++14 -g -O2
CXXFLAGS += -I ../common

OBJS := $(OBJS:%=$(OBJDIR)%)

.PHONY: all clean

all: $(BIN)

clean:
	rm $(BIN) $(OBJS)

$(BIN): $(OBJS) ../../bin/common_lib.a
	@+mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ $(OBJS) ../../bin/common_lib.a $(LINKFLAGS)
ifeq ($(OS),Windows_NT)
else ifeq ($(shell uname -s || echo not),Darwin)
else
	objcopy --only-keep-debug $(BIN) $(BIN).debug
	objcopy --add-gnu-debuglink=$(BIN).debug $(BIN)
	strip $(BIN)
endif

$(OBJDIR)%.o: %.cpp
	@+mkdir -p $(dir $@)
	@echo [CXX] $<
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) -MMD -MP -MF $@.dep

../../bin/common_lib.a: $(wildcard ../common/*.* ../common/Makefile)
	$(MAKE) -C ../common

-include $(OBJS:%.o=%.o.dep)

