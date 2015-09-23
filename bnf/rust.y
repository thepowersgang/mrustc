%token <text> IDENT LIFETIME STRING MACRO
%token <integer> INTEGER CHARLIT
%token <realnum> FLOAT 
%token DOC_COMMENT SUPER_DOC_COMMENT
%token DOUBLECOLON THINARROW FATARROW DOUBLEDOT TRIPLEDOT
%token DOUBLEEQUAL EXCLAMEQUAL DOUBLEPIPE DOUBLEAMP
%token GTEQUAL LTEQUAL
%token PLUSEQUAL MINUSEQUAL STAREQUAL SLASHEQUAL
%token DOUBLELT DOUBLEGT
%token RWD_mod RWD_fn RWD_const RWD_static RWD_use RWD_struct RWD_enum RWD_trait RWD_impl RWD_type
%token RWD_as RWD_in RWD_mut RWD_ref RWD_pub RWD_where RWD_unsafe
%token RWD_let
%token RWD_self RWD_super
%token RWD_match RWD_if RWD_while RWD_loop RWD_for RWD_else
%token RWD_return RWD_break RWD_continue
%token RWD_extern
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

/*static inline*/ void bnf_trace(const char* fmt, ...) {
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
	case STRING: fprintf(outstream, "\"%s\"", value.text); break;
	case LIFETIME: fprintf(outstream, "'%s", value.text); break;
	default:
		break;
	}
}

extern int rustbnf_forcetoken;
%}

%%

/*
==========================
Root
==========================
*/
crate : super_attrs module_body;

tt_list: | tt_list tt_item;
tt_item: tt_paren | tt_brace | tt_square | tt_tok;
tt_tok
 : IDENT | STRING | CHARLIT | LIFETIME | INTEGER | MACRO
 | '+' | '*' | '/' | ',' | ';' | '#'
 | RWD_self | RWD_super | RWD_mut | RWD_ref | RWD_let | RWD_where
 | RWD_for | RWD_while | RWD_loop | RWD_if | RWD_else | RWD_match | RWD_return
 | RWD_impl | RWD_pub | RWD_struct | RWD_enum | RWD_fn | RWD_type | RWD_static | RWD_const
 | '!' | EXCLAMEQUAL
 | '-' | THINARROW
 | '&' | DOUBLEAMP
 | ':' | DOUBLECOLON
 | '|' | DOUBLEPIPE
 | '=' | DOUBLEEQUAL | FATARROW
 | '<' | DOUBLELT | LTEQUAL
 | '>' | DOUBLEGT | GTEQUAL
 | '.' | DOUBLEDOT | TRIPLEDOT
 | '$'
 ;
tt_paren: '(' tt_list ')';
tt_brace: '{' tt_list '}';
tt_square: '[' tt_list ']';

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
 : '#' '!' '[' meta_items ']'
 | SUPER_DOC_COMMENT
 ;
attr
 : '#' '[' meta_items ']'
 | DOC_COMMENT
 ;
meta_items: meta_item | meta_items ',' meta_item;
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
 | opt_pub fn_qualifiers RWD_fn fn_def
 | opt_pub RWD_use use_def
 | opt_pub RWD_static static_def
 | opt_pub RWD_const const_def
 | opt_pub RWD_struct struct_def
 | opt_pub RWD_enum enum_def
 | opt_pub RWD_trait trait_def
 | RWD_extern extern_block
 | RWD_impl impl_def
 | MACRO IDENT tt_brace
 | MACRO tt_brace
 | MACRO tt_paren ';'
 ;

extern_block: extern_abi | '{' extern_items '}';
extern_abi: | STRING;
extern_items: | extern_items extern_item;
extern_item: opt_pub RWD_fn fn_def_hdr ';';

module_def
 : IDENT '{' module_body '}'
 | IDENT ';'	{ bnf_trace("mod %s;", $1); }
 ;

/* --- Function --- */
fn_def: fn_def_hdr code	{ bnf_trace("function defined"); };
fn_def_hdr: IDENT generic_def '(' fn_def_args ')' fn_def_ret where_clause	{ bnf_trace("function '%s'", $1); };

fn_def_ret: /* -> () */ | THINARROW type | THINARROW '!';

fn_def_args: /* empty */ | fn_def_self | fn_def_self ',' fn_def_arg_list | fn_def_arg_list;
fn_def_self
 : RWD_self
 | RWD_mut RWD_self
 | '&' RWD_self
 | '&' LIFETIME RWD_self
 | '&' RWD_mut RWD_self
 | '&' LIFETIME RWD_mut RWD_self
 ;
fn_def_arg_list: fn_def_arg | fn_def_arg_list ',' fn_def_arg;
fn_def_arg : pattern ':' type;

fn_qualifiers
 :
 | RWD_extern extern_abi
 | RWD_unsafe
 | RWD_const
 | RWD_unsafe RWD_const
 ;

/* --- Use --- */
use_def
 : RWD_self use_def_tail
 | RWD_self DOUBLECOLON use_path use_def_tail
 | RWD_super use_def_tail
 | RWD_super DOUBLECOLON use_path use_def_tail
 | DOUBLECOLON use_path use_def_tail
 | use_path use_def_tail
 | '{' use_picks '}' ';'
 ;
use_def_tail
 : RWD_as IDENT ';'
 | DOUBLECOLON '*' ';'
 | DOUBLECOLON '{' use_picks '}' ';'
 | ';'
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
 | IDENT generic_def '(' tuple_struct_def_items opt_comma ')' ';'	{ bnf_trace("tuple struct"); }
 | IDENT generic_def '{' struct_def_items opt_comma '}'	{ bnf_trace("normal struct"); }
 ;

tuple_struct_def_items
 : tuple_struct_def_items ',' tuple_struct_def_item
 | tuple_struct_def_item
 ;
tuple_struct_def_item: attrs opt_pub type;

struct_def_items
 : struct_def_items ',' struct_def_item
 | struct_def_item { bnf_trace("struct_def_item"); }
 ;
struct_def_item: attrs opt_pub IDENT ':' type;

/* --- Enum --- */
enum_def:
 ;
/* --- Trait --- */
trait_def: IDENT generic_def trait_bounds '{' trait_items '}';
trait_bounds: ':' type_path | ;
trait_bound_list: trait_bound_list '+' trait_bound | trait_bound;
trait_bound: type_path | LIFETIME;
trait_items: | trait_items attrs trait_item;
trait_item
 : RWD_type IDENT ';'
 | RWD_type IDENT ':' trait_bound_list ';'
 | fn_qualifiers RWD_fn fn_def_hdr ';'
 ;

/* --- Impl --- */
impl_def: impl_def_line '{' impl_items '}'
impl_def_line
 : generic_def type RWD_for type where_clause	{ bnf_trace("trait impl"); }
 | generic_def type RWD_for DOUBLEDOT where_clause	{ bnf_trace("wildcard impl"); }
 | generic_def type where_clause	{ bnf_trace("inherent impl"); }
 ;
impl_items: | impl_items attrs impl_item;
impl_item
 : opt_pub fn_qualifiers RWD_fn fn_def
 | opt_pub RWD_type generic_def IDENT '=' type ';'
 ;


/* Generic paramters */
generic_def : /* mt */ | '<' generic_def_list '>' { bnf_trace("generic_def_list"); };
generic_def_list : generic_def_one | generic_def_list ',' generic_def_one | ;
generic_def_one
 : IDENT '=' type ':' bounds
 | IDENT '=' type
 | IDENT ':' bounds { bnf_trace("bounded ident"); }
 | IDENT
 | LIFETIME
 | LIFETIME ':' LIFETIME
 ;

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
 | RWD_self DOUBLECOLON expr_path_segs
 | RWD_super DOUBLECOLON expr_path_segs
 | expr_path_segs
 ;
expr_path_segs
 : IDENT DOUBLECOLON '<' type_exprs '>'
 | IDENT DOUBLECOLON '<' type_exprs '>' DOUBLECOLON expr_path_segs
 | IDENT DOUBLECOLON expr_path_segs
 | IDENT
 ;
expr_path_seg
 : IDENT DOUBLECOLON '<' type_exprs '>'
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
type_exprs: type_exprs ',' type_arg | type_arg;
type_arg: type | LIFETIME | IDENT '=' type;

/*
=========================================
Types
=========================================
*/
type
 : type_path
 | '&' type
 | '&' LIFETIME type
 | '&' RWD_mut type
 | '&' LIFETIME RWD_mut type
 | '*' RWD_const type
 | '*' RWD_mut type
 | '[' type ']'
 | '[' type ';' expr ']'
 | '(' ')'
 | '(' type ')'
 | '(' type ',' ')'
 | '(' type ',' type_list ')'
 ;
type_list: type_list ',' type | type;

/*
=========================================
Patterns
=========================================
*/
tuple_pattern: '(' pattern_list ')' | '(' pattern_list ',' ')';

struct_pattern
	: expr_path '{' struct_pattern_items '}'
	| expr_path tuple_pattern
	;
struct_pattern_item: IDENT | IDENT ':' pattern;
struct_pattern_items: struct_pattern_items ',' struct_pattern_item | struct_pattern_item;

pattern
 : /*IDENT	{ /* maybe bind * / }
 */| IDENT '@' nonbind_pattern
 | RWD_ref IDENT
 | RWD_ref IDENT '@' nonbind_pattern
 | RWD_mut IDENT
 | RWD_mut IDENT '@' nonbind_pattern
 | RWD_ref RWD_mut IDENT
 | RWD_ref RWD_mut IDENT '@' nonbind_pattern
 | nonbind_pattern
 ;

nonbind_pattern
 : '_'	{ }
 | DOUBLEDOT	{ }
 | struct_pattern
 | tuple_pattern
 | value_pattern
 | value_pattern TRIPLEDOT value_pattern
 | '&' pattern
 | '&' RWD_mut pattern
 ;
value_pattern
 : expr_path
 | INTEGER
 | CHARLIT
 | STRING
 ;

pattern_list
 : pattern_list ',' pattern
 | pattern
 ;


/*
=========================================
Expressions!
=========================================
*/
code: '{' block_contents '}'	{ bnf_trace("code parsed"); };

block_contents
 :
 | block_lines
 | block_lines tail_expr
 ;
tail_expr
 : expr
 | flow_control
 ;
flow_control
 : RWD_return opt_semicolon {}
 | RWD_return expr_0 opt_semicolon {}
 | RWD_break opt_lifetime opt_semicolon {}
 | RWD_continue opt_lifetime opt_semicolon {}
 ;
block_lines: | block_lines block_line;
block_line
 : RWD_let let_binding ';'
 | MACRO IDENT tt_brace
 | attrs item
 | expr_blocks
 | stmt
 ;

opt_type_annotation: | ':' type;
let_binding
 : pattern opt_type_annotation '=' expr
 | pattern opt_type_annotation

stmt
 : expr ';'
 ;

expr_list: expr_list ',' expr | expr | /* mt */;

struct_literal_ent: IDENT | IDENT ':' expr;
struct_literal_list
 : struct_literal_list ',' struct_literal_ent
 | struct_literal_ent
 ;

expr_blocks
 : RWD_match expr_NOSTRLIT '{' match_arms opt_comma '}'	{ }
 | RWD_if if_block
 | RWD_unsafe '{' block_contents '}' { }
 | RWD_loop '{' block_contents '}' { }
 | RWD_while expr_NOSTRLIT '{' block_contents '}' { }
 | RWD_for pattern RWD_in expr_NOSTRLIT '{' block_contents '}' { }
 | flow_control
 | '{' block_contents '}'
 ;
opt_lifetime: | LIFETIME;
opt_semicolon: | ';';

if_block
 : if_block_head {}
 | if_block_head RWD_else code { }
 | if_block_head RWD_else RWD_if if_block { }
 ;
if_block_head
 : expr_NOSTRLIT code {}
 | RWD_let pattern '=' expr_NOSTRLIT code {}
 ;
match_arms
 : match_arm ',' match_arms
 | match_arm_brace match_arms
 | match_arm
 | match_arm ','
 ;
match_pattern
 : pattern
 | pattern RWD_if expr_0
 ;
match_patterns
 : match_patterns '|' match_pattern
 | match_pattern
 ;
match_arm
 : match_patterns FATARROW expr	{ bnf_trace("match_arm"); }
 | match_arm_brace
 ;
match_arm_brace : match_patterns FATARROW '{' block_contents '}';


/* rust_expr.y.h inserted */
