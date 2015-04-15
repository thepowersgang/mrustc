%token <text> IDENT LIFETIME STRING MACRO
%token <integer> INTEGER CHARLIT
%token <realnum> FLOAT 
%token SUPER_ATTR SUB_ATTR DOC_COMMENT SUPER_DOC_COMMENT
%token DOUBLECOLON THINARROW FATARROW DOUBLEDOT TRIPLEDOT
%token DOUBLEEQUAL EXCLAMEQUAL DOUBLEPIPE DOUBLEAMP
%token DOUBLELT DOUBLEGT
%token RWD_mod RWD_fn RWD_const RWD_static RWD_use RWD_struct RWD_enum RWD_trait RWD_impl RWD_type
%token RWD_as RWD_mut RWD_pub RWD_where
%token RWD_let
%token RWD_self RWD_super
%token RWD_match RWD_if RWD_while RWD_loop RWD_for RWD_else
%start crate

%union {
	char *text;
	unsigned long long	integer;
	double	realnum;
}

%debug
%error-verbose

%{
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
extern int yylineno;

static inline void bnf_trace(const char* fmt, ...) {
	fprintf(stderr, "\x1b[32m""TRACE: ");
	va_list	args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\x1b[0m\n");
}
#define YYPRINT(f,t,v)	yyprint(f,t,v)
static void yyprint(FILE *outstream, int type, YYSTYPE value)
{
	switch(type)
	{
	case IDENT: fprintf(outstream, "%s", value.text); break;
	case MACRO: fprintf(outstream, "%s!", value.text); break;
	default:
		break;
	}
}

int rustbnf_forcetoken = 0;
static inline int read_and_clear(int* ptr) {
	int rv = *ptr;
	*ptr = 0;
	return rv;
}
#define YYLEX_PARAM	read_and_clear(&rustbnf_forcetoken)
%}

%%

/*
==========================
Root
==========================
*/
crate : super_attrs module_body;

super_attrs : | super_attrs super_attr;

opt_pub
 : RWD_pub	{ bnf_trace("public"); }
 | /* mt */	{ bnf_trace("private"); }
 ;
opt_comma: | ',';

module_body
 : module_body attrs item
 | ;

attrs: attrs attr | ;

super_attr
 : SUPER_ATTR meta_items ']'
 | SUPER_DOC_COMMENT
 ;
attr
 : SUB_ATTR meta_items ']'
 | DOC_COMMENT
 ;
meta_items
 : meta_item
 | meta_items ',' meta_item;
meta_item
 : IDENT '(' meta_items ')'
 | IDENT '=' STRING
 | IDENT ;



/*
==========================
Root Items
==========================
*/
item
 : opt_pub RWD_mod module_def
 | opt_pub RWD_fn fn_def
 | opt_pub RWD_use use_def
 | opt_pub RWD_static static_def
 | opt_pub RWD_const const_def
 | opt_pub RWD_struct struct_def
 | opt_pub RWD_enum enum_def
 | opt_pub RWD_trait trait_def
 | RWD_impl impl_def
 ;

module_def
 : IDENT '{' module_body '}'
 | IDENT ';'
 ;

/* --- Function --- */
fn_def: fn_def_hdr code	{ bnf_trace("function defined"); };
fn_def_hdr: IDENT generic_def '(' fn_def_args ')' fn_def_ret where_clause	{ bnf_trace("function '%s'", $1); };

fn_def_ret: /* -> () */ | THINARROW type;

fn_def_args: /* empty */ | fn_def_self | fn_def_self ',' fn_def_arg_list | fn_def_arg_list;
fn_def_self
 : RWD_self
 | RWD_mut RWD_self
 | '&' RWD_self
 | '&' RWD_mut RWD_self
 ;
fn_def_arg_list: fn_def_arg | fn_def_arg_list ',' fn_def_arg;
fn_def_arg : irrefutable_pattern ':' type;

/* --- Use --- */
use_def
 : use_path RWD_as IDENT ';'
 | use_path '*' ';'
 | use_path '{' use_picks '}' ';'
 | use_path ';'
/* | RWD_use error ';' */
 ;
use_picks
 : use_picks ',' path_item
 | path_item
 ;
path_item: IDENT | RWD_self;


/* --- Static/Const --- */
static_def
 : IDENT ':' type '=' const_value
 | RWD_mut IDENT ':' type '=' const_value
 ;
const_def
 : IDENT ':' type '=' const_value
 ;
const_value
 : expr ';'
 | error ';' { yyerror("Syntax error in constant expression"); }
 ;

/* --- Struct --- */
struct_def
 : IDENT generic_def ';'	{ bnf_trace("unit-like struct"); }
 | IDENT generic_def '(' tuple_struct_def_items opt_comma ')'	{ bnf_trace("tuple struct"); }
 | IDENT generic_def '{' struct_def_items opt_comma '}'	{ bnf_trace("normal struct"); }
 ;

tuple_struct_def_items
 : tuple_struct_def_items ',' tuple_struct_def_item
 | tuple_struct_def_item
 ;
tuple_struct_def_item: attrs opt_pub type;

struct_def_items
 : /*struct_def_items ',' struct_def_item
 |*/ struct_def_item { bnf_trace("struct_def_item"); }
 ;
struct_def_item: attrs opt_pub IDENT ':' type;

/* --- Enum --- */
enum_def:
 ;
/* --- Trait --- */
trait_def:
 ;
/* --- Impl --- */
impl_def: impl_def_line '{' impl_items '}'
impl_def_line
 : generic_def type RWD_for type	{ bnf_trace("trait impl"); }
 | generic_def type RWD_for DOUBLEDOT	{ bnf_trace("wildcard impl"); }
 | generic_def type	{ bnf_trace("inherent impl"); }
 ;
impl_items: | impl_items attrs impl_item;
impl_item
 : opt_pub RWD_fn fn_def
 | opt_pub RWD_type generic_def IDENT '=' type ';'
 ;


/* Generic paramters */
generic_def : /* mt */ | '<' generic_def_list '>' { bnf_trace("generic_def_list"); };
generic_def_list : generic_def_one | generic_def_list ',' generic_def_one | ;
generic_def_one
 : IDENT '=' type ':' bounds
 | IDENT '=' type
 | IDENT ':' bounds { bnf_trace("bounded ident"); }
 | IDENT ;

where_clause: | RWD_where where_clauses;
where_clauses
	: where_clause_ent ',' where_clauses
	| where_clause_ent;
where_clause_ent
	: type ':' bounds;
bounds: bounds '+' bound | bound;
bound: LIFETIME | type_path;

/*
=========================================
Paths
=========================================
*/
use_path
 : use_path DOUBLECOLON IDENT
 | IDENT;

expr_path
 : ufcs_path DOUBLECOLON IDENT
 | DOUBLECOLON expr_path_segs
 | expr_path_segs
 ;
expr_path_segs
 : IDENT DOUBLECOLON '<' type_exprs '>'
 | IDENT DOUBLECOLON '<' type_exprs '>' DOUBLECOLON expr_path_segs
 | IDENT DOUBLECOLON expr_path_segs
 | IDENT
 ;

type_path
 : ufcs_path DOUBLECOLON IDENT
 | DOUBLECOLON type_path_segs
 | type_path_segs
 ;
ufcs_path: '<' type RWD_as type_path '>';
type_path_segs
 : type_path_segs DOUBLECOLON type_path_seg
 | type_path_seg
 ;
type_path_seg
 : IDENT
 | IDENT '<' type_exprs '>'
 | IDENT '<' type_exprs DOUBLEGT { bnf_trace("Double-gt terminated type expr"); rustbnf_forcetoken = '>'; } 
 ;
type_exprs: type_exprs ',' type | type;

/*
=========================================
Types
=========================================
*/
type
 : type_path
 | '&' type
 | '&' RWD_mut type
 | '*' RWD_const type
 | '*' RWD_mut type
 | '[' type ']'
 | '[' type ';' expr ']'
 ;

/*
=========================================
Patterns
=========================================
*/
irrefutable_pattern
	: tuple_pattern
	| bind_pattern
	/*| struct_pattern */
	;

tuple_pattern: '(' ')';

bind_pattern: IDENT;

struct_pattern
	: expr_path '{' '}'
	| expr_path tuple_pattern
	;

refutable_pattern
 : IDENT	{ /* maybe bind */ }
 | IDENT '@' nonbind_pattern
 | '&' refutable_pattern
 | nonbind_pattern;

nonbind_pattern
 : '_'	{ }
 | DOUBLEDOT	{ }
 | expr_path '(' refutable_pattern_list opt_comma ')'
 | expr_path '{' refutable_struct_patern '}'
 | expr_path
/* | '&' nonbind_pattern */
 ;

refutable_pattern_list: refutable_pattern_list ',' refutable_pattern | refutable_pattern;
refutable_struct_patern: /* TODO */;


/*
=========================================
Expressions!
=========================================
*/
code: '{' block_contents '}'	{ bnf_trace("code parsed"); };

block_contents: block_contents block_line | ;
block_line
 : RWD_let let_binding ';'
 | stmt
 | expr
 ;

opt_type_annotation: | ':' type;
let_binding
 : irrefutable_pattern opt_type_annotation '=' expr
 | irrefutable_pattern opt_type_annotation

stmt
 : expr ';'
 ;

expr: expr_blocks;

expr_blocks
 : RWD_match expr '{' match_arms opt_comma '}'	{ }
 | RWD_if if_block
 | expr_0
 ;
if_block
 : expr code { }
 | expr code RWD_else code { }
 | expr code RWD_else RWD_if if_block { }
 ;
match_arms
 : match_arms ',' match_arm
 | match_arm
 ;
match_arm: refutable_pattern FATARROW expr	{ bnf_trace("match_arm"); };

expr_0: expr_bor;

expr_bor: expr_band | expr_bor DOUBLEPIPE expr_band { } ;
expr_band: expr_equ | expr_band DOUBLEAMP expr_equ { } ;
expr_equ
 : expr_cmp
 | expr_equ DOUBLEEQUAL expr_cmp
 | expr_equ EXCLAMEQUAL expr_cmp
 ;
expr_cmp
 : expr_or
 | expr_cmp '<' expr_or {}
 | expr_cmp '>' expr_or {}
 ;
expr_or: expr_and | expr_or '|' expr_and { };
expr_and: expr_xor | expr_and '&' expr_xor { };
expr_xor: expr_8 | expr_xor '^' expr_8 { };
expr_8
 : expr_9
 | expr_8 DOUBLELT expr_9 {}
 | expr_8 DOUBLEGT expr_9 {}
 ;
expr_9
 : expr_cast
 | expr_9 '+' expr_cast {}
 | expr_9 '-' expr_cast {}
 ;
/* 10: Cast */
expr_cast: expr_11 | expr_cast RWD_as type { bnf_trace("expr:cast"); };
/* 11: Times/Div/Modulo */
expr_11
 : expr_value
 | expr_11 '*' expr_value {}
 | expr_11 '/' expr_value {}
 | expr_11 '%' expr_value {}
 ;

expr_value
 : CHARLIT | INTEGER | FLOAT | STRING
 | expr_path '(' expr_list ')'	{ bnf_trace("function call"); }
 | expr_path '{' struct_literal_list '}'
 | expr_path
 | RWD_self
 | '(' expr ')'
 | MACRO tt_paren	{ bnf_trace("Expr macro invocation"); }
 ;

expr_list: expr_list ',' expr | expr | /* mt */;

struct_literal_list
 : struct_literal_list IDENT ':' expr
 | struct_literal_list IDENT ':' expr ','
 | ;


tt_list: | tt_list tt_item;
tt_item: tt_paren | tt_brace | tt_square | tt_tok;
tt_tok: IDENT | '+' | '-' | '.' | '!' | RWD_self;
tt_paren: '(' tt_list ')';
tt_brace: '{' tt_list '}';
tt_square: '[' tt_list ']';

