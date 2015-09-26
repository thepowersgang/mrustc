%{
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <string>
#include <vector>
#include <memory>
#include "lex.hpp"

#include "ast_types.hpp"

#define YYPARSE_PARAM parser_context
#define YYLEX_PARAM parser_context
%}

%define api.value.type union
%define api.pure full
%parse-param {ParserContext& context}
%lex-param {*context}

%token <::std::string*> IDENT LIFETIME STRING MACRO
%token <int> INTEGER CHARLIT
%token <double> FLOAT 
%token <::std::string*> DOC_COMMENT SUPER_DOC_COMMENT
%token HASHBANG
%token DOUBLECOLON THINARROW FATARROW DOUBLEDOT TRIPLEDOT
%token DOUBLEEQUAL EXCLAMEQUAL DOUBLEPIPE DOUBLEAMP
%token PIPEEQUAL AMPEQUAL
%token GTEQUAL LTEQUAL
%token PLUSEQUAL MINUSEQUAL STAREQUAL SLASHEQUAL PERCENTEQUAL
%token DOUBLELT DOUBLEGT DOUBLELTEQUAL DOUBLEGTEQUAL
%token RWD_mod RWD_fn RWD_const RWD_static RWD_use RWD_struct RWD_enum RWD_trait RWD_impl RWD_type
%token RWD_as RWD_in RWD_mut RWD_ref RWD_pub RWD_where RWD_unsafe
%token RWD_let
%token RWD_self RWD_super
%token RWD_match RWD_if RWD_while RWD_loop RWD_for RWD_else
%token RWD_return RWD_break RWD_continue
%token RWD_extern

%type <Module*>	module_root
%type <int> tt_tok
%type <TokenTreeList*> tt_list
%type <TokenTree*> /* tt_item */ tt_paren tt_square tt_brace tt_subtree
%type <bool>	opt_pub opt_mut
%type <::std::string*>	opt_lifetime
%type <AttrList*>	super_attrs attrs
%type <MetaItem*>	super_attr attr
%type <ItemList*>	module_body extern_items
%type <MetaItem*>	meta_item
%type <MetaItems*>	meta_items

%type <Item*>	item vis_item unsafe_item unsafe_vis_item 
%type <UseSet*>	use_def
%type <TypeAlias*>	type_def
%type <Module*>	module_def
%type <Global*>	static_def const_def
%type <Struct*>	struct_def
%type <Enum*>	enum_def
%type <Trait*>	trait_def
%type <Fn*>	fn_def fn_def_hdr fn_def_hdr_PROTO
%type <ExternBlock*>	extern_block
%type <Impl*>	impl_def

%type <UseItems*>	use_def_tail
%type < ::std::vector<UseItem>* >	use_picks
%type <UseItem*>	path_item

%type <::std::string*>	extern_abi
%type <Item*>	extern_item

%debug
%error-verbose

%{
// TODO: Replace this with a C++-safe printf
/*static inline*/ void bnf_trace(const char* fmt, ...) {
	fprintf(stderr, "\x1b[32m""TRACE: ");
	va_list	args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\x1b[0m\n");
}
/*static inline*/ void bnf_trace(const char* fmt, const ::std::string* s) {
	bnf_trace(fmt, s->c_str(), '0');
}
/*static inline*/ void bnf_trace(const char* fmt, const ::std::string& s) {
	bnf_trace(fmt, s.c_str(), '0');
}

%}

%start parse_root
%%

/*
==========================
Root
==========================
*/
parse_root: module_root	{ context.output_module = box_raw($1); }

module_root: super_attrs module_body { $$ = new Module(consume($1), consume($2));  };

tt_list
 :	{ $$ = new ::std::vector<TokenTree>(); }
 | tt_list tt_subtree	{ $$ = $1; $$->push_back( consume($2) ); }
 | tt_list tt_tok	{ $$ = $1; $$->push_back( TokenTree($2) ); }
 ;
tt_subtree: tt_paren | tt_brace | tt_square;
tt_paren: '(' tt_list ')' { $$ = new TokenTree( consume($2) ); };
tt_brace: '{' tt_list '}' { $$ = new TokenTree( consume($2) ); };
tt_square: '[' tt_list ']' { $$ = new TokenTree( consume($2) ); };
/* tt_tok: SEE rust_tts.y.h */

tt_group_item: tt_paren ';' | tt_brace | tt_square ';';

super_attrs
 :	{ $$ = new AttrList(); }
 | super_attrs super_attr	{ $$ = $1; $$->push_back( consume($2) ); }
 ;

opt_pub
 : /* mt */	{ $$ = false; bnf_trace("private"); }
 | RWD_pub	{ $$ = true; bnf_trace("public"); }
 ;
opt_comma: | ',';
opt_semicolon: | ';';
opt_unsafe: | RWD_unsafe;
opt_lifetime: { $$ = nullptr; } | LIFETIME;

module_body
 :	{ $$ = new ::std::vector< ::std::unique_ptr<Item> >(); }
 | module_body attrs item { $3->add_attrs( consume($2) ); $1->push_back( box_raw($3) ); $$ = $1; }
 ;

attrs
 :	{ $$ = new AttrList(); }
 | attrs attr	{ $$ = $1; $$->push_back( consume($2) ); }
 ;

super_attr
 : HASHBANG /*'#' '!'*/ '[' meta_item ']'	{ $$ = $3; }
 | SUPER_DOC_COMMENT	{ $$ = new MetaItem("doc", consume($1)); }
 ;
attr
 : '#' '[' meta_item ']'	{ $$ = $3; }
 | DOC_COMMENT	{ $$ = new MetaItem("doc", consume($1)); }
 ;
meta_items
 : meta_item	{ $$ = new MetaItems(); $$->push_back( consume($1) ); }
 | meta_items ',' meta_item	{ $$ = $1; $$->push_back( consume($3) ); }
 ;
meta_item
 : IDENT '(' meta_items ')'	{ $$ = new MetaItem(consume($1), consume($3)); }
 | IDENT '=' STRING	{ $$ = new MetaItem(consume($1), consume($3)); }
 | IDENT	{ $$ = new MetaItem(consume($1)); }
 ;



/*
==========================
Root Items
==========================
*/
item
 : RWD_pub vis_item	{ $2->set_pub(); $$ = $2; }
 |         vis_item
 | RWD_pub RWD_unsafe unsafe_vis_item	{ $3->set_pub(); $$ = $3; }
 |         RWD_unsafe unsafe_item	{ $$ = $2; }
 | RWD_impl impl_def	{ $$ = $2; }
 | RWD_extern extern_block	{ $$ = $2; }
 | MACRO IDENT tt_brace	{ $$ = new Macro(consume($1), consume($2), consume($3)); }
 | MACRO tt_brace	{ $$ = new Macro(consume($1), consume($2)); }
 | MACRO tt_paren ';'	{ $$ = new Macro(consume($1), consume($2)); }
 ;
/* Items for which visibility is valid */
vis_item
 : RWD_mod module_def	{ $$ = $2; }
 | RWD_type type_def	{ $$ = $2; }
 | RWD_use use_def	{ $$ = $2; }
 | RWD_static static_def	{ $$ = $2; }
 | RWD_const const_def	{ $$ = $2; }
 | RWD_struct struct_def	{ $$ = $2; }
 | RWD_enum enum_def	{ $$ = $2; }
 | unsafe_vis_item
 ;
/* Possibily unsafe visibility item */
unsafe_vis_item
 : fn_qualifiers RWD_fn fn_def	{ $$ = $3; }
 | RWD_trait trait_def	{ $$ = $2; }
 ;
unsafe_item
 : unsafe_vis_item
 | RWD_impl impl_def	{ $$ = $2; }
 ;

extern_block: extern_abi '{' extern_items '}' { $$ = new ExternBlock( consume($1), consume($3) ); };
extern_abi: { $$ = new ::std::string("C"); } | STRING;
extern_items
 :	{ $$ = new ItemList(); }
 | extern_items extern_item	{ $$ = $1; $$->push_back( box_raw($2) ); }
 ;
extern_item
 : attrs opt_pub RWD_fn fn_def_hdr ';'	{ $$ = $4; if($2) $$->set_pub(); $$->add_attrs( consume($1) ); }
 ;

module_def
 : IDENT '{' module_root '}'	{ $3->set_name( consume($1) ); $$ = new Module(consume($3)); }
 | IDENT ';'	{ bnf_trace("mod %s;", $1); $$ = new Module( consume($1) ); }
 ;

/* --- Function --- */
fn_def: fn_def_hdr code	{ bnf_trace("function defined"); };
fn_def_hdr: IDENT generic_def '(' fn_def_args ')' fn_def_ret where_clause	{ $$ = new Fn(); bnf_trace("function '%s'", *$1); };

fn_def_hdr_PROTO: IDENT generic_def '(' fn_def_args_PROTO ')' fn_def_ret where_clause	{ $$ = new Fn(); bnf_trace("function '%s'", *$1); };

fn_def_ret
 : /* -> () */
 | THINARROW type
 | THINARROW '!'
 ;

fn_def_args: /* empty */ | fn_def_self | fn_def_self ',' fn_def_arg_list | fn_def_arg_list;
fn_def_args_PROTO: /* empty */ | fn_def_self | fn_def_self ',' fn_def_arg_list_PROTO | fn_def_arg_list_PROTO;

fn_def_self
 : RWD_self
 | RWD_self ':' type
 | RWD_mut RWD_self
 | RWD_mut RWD_self ':' type
 | '&' RWD_self
 | '&' LIFETIME RWD_self
 | '&' RWD_mut RWD_self
 | '&' LIFETIME RWD_mut RWD_self
 ;
fn_def_arg_list: fn_def_arg | fn_def_arg_list ',' fn_def_arg;
fn_def_arg: pattern ':' type;

fn_def_arg_list_PROTO: fn_def_arg_PROTO | fn_def_arg_list_PROTO ',' fn_def_arg_PROTO;
fn_def_arg_PROTO
 : IDENT ':' type
 | RWD_mut IDENT ':' type
 | type
 ;

fn_qualifiers
 :
 | RWD_extern extern_abi
 | RWD_const
 | RWD_unsafe
 | RWD_unsafe RWD_const
 ;

/* --- Type --- */
type_def
 : IDENT generic_def '=' type ';'	{ $$ = new TypeAlias(); };

/* --- Use --- */
use_def
 : RWD_self use_def_tail	{ $$ = new UseSet( Path(Path::TagSelf()), consume($2) ); }
 | RWD_self DOUBLECOLON use_path use_def_tail	{ $$ = new UseSet(/* TODO */); }
 | RWD_super use_def_tail	{ $$ = new UseSet( Path(Path::TagSuper()), consume($2) ); }
 | RWD_super DOUBLECOLON use_path use_def_tail	{ $$ = new UseSet(/* TODO */); }
 | DOUBLECOLON use_path use_def_tail	{ $$ = new UseSet(/* TODO */); }
 | use_path use_def_tail	{ $$ = new UseSet(/* TODO */); }
 | '{' use_picks '}' ';'	{ $$ = new UseSet( Path(Path::TagAbs()), consume($2) ); }
 ;
use_def_tail
 : RWD_as IDENT ';'	{ $$ = new UseItems(UseItems::TagRename(), consume($2) ); }
 | DOUBLECOLON '*' ';'	{ $$ = new UseItems(UseItems::TagWildcard()); }
 | DOUBLECOLON '{' use_picks '}' ';'	{ $$ = new UseItems( consume($3) ); }
 | ';'	{ $$ = new UseItems(); }
/* | RWD_use error ';' */
 ;
use_picks
 : use_picks ',' path_item	{ $$ = $1; $$->push_back( consume($3) ); }
 | path_item	{ $$ = new ::std::vector<UseItem>(); $$->push_back( consume($1) ); }
 ;
path_item
 : IDENT	{ $$ = new UseItem( consume($1) ); }
 | RWD_self	{ $$ = new UseItem(UseItem::TagSelf()); }
 ;


/* --- Static/Const --- */
opt_mut: { $$ = false; } | RWD_mut { $$ = true; };
static_def: opt_mut IDENT ':' type '=' const_value { $$ = new Global(); };
const_def: IDENT ':' type '=' const_value { $$ = new Global(); };
const_value
 : expr ';'
 | error ';' { yyerror(context, "Syntax error in constant expression"); }
 ;

/* --- Struct --- */
struct_def
 : IDENT generic_def where_clause ';'	{ $$ = new Struct(); bnf_trace("unit-like struct"); }
 | IDENT generic_def '(' tuple_struct_def_items opt_comma ')' where_clause ';'	{ $$ = new Struct(); bnf_trace("tuple struct"); }
 | IDENT generic_def where_clause '{' struct_def_items opt_comma '}'	{ $$ = new Struct(); bnf_trace("normal struct"); }
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
enum_def: IDENT generic_def where_clause '{' enum_variants '}' { $$ = new Enum(); };
enum_variants: | enum_variant_list | enum_variant_list ',';
enum_variant_list: enum_variant | enum_variant_list ',' enum_variant;
enum_variant: attrs enum_variant_;
enum_variant_
 : IDENT
 | IDENT '=' expr
 | IDENT '(' type_list ')'
 | IDENT '{' struct_def_items '}'
 ;

/* --- Trait --- */
trait_def: IDENT generic_def trait_bounds '{' trait_items '}' { $$ = new Trait(); };
trait_bounds: ':' trait_bound_list | ;
trait_bound_list: trait_bound_list '+' bound | bound;

trait_items: | trait_items attrs trait_item;
trait_item
 : RWD_type IDENT ';'
 | RWD_type IDENT ':' trait_bound_list ';'
 | RWD_type IDENT '=' type ';'
 | opt_unsafe fn_qualifiers RWD_fn fn_def_hdr_PROTO ';'
 | opt_unsafe fn_qualifiers RWD_fn fn_def_hdr_PROTO code 
 ;

/* --- Impl --- */
impl_def: impl_def_line '{' impl_items '}' { $$ = new Impl(); };
impl_def_line
 : generic_def trait_path RWD_for type where_clause	{ bnf_trace("trait impl"); }
 | generic_def '!' trait_path RWD_for type where_clause	{ bnf_trace("negative trait impl"); }
 | generic_def trait_path RWD_for DOUBLEDOT where_clause	{ bnf_trace("wildcard impl"); }
 | generic_def type where_clause	{ bnf_trace("inherent impl"); }
 ;
impl_items: | impl_items attrs impl_item;
impl_item
 : opt_pub opt_unsafe fn_qualifiers RWD_fn fn_def
 | opt_pub RWD_type generic_def IDENT '=' type ';'
 | MACRO tt_group_item
 ;


/* Generic paramters */
generic_def : /* mt */ | '<' generic_def_list '>' { bnf_trace("generic_def_list"); };
generic_def_list : generic_def_one | generic_def_list ',' generic_def_one | ;
generic_def_one
 : IDENT ':' bounds '=' type
 | IDENT '=' type
 | IDENT ':' bounds { bnf_trace("bounded ident"); }
 | IDENT
 | LIFETIME
 | LIFETIME ':' LIFETIME
 ;

where_clause: | RWD_where where_clauses;
where_clauses
 : where_clause_ent ',' where_clauses
 | where_clause_ent ','
 | where_clause_ent
 ;
where_clause_ent
 : hrlb_def type ':' bounds
 ;
hrlb_def: | RWD_for '<' lifetime_list '>';
lifetime_list: LIFETIME | lifetime_list ',' LIFETIME
bounds: bounds '+' bound | bound;
bound: LIFETIME | '?' trait_path | trait_path;

/*
=========================================
Paths
=========================================
*/
use_path
 : use_path DOUBLECOLON IDENT
 | IDENT;

dlt: DOUBLELT	{ context.pushback('<'); context.pushback('<'); }

type_args
 : '<' type_exprs '>'
 | '<' type_exprs DOUBLEGT { bnf_trace("Double-gt terminated type expr"); context.pushback('>'); } 
 | dlt type_args
 ;

expr_path
 : ufcs_path DOUBLECOLON IDENT
 | DOUBLECOLON expr_path_segs
 | RWD_self DOUBLECOLON expr_path_segs
 | RWD_super DOUBLECOLON expr_path_segs
 | expr_path_segs
 ;
expr_path_segs
 : IDENT DOUBLECOLON type_args
 | IDENT DOUBLECOLON type_args DOUBLECOLON expr_path_segs
 | IDENT DOUBLECOLON expr_path_segs
 | IDENT
 ;
expr_path_seg
 : IDENT DOUBLECOLON type_args
 | IDENT
 ;

trait_path:
 | DOUBLECOLON type_path_segs
 | RWD_super DOUBLECOLON type_path_segs
 | RWD_self DOUBLECOLON type_path_segs
 | type_path_segs
 | type_path_segs '(' type_list ')' fn_def_ret
 ;
type_path
 : ufcs_path DOUBLECOLON IDENT
 | trait_path
 ;
ufcs_path: '<' ufcs_path_tail;
ufcs_path_tail
 : type RWD_as trait_path '>'
 | type RWD_as trait_path DOUBLEGT { context.pushback('>'); }
 ; 
type_path_segs
 : type_path_segs DOUBLECOLON type_path_seg
 | type_path_seg
 ;
type_path_seg
 : IDENT
 | IDENT type_args
 | IDENT type_args
 ;
type_exprs: type_exprs ',' type_arg | type_arg;
type_arg: type | LIFETIME | IDENT '=' type;

/*
=========================================
Types
=========================================
*/
type
 : trait_list
 | type_ele
 ;
type_ele
 : type_path
 | RWD_fn '(' type_list ')' fn_def_ret
 | '_'
 | '&' opt_lifetime type_ele
 | DOUBLEAMP opt_lifetime type_ele
/* | '&' LIFETIME type_ele */
 | '&' opt_lifetime RWD_mut type_ele
 | DOUBLEAMP opt_lifetime RWD_mut type_ele
/* | '&' LIFETIME RWD_mut type_ele */
 | '*' RWD_const type_ele
 | '*' RWD_mut type_ele
 | '[' type ']'
 | '[' type ';' expr ']'
 | '(' ')'
 | '(' type ')'
 | '(' type ',' ')'
 | '(' type ',' type_list ')'
 ;
trait_list: type_path '+' trait_list_inner;
trait_list_inner: trait_list_ent | trait_list_inner '+' trait_list_ent;
trait_list_ent: trait_path | LIFETIME;
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
 |*/ IDENT '@' nonbind_pattern
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
code: block	{ bnf_trace("code parsed"); };

block: '{' block_contents '}';

block_contents
 : block_lines
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
 | super_attr
 | attrs item
 | expr_blocks
 | stmt
 | LIFETIME ':' loop_block
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
 : RWD_match expr_NOSTRLIT '{' match_arms '}'	{ }
 | RWD_if if_block
 | RWD_unsafe '{' block_contents '}' { }
/* | flow_control */
 | loop_block
 | block
 ;
loop_block
 : RWD_loop '{' block_contents '}' { }
 | RWD_while expr_NOSTRLIT '{' block_contents '}' { }
 | RWD_while RWD_let pattern '=' expr_NOSTRLIT '{' block_contents '}' { }
 | RWD_for pattern RWD_in expr_NOSTRLIT '{' block_contents '}' { }
 ;

if_block
 : if_block_head {}
 | if_block_head RWD_else code { }
 | if_block_head RWD_else RWD_if if_block { }
 ;
if_block_head
 : expr_NOSTRLIT code {}
 | RWD_let pattern '=' expr_NOSTRLIT code {}
 ;
match_arms: match_arms_list match_arm_last;
match_arms_list
 :
 | match_arms_list match_arm ','
 | match_arms_list match_arm_brace
 ;
match_arm_last
 : match_arm 
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
 : match_arm_expr
 | match_arm_brace
 ;
match_arm_brace : match_patterns FATARROW '{' block_contents '}'	{ };
match_arm_expr: match_patterns FATARROW tail_expr	{ bnf_trace("match_arm"); };


/* rust_expr.y.h inserted */
/* vim: ft=yacc
 */
