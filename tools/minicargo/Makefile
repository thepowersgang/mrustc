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

BIN := ../bin/minicargo$(EXESUF)
OBJS := main.o build.o manifest.o repository.o

LINKFLAGS := -g -lpthread
CXXFLAGS := -Wall -std=c++14 -g -O2
CXXFLAGS += -I ../common

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

../bin/common_lib.a: $(wildcard ../common/*.* ../common/Makefile)
	make -C ../common

-include $(OBJS:%.o=%.o.dep)

