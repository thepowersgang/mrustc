
%option yylineno

%{
#include "rust.tab.h"
#include <stdio.h>
void yyerror(const char *s);
extern int yydebug;

int rustbnf_forcetoken = 0;
#define YY_DECL	int yylex_inner()

int yylex() {
	if(rustbnf_forcetoken>0) {
		int rv = rustbnf_forcetoken;
		rustbnf_forcetoken = 0;
		return rv;
	}
	else {
		return yylex_inner();
	}
}


%}

dec_digit	[0-9_]
ident_c	[a-zA-Z_]

%%

"//"[^/].*\n	{ }
"///".*\n	{ /* TODO: Handle /// by desugaring */ }
"/*"[^*]([^(\*/)])*"*/"	{ }
"/**"([^(\*/)])*"*/"	{ /* TODO: handle / ** by desugaring */ }
\n	/* */
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
"in"	{ return RWD_in; }
"mut"	{ return RWD_mut; }
"pub"	{ return RWD_pub; }
"where"	{ return RWD_where; }
"extern"	{ return RWD_extern; }

"let"	{ return RWD_let; }
"ref"	{ return RWD_ref; }

"self"	{ return RWD_self; }
"super"	{ return RWD_super; }

"match"	{ return RWD_match; }
"if"	{ return RWD_if; }
"else"	{ return RWD_else; }
"loop"	{ return RWD_loop; }
"while"	{ return RWD_while; }
"for"	{ return RWD_for; }
"unsafe" { return RWD_unsafe; }
"return" { return RWD_return; }
"break" { return RWD_break; }
"continue" { return RWD_continue; }

"::"	{ return DOUBLECOLON; }
"->"	{ return THINARROW; }
"=>"	{ return FATARROW; }

"=="	{ return DOUBLEEQUAL; }
"!="	{ return EXCLAMEQUAL; }
">="	{ return GTEQUAL; }
"<="	{ return LTEQUAL; }
"+="	{ return PLUSEQUAL; }
"-="	{ return MINUSEQUAL; }
"*="	{ return STAREQUAL; }
"/="	{ return SLASHEQUAL; }

"&&"	{ return DOUBLEAMP; }
"||"	{ return DOUBLEPIPE; }
"<<"	{ return DOUBLELT; }
">>"	{ return DOUBLEGT; }
".."	{ return DOUBLEDOT; }
"..."	{ return TRIPLEDOT; }

"#"	{ return *yytext; }
"$"	{ return *yytext; }
"&"	{ return *yytext; }
"|"	{ return *yytext; }
"!"	{ return *yytext; }
"."	{ return *yytext; }
":"	{ return *yytext; }
";"	{ return *yytext; }
"="	{ return *yytext; }
"{"|"}"	{ return *yytext; }
"("|")"	{ return *yytext; }
"["|"]"	{ return *yytext; }
"<"	{ return *yytext; }
">"	{ return *yytext; }
","	{ return *yytext; }
"/"	{ return *yytext; }
"*"	{ return *yytext; }
"+"	{ return *yytext; }
"-"	{ return *yytext; }

{ident_c}({ident_c}|[0-9])*	{ yylval.text = strdup(yytext); return IDENT; }
{ident_c}({ident_c}|[0-9])*"!"	{ yylval.text = strdup(yytext); return MACRO; }
'{ident_c}{ident_c}*	{ yylval.text = strdup(yytext+1); return LIFETIME; }
[0-9]{dec_digit}*"."{dec_digit}+	{ yylval.realnum = strtod(yytext, NULL); return FLOAT; }
[0-9]{dec_digit}*	{ yylval.integer = strtoull(yytext, NULL, 0); return INTEGER; }
0x[0-9a-fA-F]*	{ yylval.integer = strtoull(yytext, NULL, 0); return INTEGER; }

b?'(.|\\['rn])'	{ yylval.text = strdup(yytext); return CHARLIT; }
\"([^"])+\"	{ yylval.text = strdup(yytext); return STRING; }

.	{ fprintf(stderr, "\x1b[31m" "ERROR: Invalid character '%c' on line %i\x1b[0m\n", *yytext, yylineno); exit(1); }

%%
int main() {
	yylineno = 1;
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
