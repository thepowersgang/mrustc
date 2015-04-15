%{
#include "rust.tab.h"
#include <stdio.h>
void yyerror(const char *s);
extern int yydebug;

#define YY_DECL	int yylex(int force_ret)

#define YY_USER_ACTION	if(force_ret>0)	return force_ret;

%}

dec_digit	[0-9_]
ident_c	[a-zA-Z_]

%%

"//"[^/].*\n	{ yylineno += 1; }
"///".*\n	{ yylineno += 1; }	// TODO: Handle /// by desugaring
\n	{ yylineno += 1; }
\r	/* */
[ \t]	/* */


"mod"	{ return RWD_mod; }
"impl"	{ return RWD_impl; }
"use"	{ return RWD_use; }
"type"	{ return RWD_type; }
"static" { return RWD_static; }
"const"	{ return RWD_const; }
"struct" { return RWD_struct; }
"trait"	{ return RWD_trait; }
"enum"	{ return RWD_enum; }

"fn"	{ return RWD_fn; }
"as"	{ return RWD_as; }
"mut"	{ return RWD_mut; }
"pub"	{ return RWD_pub; }

"let"	{ return RWD_let; }

"self"	{ return RWD_self; }
"super"	{ return RWD_super; }

"match"	{ return RWD_match; }
"if"	{ return RWD_if; }
"else"	{ return RWD_else; }
"loop"	{ return RWD_loop; }
"while"	{ return RWD_while; }
"for"	{ return RWD_for; }

"::"	{ return DOUBLECOLON; }
"->"	{ return THINARROW; }
"=>"	{ return FATARROW; }
"#!["	{ return SUPER_ATTR; }

"=="	{ return DOUBLEEQUAL; }
"!="	{ return EXCLAMEQUAL; }

"&&"	{ return DOUBLEAMP; }
"||"	{ return DOUBLEPIPE; }
"<<"	{ return DOUBLELT; }
">>"	{ return DOUBLEGT; }

"&"	{ return *yytext; }
"|"	{ return *yytext; }
"!"	{ return *yytext; }
"."	{ return *yytext; }
":"	{ return *yytext; }
";"	{ return *yytext; }
"="	{ return *yytext; }
"{"|"}"	{ return *yytext; }
"("|")"	{ return *yytext; }
"<"	{ return *yytext; }
">"	{ return *yytext; }
","	{ return *yytext; }

{ident_c}({ident_c}|[0-9])*	{ yylval.text = strdup(yytext); return IDENT; }
{ident_c}({ident_c}|[0-9])*"!"	{ yylval.text = strdup(yytext); return MACRO; }
[0-9]{dec_digit}*"."{dec_digit}+	{ yylval.realnum = strtod(yytext, NULL); return FLOAT; }
[0-9]{dec_digit}*	{ yylval.integer = strtoull(yytext, NULL, 0); return INTEGER; }
0x[0-9a-fA-F]*	{ yylval.integer = strtoull(yytext, NULL, 0); return INTEGER; }

'(\\.|[^\\'])+'	{ return CHARLIT; }

.	{ fprintf(stderr, "\x1b[31m" "ERROR: Invalid character '%c' on line %i\x1b[0m\n", *yytext, yylineno); exit(1); }

%%
int main() {
	yydebug = (getenv("BNFDEBUG") != NULL);
	yyparse();
	return 0;
}
void yyerror(const char *s) {
	fprintf(stderr, "\x1b[31mERROR: ?:%d: yyerror(%s)\x1b[0m\n", yylineno, s);
}
int yywrap(void) {
	printf("done\n");
	return 1;
}
