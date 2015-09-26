
.SUFFIXES:


OBJS := main.o rust.tab.o rust.lex.o

#TSTFILES := ../samples/1.rs
TSTFILES := ../../rust_os/rustc_src/libcore/lib.rs
#TSTFILES := $(addprefix ../../rust_os/libcore/, lib.rs str/mod.rs str/pattern.rs borrow.rs any.rs array.rs result.rs)



all: test.bin

test: test.bin $(TSTFILES)
	$(foreach f,$(TSTFILES), $(DBG) ./test.bin "$f" &&) true

test.bin: $(OBJS)
	g++ -std=c++11 $(OBJS) -o $@

%.o: %.cpp ast_types.hpp
	g++  -x c++ -std=c++11 $< -c -o $@ -I .
%.o: .gen/%.cpp ast_types.hpp
	g++  -x c++ -std=c++11 $< -c -o $@ -I .
	
.gen/rust.tab.cpp: .gen/.rust.y
	@mkdir -p $(dir $@)
	@echo [BISON] $@
	@bison -o $@ $< -d --verbose
# HACK: Bison emits duplicate entries in the union, remove those
	@mv $(@:%.cpp=%.hpp) $(@:%.cpp=%.hpp.OLD)
	@awk '{ if($$0 ~ /yacc.c:1909/) { ignore = 1; print $$0; } else if( ignore == 1 ) { trigger = $$0; ignore = 2; } else if( ignore == 2 ) { if($$0 == trigger) { ignore = 0 } }  if( ignore > 0 ) { } else { print $$0; }  }' < ${@:%.cpp=%.hpp.OLD} > $(@:%.cpp=%.hpp)
.gen/rust.tab.hpp: .gen/rust.tab.cpp

.gen/.rust.y: Makefile rust.y rust_expr.y.h rust_tts.y.h
	@mkdir -p $(dir $@)
	cat rust.y > $@
	cpp -P rust_expr.y.h >> $@
	cpp -P rust_tts.y.h >> $@


.gen/rust.lex.cpp: rust.lex
	@mkdir -p $(dir $@)
	flex -o $@ $<


main.cpp: .gen/rust.tab.hpp

