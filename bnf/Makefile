
.SUFFIXES:

all: test.bin

test: test.bin ../samples/1.rs
	./test.bin < ../samples/1.rs

test.bin: rust.tab.c rust.lex.c
	gcc rust.tab.c rust.lex.c -o $@
	
rust.tab.c: rust.y
	yacc -o $@ $< -d --debug --verbose

rust.lex.c: rust.lex
	lex -o $@ $<
	

