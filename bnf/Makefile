
.SUFFIXES:

all: test.bin

TSTFILE := ../samples/1.rs
TSTFILE := $(addprefix ../../rust_os/libcore/, lib.rs str/mod.rs str/pattern.rs borrow.rs any.rs array.rs)

test: test.bin $(TSTFILE)
	$(foreach f,$(TSTFILE), ./test.bin "$f" &&) true
#	for f in $(TSTFILE); do ./test.bin "$$f"; done

test.bin: rust.tab.c rust.lex.c
	gcc rust.tab.c rust.lex.c -o $@
	
rust.tab.c: .rust.y
	yacc -o $@ $< -d --verbose
.rust.y: rust.y rust_expr.y.h rust_expr.y_tree.h
	cat rust.y > $@
	cpp -P rust_expr.y.h >> $@

rust.lex.c: rust.lex
	lex -o $@ $<
	

