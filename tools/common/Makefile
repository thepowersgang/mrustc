#
# Common helper tools for all applications
#

V ?= @

OBJDIR := .obj/

BIN := ../../bin/common_lib.a
OBJS = toml.o path.o debug.o
OBJS += jobserver.o

CXXFLAGS := -Wall -std=c++14 -g -O2

CXXFLAGS += $(CXXFLAGS_EXTRA)

OBJS := $(OBJS:%=$(OBJDIR)%)

.PHONY: all clean

all: $(BIN)

clean:
	rm $(BIN) $(OBJS)

$(BIN): $(OBJS)
	@+mkdir -p $(dir $@)
	@echo [AR] $@
	$V$(AR) crs $@ $(OBJS)

$(OBJDIR)%.o: %.cpp
	@+mkdir -p $(dir $@)
	@echo [CXX] $<
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) -MMD -MP -MF $@.dep

-include $(OBJS:%.o=%.o.dep)

