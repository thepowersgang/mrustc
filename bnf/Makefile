
.SUFFIXES:

all: test.bin

TSTFILES := ../samples/1.rs
TSTFILES := $(addprefix ../../rust_os/libcore/, lib.rs str/mod.rs str/pattern.rs borrow.rs any.rs array.rs result.rs)

test: test.bin $(TSTFILES)
	$(foreach f,$(TSTFILES), ./test.bin "$f" &&) true

OBJS := rust.tab.o rust.lex.o

test.bin: $(OBJS)
	g++ -std=c++11 $(OBJS) -o $@

%.o: %.cpp
	g++  -x c++ -std=c++11 $< -c -o $@ -I .
%.o: .gen/%.cpp
	g++  -x c++ -std=c++11 $< -c -o $@ -I .
	
.gen/rust.tab.cpp: .gen/.rust.y
	@mkdir -p $(dir $@)
	bison -o $@ $< -d --verbose

.gen/.rust.y: Makefile rust.y rust_expr.y.h rust_tts.y.h
	@mkdir -p $(dir $@)
	cat rust.y > $@
	cpp -P rust_expr.y.h >> $@
	cpp -P rust_tts.y.h >> $@


.gen/rust.lex.cpp: rust.lex
	@mkdir -p $(dir $@)
	flex -o $@ $<
	

