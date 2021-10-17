V ?= @

OBJDIR := .obj/

BIN := ../../bin/testrunner
OBJS := main.o path.o

LINKFLAGS := -g
CXXFLAGS := -Wall -std=c++14 -g -O2

OBJS := $(OBJS:%=$(OBJDIR)%)

.PHONY: all clean

all: $(BIN)

clean:
	rm $(BIN) $(OBJS) $(OBJS:%.o=%.o.dep)

$(BIN): $(OBJS)
	@+mkdir -p $(dir $@)
	@echo [CXX] -o $@
	$V$(CXX) -o $@ $(OBJS) $(LINKFLAGS)

$(OBJDIR)%.o: %.cpp
	@+mkdir -p $(dir $@)
	@echo [CXX] $<
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) -MMD -MP -MF $@.dep
# - Include files from common (TODO: Be less hacky)
$(OBJDIR)%.o: ../common/%.cpp
	@+mkdir -p $(dir $@)
	@echo [CXX] $<
	$V$(CXX) -o $@ -c $< $(CXXFLAGS) -MMD -MP -MF $@.dep

-include $(OBJS:%.o=%.o.dep)
