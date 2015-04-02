%{
#include "rust.tab.h"
#include <stdio.h>
void yyerror(const char *s);
extern int yydebug;
%}

dec_digit	[0-9]
ident_c	[a-zA-Z_]

%%

[/][/][^/].*\n	{ yylineno += 1; }
[/][/][/].*\n	{ yylineno += 1; }	// TODO: Handle /// by desugaring
\n	{ yylineno += 1; }
\r	/* */
[ \t]	/* */

mod	{ return RWD_mod; }
use	{ return RWD_use; }
static	{ return RWD_static; }
const	{ return RWD_const; }
fn	{ return RWD_fn; }
as	{ return RWD_as; }
mut	{ return RWD_mut; }

::	{ return DOUBLECOLON; }
#![[]	{ return SUPER_ATTR; }
:	{ return *yytext; }
;	{ return *yytext; }
=	{ return *yytext; }

{ident_c}({ident_c}|[0-9])*	{ yylval.text = strdup(yytext); return IDENT; }
.	{ fprintf(stderr, "invalid character '%c' on line %i\n", *yytext, yylineno); exit(1); }
%%
int main() {
	yydebug = 1;
	yyparse();
	return 0;
}
void yyerror(const char *s) {
	fprintf(stderr, "yyerror(%s) line %i, token \n", s, yylineno);
}
int yywrap(void) {
	printf("done\n");
	return 1;
}
