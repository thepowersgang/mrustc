
.SUFFIXES:

all: test.bin

TSTFILE := ../samples/1.rs
TSTFILE := ../../rust_os/libcore/lib.rs
TSTFILE := ../../rust_os/libcore/str/mod.rs

test: test.bin $(TSTFILE)
	./test.bin < $(TSTFILE)

test.bin: rust.tab.c rust.lex.c
	gcc rust.tab.c rust.lex.c -o $@
	
rust.tab.c: rust.y
	yacc -o $@ $< -d --verbose

rust.lex.c: rust.lex
	lex -o $@ $<
	

