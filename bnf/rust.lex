%{
#include "ast_types.hpp"
#include "lex.hpp"
#include <stdio.h>
%}

%option yylineno
%option noyywrap batch

%{
int rustbnf_forcetoken = 0;
//#define YY_DECL	yy::parser::symbol_type yylex_inner()
#define YY_DECL	int yylex_inner(YYSTYPE* lvalp, ParserContext& context)

YY_DECL;
// Wrap the real yylex with one that can yeild a pushbacked token
int yylex(YYSTYPE* lvalp, ParserContext& context) {
	int rv = context.popback();
	if(rv > 0) {
		printf("--return %i\n", rv);
		return rv;
	}
	else {
		return yylex_inner(lvalp, context);
	}
}

void handle_block_comment();
::std::string parse_escaped_string(const char* s);
::std::string handle_raw_string(const char* s);

%}

dec_digit	[0-9_]
ident_c	[a-zA-Z_]
int_suffix	([ui](size|8|16|32|64))?

%%

"//".*\n	{ if(yytext[2] == '/' && yytext[3] != '/') { lvalp->DOC_COMMENT = new ::std::string(yytext+1, strlen(yytext)-2); return DOC_COMMENT; } }
"//!".*\n	{ lvalp->SUPER_DOC_COMMENT = new ::std::string(yytext+1, strlen(yytext)-2); return SUPER_DOC_COMMENT; }
"/*"	{ handle_block_comment(); /* TODO: Handle doc comments */ }
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
"crate"	{ return RWD_crate; }

"let"	{ return RWD_let; }
"ref"	{ return RWD_ref; }
"move"	{ return RWD_move; }
"box"	{ return RWD_box; }

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

"dyn" { return RWD_dyn; }

"default" { return RWC_default; }
"union" { return RWC_union; }

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
"%="	{ return PERCENTEQUAL; }

"|="	{ return PIPEEQUAL; }
"&="	{ return AMPEQUAL; }
"^="	{ return CARETEQUAL; }

"&&"	{ return DOUBLEAMP; }
"||"	{ return DOUBLEPIPE; }
"<<"	{ return DOUBLELT; }
">>"	{ return DOUBLEGT; }
"<<="	{ return DOUBLELTEQUAL; }
">>="	{ return DOUBLEGTEQUAL; }
".."	{ return DOUBLEDOT; }
"..."	{ return TRIPLEDOT; }
"..="	{ return DOUBLEDOTEQ; }

"#!"	{ return HASHBANG; }

"?"	{ return *yytext; }
"#"	{ return *yytext; }
"@"	{ return *yytext; }
"$"	{ return *yytext; }
"&"	{ return *yytext; }
"|"	{ return *yytext; }
"^"	{ return *yytext; }
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
"%"	{ return *yytext; }
"*"	{ return *yytext; }
"+"	{ return *yytext; }
"-"	{ return *yytext; }

{ident_c}({ident_c}|[0-9])*	{
	if(*yytext == '_' && yytext[1] == 0)
		return '_';
	else {
		lvalp->IDENT_ = new ::std::string( yytext );
		return IDENT_;
	}
	}
[0-9]{dec_digit}*"."{dec_digit}+(e[+\-]?{dec_digit}+)?(f32|f64)?	{ lvalp->FLOAT = strtod(yytext, NULL); return FLOAT; }
[0-9]{dec_digit}*e[+\-]?{dec_digit}+(f32|f64)?	{ lvalp->FLOAT = strtod(yytext, NULL); return FLOAT; }
[0-9]{dec_digit}*\.[^a-zA-Z\.]	{ auto len = strlen(yytext); yyless(len-1); lvalp->FLOAT = strtod(yytext, NULL); return FLOAT; }
[0-9]{dec_digit}*(f32|f64)	{ lvalp->FLOAT = strtod(yytext, NULL); return FLOAT; }
[0-9]{dec_digit}*{int_suffix}	{ lvalp->INTEGER = strtoull(yytext, NULL, 0); return INTEGER; }
0x[0-9a-fA-F_]+{int_suffix}	{ lvalp->INTEGER = strtoull(yytext, NULL, 0); return INTEGER; }
0o[0-7]+{int_suffix}	{ lvalp->INTEGER = strtoull(yytext, NULL, 0); return INTEGER; }
0b[01_]+{int_suffix}	{ lvalp->INTEGER = strtoull(yytext, NULL, 0); return INTEGER; }
'{ident_c}{ident_c}*	{ lvalp->LIFETIME = new ::std::string(yytext, 1); return LIFETIME; }

b?'(.|\\'|\\[^']+|[\x80-\xFF]*)'	{ lvalp->CHARLIT = yytext[0]; return CHARLIT; }
b?\"(\\.|[^\\"]|\\\n)*\"	{ lvalp->STRING = new ::std::string( parse_escaped_string(yytext) ); return STRING; }
b?r#*\"	{ auto rs = handle_raw_string( (*yytext=='b' ? yytext+2 : yytext+1) ); lvalp->STRING = new ::std::string(rs); return STRING; }

.	{ fprintf(stderr, "\x1b[31m" "ERROR: %s:%d: Invalid character '%c'\x1b[0m\n", context.filename.c_str(), yylineno, *yytext); exit(1); }

%%
uint32_t parse_char_literal(const char *_s) {
	const uint8_t* s = (const uint8_t*)_s;

	assert(*s++ == '\'');
	uint32_t rv = 0;

	if( *s == '\\' ) {
		s ++;
		switch(*s)
		{
		case 'n':	rv = '\0';	break;
		case 'r':	rv = '\0';	break;
		case 'x':
			rv = strtoul((const char*)(s+1), NULL, 16);
			s += 2;
			break;
		//case 'u':
		//	rv = strtoul((const char*)(s+1), NULL, 16);
		//	s += 2;
		//	break;
		default:
			return 0;
		}
	}
	else if( *s < 0x80 ) {
		rv = *s;
	}
	else {
		fprintf(stderr, "TODO: UTF-8 char literals");
		exit(1);
	}
	s ++;
	if( *s != '\'' ) {
		exit(1);
	}
	assert(*s == '\0');
	return rv;
}

::std::string parse_escaped_string(const char* s) {
	printf("parse_escaped_string(%s)\n", s);
	if( *s == 'b' ) {
		s ++;
	}
	assert(*s++ == '"');

	::std::string rv;

	for( ; *s != '"'; s ++ )
	{
		if( *s == '\\' )
		{
			s ++;
			switch(*s)
			{
			case 'n':	rv += '\n';	break;
			case 'r':	rv += '\r';	break;
			case 't':	rv += '\t';	break;
			case '"':	rv += '"';	break;
			case '0':	rv += '\0';	break;
			case '\\':	rv += '\\';	break;
			case '\'':	rv += '\'';	break;
			case '\n':
				while( isblank(*s) )
					s ++;
				s --;
				break;
			case 'x':
				rv += (char)strtoul((const char*)(s+1), NULL, 16);
				s += 2;
				break;
			case 'u': {
				char *out;
				assert(s[1] == '{');
				rv += (char)strtoul((const char*)(s+2), &out, 16);
				s  = out;
				assert(*s == '}');
				break; }
			default:
				fprintf(stderr, "Unknown escape code '\\%c' in string\n", *s);
				exit(1);
			}
		}
		else if( *s == '\0' ) {
			// wut?
			fprintf(stderr, "Unexpected EOS\n");
			exit(1);
		}
		else {
			rv += *s;
		}
	}
	assert(*s++ == '"');
	assert(*s == '\0');
	return rv;
}

// Thanks stackoverflow: http://www.lysator.liu.se/c/ANSI-C-grammar-l.html
void handle_block_comment() {
    char c, c1;

loop:
    while ((c = yyinput()) != '*' && c != 0) {
    //    putchar(c);
    }

    if ((c1 = yyinput()) != '/' && c != 0) {
        unput(c1);
        goto loop;
    }

    //if (c != 0)
    //    putchar(c1);
}

::std::string handle_raw_string(const char* s) {
	 int	num_hash = 0;
	for(; *s == '#'; s++)
		num_hash ++;
	assert(*s == '"');
	printf("handle_raw_string - num_hash=%i\n", num_hash);

	::std::string rv;

	for(;;)
	{
		char	c;
		if( (c = yyinput()) == '"' ) {
			if( num_hash == 0 )
				break;
			int i;
			for(i = 0; i < num_hash; i ++) {
				if( (c = yyinput()) != '#' )
					break;
			}
			// Found `num_hash` '#' characters in a row, break out
			if( i == num_hash ) {
				break;
			}
			unput(c);
			// Didn't find enough, append to output
			rv += '"';
			while(i--)	rv += '#';

		}
		else if( c <= 0 ) {
			break;
		}
		else {
			rv += c;
		}
	}

	return rv;
}
