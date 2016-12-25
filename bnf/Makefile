
.SUFFIXES:

CXXFLAGS := -std=c++11 -x c++ -I . -g

OBJS := main.o rust.tab.o rust.lex.o

RUSTSRC := /home/tpg/Source/rust/rustc-nightly/src/

TSTFILES := ../samples/1.rs
TSTFILES += $(addprefix $(RUSTSRC), libcore/lib.rs libstd/lib.rs)

BIN := ./test.bin


all: $(BIN)

clean:
	rm $(BIN) $(OBJS)

test: $(BIN) $(TSTFILES)
	$(foreach f,$(TSTFILES), $(DBG) ./test.bin "$f" &&) true


.PHONY: rust_tests
RUST_TESTS_DIR := $(RUSTSRC)test/
rust_tests: $(patsubst $(RUST_TESTS_DIR)%.rs,output/rust/%.txt,$(sort $(wildcard $(RUST_TESTS_DIR)run-pass/*.rs)))

output/rust/%.txt: $(RUST_TESTS_DIR)%.rs $(BIN)
	@mkdir -p $(dir $@)
	$(BIN) $< > $@ 2>&1

$(BIN): $(OBJS)
	g++ -std=c++11 $(OBJS) -o $@ -g

%.o: %.cpp
	g++ $(CXXFLAGS) $< -c -o $@ -MMD -MP -MF $@.dep
%.o: .gen/%.cpp
	g++ $(CXXFLAGS) $< -c -o $@ -MMD -MP -MF $@.dep

.gen/rust.tab.cpp: .gen/.rust.y
	@mkdir -p $(dir $@)
	@echo [BISON] $@
	@bison -o $@ $< -d --verbose
# HACK: Bison emits duplicate entries in the union, remove those
	@mv $(@:%.cpp=%.hpp) $(@:%.cpp=%.hpp.OLD)
	@awk '{ if($$0 ~ /yacc.c:1909/) { ignore = 1; print $$0; } else if( ignore == 1 ) { trigger = $$0; ignore = 2; } else if( ignore == 2 ) { if($$0 == trigger) { ignore = 0 } }  if( ignore > 0 ) { } else { print $$0; }  }' < ${@:%.cpp=%.hpp.OLD} > $(@:%.cpp=%.hpp)
.gen/rust.tab.hpp: .gen/rust.tab.cpp

.gen/.rust.y: Makefile rust.y rust_expr.y.h rust_expr.y_tree.h rust_tts.y.h
	@mkdir -p $(dir $@)
	cat rust.y > $@
	cpp -P rust_expr.y.h >> $@
	cpp -P rust_tts.y.h >> $@


.gen/rust.lex.cpp: rust.lex
	@mkdir -p $(dir $@)
	flex -o $@ $<


main.cpp: .gen/rust.tab.hpp

-include $(OBJS:%=%.dep)

