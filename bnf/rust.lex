%{
#include "rust.tab.h"
%}

dec_digit	[0-9]
ident_c	[a-zA-Z_]

%%

{ident_c}({ident_c}|[0-9])	{ yylval.text = strdup(yytext); return IDENT; }

%%
int main() {
	yylex();
	return 0;
}
