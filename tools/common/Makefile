#
# Mini version of cargo
# - Interprets Cargo.toml files and emits makefiles
# - Supports overriding build script output
#

V ?= @

OBJDIR := .obj/

BIN := ../../bin/common_lib.a
OBJS = toml.o path.o debug.o

CXXFLAGS := -Wall -std=c++14 -g -O2

OBJS := $(OBJS:%=$(OBJDIR)%)

.PHONY: all clean

all: $(BIN)

clean:
	rm $(BIN) $(OBJS)

$(BIN): $(OBJS)
	@+mkdir -p $(dir $@)
	@echo [AR] rcu $@
	$V$(AR) rcu $@ $(OBJS)

$(OBJDIR)%.o: %.cpp
	@+mkdir -p $(dir $@)
	@echo [CXX] $<
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) -MMD -MP -MF $@.dep

-include $(OBJS:%.o=%.o.dep)

