
.SUFFIXES:

test.bin: rust.tab.c rust.lex.c
	gcc rust.tab.c rust.lex.c -o $@
	
rust.tab.c: rust.y
	yacc -o $@ $< -d

rust.lex.c: rust.lex
	lex -o $@ $<
	

