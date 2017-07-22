#
# Mini version of cargo
# - Interprets Cargo.toml files and emits makefiles
# - Supports overriding build script output
#

V ?= @

OBJDIR := .obj/

BIN := ../bin/minicargo
OBJS := main.o

LINKFLAGS := 
CXXFLAGS := -Wall

OBJS := $(OBJS:%=$(OBJDIR)%)

.PHONY: all clean

all: $(BIN)

clean:
	rm $(BIN) $(OBJS)

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ $(OBJS) $(LINKFLAGS)

$(OBJDIR)%.o: %.cpp
	@echo [CXX] $<
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) -MMD -MP -MF $@.dep

-include $(OBJS:%.o=%.dep)

