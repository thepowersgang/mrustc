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
/* %glr-parser */

%token <::std::string*> IDENT_ LIFETIME STRING
%token <int> INTEGER CHARLIT
%token <double> FLOAT
%token <::std::string*> DOC_COMMENT SUPER_DOC_COMMENT
%token HASHBANG
%token DOUBLECOLON THINARROW FATARROW DOUBLEDOT TRIPLEDOT DOUBLEDOTEQ
%token DOUBLEEQUAL EXCLAMEQUAL DOUBLEPIPE DOUBLEAMP
%token PIPEEQUAL AMPEQUAL CARETEQUAL
%token GTEQUAL LTEQUAL
%token PLUSEQUAL MINUSEQUAL STAREQUAL SLASHEQUAL PERCENTEQUAL
%token DOUBLELT DOUBLEGT DOUBLELTEQUAL DOUBLEGTEQUAL
%token RWD_mod RWD_fn RWD_const RWD_static RWD_use RWD_struct RWD_enum RWD_trait RWD_impl RWD_type
%token RWD_as RWD_in RWD_mut RWD_ref RWD_pub RWD_where RWD_unsafe
%token RWD_let RWD_box RWD_move
%token RWD_self RWD_super
%token RWD_match RWD_if RWD_while RWD_loop RWD_for RWD_else
%token RWD_return RWD_break RWD_continue
%token RWD_extern RWD_crate

%token RWD_dyn	/* 2018 */
%token RWC_union
%token RWC_default

%type <std::string*>	IDENT
%type <Module*>	module_root
%type <int> tt_tok
%type <TokenTreeList*> tt_list
%type <TokenTree*> /* tt_item */ tt_paren tt_square tt_brace tt_subtree
%type <bool>	opt_vis vis opt_mut
%type <::std::string*>	opt_lifetime
%type <AttrList*>	super_attrs attrs attrs_p
%type <MetaItem*>	super_attr attr
%type <ItemList*>	module_body extern_items
%type <MetaItem*>	meta_item

%type <ItemPath*>	use_path
%type <MacroInv*>	macro_inv macro_braced macro_nonbrace

%type <Item*>	item macro_item vis_item unsafe_item unsafe_vis_item
%type <UseSet*>	use_def
%type <TypeAlias*>	type_def
%type <Module*>	module_def
%type <Global*>	static_def const_def
%type <Struct*>	struct_def
%type <Enum*>	enum_def
%type <Union*>	union_def
%type <Trait*>	trait_def
%type <Fn*>	fn_def fn_def_hdr fn_def_hdr_PROTO
%type <ExternCrate*>	extern_crate
%type <ExternBlock*>	extern_block
%type <Impl*>	impl_def

%type < std::vector<UseEntry_Proto>* > use_inners
%type < UseEntry_Proto* > use_inner_one
%type < std::vector<std::string>* >	ident_chain

%type <::std::string*>	extern_abi
%type <Item*>	extern_item

%debug
%define parse.error verbose

%{
// TODO: Replace this with a C++-safe printf
/*static inline*/ void bnf_trace(const ParserContext& context, const char* fmt, ...) {
	fprintf(stderr, "\x1b[32m""TRACE:%s:%i: ", context.filename.c_str(), yylineno);
	va_list	args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\x1b[0m\n");
}
/*static inline*/ void bnf_trace(const ParserContext& context, const char* fmt, const ::std::string* s) {
	bnf_trace(context, fmt, s->c_str(), '0');
}
/*static inline*/ void bnf_trace(const ParserContext& context, const char* fmt, const ::std::string& s) {
	bnf_trace(context, fmt, s.c_str(), '0');
}

%}

%start parse_root
%%

IDENT
 : IDENT_	{ $$ = $1; }
 | RWC_default	{ $$ = new std::string("default"); }
 | RWC_union	{ $$ = new std::string("union"); }
 ; 

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

super_attrs
 :	{ $$ = new AttrList(); }
 | super_attrs super_attr	{ $$ = $1; $$->push_back( consume($2) ); }
 ;

opt_vis
 : /* mt */	{ $$ = false; }
 | vis
 ;
vis
 : RWD_pub	{ $$ = true; bnf_trace(context, "public"); }
 | RWD_pub '(' use_path ')'	{ $$ = true; bnf_trace(context, "public"); }
 ;
opt_comma: | ',';
/*opt_semicolon: | ';';*/
opt_unsafe: | RWD_unsafe;
opt_lifetime: { $$ = nullptr; } | LIFETIME;

module_body
 :	{ $$ = new ::std::vector< ::std::unique_ptr<Item> >(); }
 | module_body attrs item { $3->add_attrs( consume($2) ); $1->push_back( box_raw($3) ); $$ = $1; }
 | module_body attrs macro_item { $3->add_attrs( consume($2) ); $1->push_back( box_raw($3) ); $$ = $1; }
 ;

attrs_p
 : attr	{ $$ = new AttrList(); $$->push_back( consume($1) ); }
 | attrs attr	{ $$ = $1; $$->push_back( consume($2) ); }
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
meta_item
 : IDENT tt_paren	{ $$ = new MetaItem(consume($1), consume($2)); }
 | IDENT '=' STRING	{ $$ = new MetaItem(consume($1), consume($3)); }
 | IDENT	{ $$ = new MetaItem(consume($1)); }
 ;

macro_braced
 : use_path '!' IDENT tt_brace	{ $$ = new MacroInv(consume($1), consume($3), consume($4)); }
 | use_path '!' tt_brace	{ $$ = new MacroInv(consume($1), consume($3)); }
 ;
macro_nonbrace
 : use_path '!' IDENT tt_paren	{ $$ = new MacroInv(consume($1), consume($3), consume($4)); }
 | use_path '!' IDENT tt_square	{ $$ = new MacroInv(consume($1), consume($3), consume($4)); }
 | use_path '!' tt_paren	{ $$ = new MacroInv(consume($1), consume($3)); }
 | use_path '!' tt_square	{ $$ = new MacroInv(consume($1), consume($3)); }
 ;
macro_inv : macro_braced | macro_nonbrace ';' { $$ = $1; }
macro_item
 : macro_inv	{ $$ = new Macro(consume($1)); }
 ;
/*
==========================
Root Items
==========================
*/
item
 : opt_vis vis_item	{ $$ = $2; $$->set_pub($1); }
 | RWD_impl impl_def	{ $$ = $2; }
 | vis RWD_unsafe unsafe_vis_item	{ $3->set_pub($1); $$ = $3; }
 |     RWD_unsafe unsafe_item	{ $$ = $2; }
 | RWD_extern extern_block	{ $$ = $2; }
 | RWD_extern RWD_crate extern_crate	{ $$ = $3; }
 | opt_vis RWD_extern RWD_crate extern_crate	{ $$ = $4; $$->set_pub($1); }
 ;
/* Items for which visibility is valid */
vis_item
 : RWD_mod module_def	{ $$ = $2; }
 | RWD_type type_def	{ $$ = $2; }
 | RWD_use use_def	{ $$ = $2; }
 | RWD_static static_def	{ $$ = $2; }
 | RWD_const const_def	{ $$ = $2; }
 | RWD_struct struct_def	{ $$ = $2; }
 | RWC_union union_def	{ $$ = $2; }
 | RWD_enum enum_def	{ $$ = $2; }
 | unsafe_vis_item
 ;
/* Possibily unsafe visibility item */
unsafe_vis_item
 : RWD_fn fn_def	{ $$ = $2; }
 | fn_qualifiers RWD_fn fn_def	{ $$ = $3; }
 | RWD_trait trait_def	{ $$ = $2; }
 | IDENT RWD_trait trait_def	{ if(*$1 != "auto") yyerror(context, "Expected 'auto' before `trait`"); $$ = $3; }
 ;
unsafe_item
 : unsafe_vis_item
 | RWD_impl impl_def	{ $$ = $2; }
 ;


extern_crate
 : IDENT ';'	{ $$ = new ExternCrate( consume($1) ); }
 | IDENT RWD_as IDENT ';'	{ $$ = new ExternCrate( consume($1), consume($3)); }
 ;

extern_block: extern_abi '{' extern_items '}' { $$ = new ExternBlock( consume($1), consume($3) ); };
extern_abi: { $$ = new ::std::string("C"); } | STRING;
extern_items
 :	{ $$ = new ItemList(); }
 | extern_items extern_item	{ $$ = $1; $$->push_back( box_raw($2) ); }
 ;
extern_item
 : attrs opt_vis RWD_fn fn_def_hdr ';'	{ $$ = $4; $$->set_pub($2); $$->add_attrs( consume($1) ); }
 | attrs opt_vis RWD_static opt_mut IDENT ':' type ';'	{ $$ = new Global(); $$->set_pub($2); }
 ;

module_def
 : IDENT '{' module_root '}'	{ $3->set_name( consume($1) ); $$ = new Module(consume($3)); }
 | IDENT ';'	{ bnf_trace(context, "mod %s;", $1); $$ = new Module( consume($1) ); }
 ;

/* --- Function --- */
fn_def: fn_def_hdr code	{ bnf_trace(context, "function defined"); };
fn_def_hdr: IDENT generic_def '(' fn_def_args ')' fn_def_ret where_clause	{ $$ = new Fn(); bnf_trace(context, "function '%s'", *$1); };

fn_def_hdr_PROTO: IDENT generic_def '(' fn_def_args_PROTO ')' fn_def_ret where_clause	{ $$ = new Fn(); bnf_trace(context, "function '%s'", *$1); };

fn_def_ret
 : /* -> () */
 | THINARROW type
 | THINARROW '!'
 ;

fn_def_args
 : /* empty */
 | fn_def_self
 | fn_def_self ',' fn_def_arg_list opt_comma
 | fn_def_arg_list opt_comma
 | fn_def_arg_list ',' TRIPLEDOT
 ;
fn_def_args_PROTO
 : /* empty */
 | fn_def_self
 | fn_def_self ',' fn_def_arg_list_PROTO opt_comma
 | fn_def_arg_list_PROTO opt_comma
 | fn_def_arg_list_PROTO ',' TRIPLEDOT
 ;

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
 | '_' ':' type
 | RWD_mut IDENT ':' type
 | type
 ;

fn_qualifiers
 : RWD_extern extern_abi
 | RWD_const
/* | RWD_unsafe */
 | RWD_const RWD_unsafe
 ;

/* --- Type --- */
type_def
 : IDENT generic_def '=' type ';'	{ $$ = new TypeAlias(); };

/* --- Use --- */

use_def
 : use_path ';'	{ $$ = new UseSet(consume($1)); }
 | use_path RWD_as IDENT ';'	{ $$ = new UseSet(consume($1), consume($3)); }
 | use_path RWD_as '_' ';' 	{ $$ = new UseSet(consume($1), "_"); }
 | use_path DOUBLECOLON '*' ';'	{ $$ = new UseSet(consume($1), ""); }
 | use_path DOUBLECOLON '{' use_inners opt_comma '}' ';'	{ $$ = new UseSet(consume($1), consume($4)); }
 | '{' use_inners opt_comma '}' ';'	{ $$ = new UseSet(ItemPath::new_self(), consume($2)); }	/* TODO: This depends on 2018 */
 ;
use_inners
 : use_inner_one	{ $$ = new ::std::vector<UseEntry_Proto>(); $$->push_back(consume($1)); }
 | use_inners ',' use_inner_one	{ $$ = $1; $$->push_back(consume($3)); }
 ;
use_inner_one
 : '*'	{ $$ = new UseEntry_Proto(); }
 | RWD_self	{ $$ = new UseEntry_Proto(); }
 | ident_chain	{ $$ = new UseEntry_Proto(); }
 | ident_chain RWD_as IDENT	{ $$ = new UseEntry_Proto(consume($1), consume($3)); }
 | ident_chain RWD_as '_'	{ $$ = new UseEntry_Proto(consume($1), "_"); }
 | ident_chain DOUBLECOLON '*'	{ $$ = new UseEntry_Proto(consume($1), ""); }
 | ident_chain DOUBLECOLON '{' use_inners opt_comma '}'	{ $$ = new UseEntry_Proto(consume($1), consume($4)); }
 ;
ident_chain
 : IDENT	{ $$ = new ::std::vector<std::string>(); $$->push_back(consume($1)); }
 | ident_chain DOUBLECOLON IDENT	{ $$ = $1; $$->push_back(consume($3)); }
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
 : IDENT generic_def where_clause ';'	{ $$ = new Struct(); bnf_trace(context, "unit-like struct"); }
 | IDENT generic_def '(' tuple_struct_def_items opt_comma ')' where_clause ';'	{ $$ = new Struct(); bnf_trace(context, "tuple struct"); }
 | IDENT generic_def where_clause '{' struct_def_items opt_comma '}'	{ $$ = new Struct(); bnf_trace(context, "normal struct"); }
 ;

tuple_struct_def_items
 : tuple_struct_def_items ',' tuple_struct_def_item
 | tuple_struct_def_item
 ;
tuple_struct_def_item: attrs opt_vis type;

struct_def_items
 : struct_def_items ',' struct_def_item
 | struct_def_item { bnf_trace(context, "struct_def_item"); }
 ;
struct_def_item: attrs opt_vis IDENT ':' type;

/* --- Enum --- */
enum_def: IDENT generic_def where_clause '{' enum_variants '}' { $$ = new Enum(); };
enum_variants: | enum_variant_list;
enum_variant_list: enum_variant | enum_variant ',' | enum_variant ',' enum_variant_list;
enum_variant: attrs enum_variant_;
enum_variant_
 : IDENT
 | IDENT '=' expr
 | IDENT '(' tuple_var_def_items opt_comma ')'
 | IDENT '{' struct_def_items opt_comma '}'
 ;
tuple_var_def_items
 : tuple_var_def_items ',' tuple_var_def_item
 | tuple_var_def_item
 ;
tuple_var_def_item: attrs type;

/* --- Union --- */
union_def
 : IDENT generic_def where_clause '{' struct_def_items opt_comma '}' { $$ = new Union(); }
 ;

/* --- Trait --- */
trait_def: IDENT generic_def trait_bounds where_clause '{' trait_items '}' { $$ = new Trait(); };
trait_bounds: ':' trait_bound_list | ;
trait_bound_list: trait_bound_list '+' bound | bound;

trait_items: | trait_items attrs trait_item;
trait_item
 : RWD_type IDENT trait_bounds ';'
 | RWD_type IDENT trait_bounds '=' type ';'
 | RWD_const IDENT ':' type opt_assign_value ';'
 | opt_unsafe RWD_fn fn_def_hdr_PROTO ';'
 | opt_unsafe fn_qualifiers RWD_fn fn_def_hdr_PROTO ';'
 | opt_unsafe RWD_fn fn_def_hdr_PROTO code
 | opt_unsafe fn_qualifiers RWD_fn fn_def_hdr_PROTO code
 ;
opt_assign_value: | '=' expr;

/* --- Impl --- */
impl_def: generic_def impl_def_line '{' super_attrs impl_items '}' { $$ = new Impl(); };
impl_def_line
 : trait_path RWD_for type where_clause	{ bnf_trace(context, "trait impl"); }
 | '!' trait_path RWD_for type where_clause	{ bnf_trace(context, "negative trait impl"); }
 | trait_path RWD_for DOUBLEDOT where_clause	{ bnf_trace(context, "wildcard impl"); }
 | type_noufcs where_clause	{ bnf_trace(context, "inherent impl"); }
 ;
impl_items: | impl_items attrs impl_item;

impl_item
 : macro_inv
 | RWC_default impl_item_nomac
 | opt_vis RWC_default impl_item_nomac
 | opt_vis impl_item_nomac
 ;
impl_item_nomac
 : opt_unsafe fn_qualifiers RWD_fn fn_def
 | opt_unsafe RWD_fn fn_def
 | RWD_type generic_def IDENT '=' type ';'
 | RWD_const const_def
 | RWD_const RWD_fn fn_def
 | RWD_const RWD_unsafe RWD_fn fn_def
 ;


/* Generic paramters */
generic_def : /* mt */ | '<' generic_def_list '>' { bnf_trace(context, "generic_def_list"); };
generic_def_list : generic_def_one | generic_def_list ',' generic_def_one | ;
generic_def_one
 : attrs IDENT ':' bounds '=' type
 | attrs IDENT '=' type
 | attrs IDENT ':' bounds { bnf_trace(context, "bounded ident"); }
 | attrs IDENT
 | attrs LIFETIME
 | attrs LIFETIME ':' lifetime_bounds
 ;

where_clause: | RWD_where where_clauses;
where_clauses
 : where_clause_ent ',' where_clauses
 | where_clause_ent ','
 | where_clause_ent
 ;
where_clause_ent
 : hrlb_def type ':' bounds
 | LIFETIME ':' lifetime_bounds
 ;
lifetime_bounds: LIFETIME | lifetime_bounds '+' LIFETIME;
hrlb_def: | RWD_for '<' lifetime_list '>';
lifetime_list: LIFETIME | lifetime_list ',' LIFETIME
bounds: bounds '+' bound | bound;
bound: LIFETIME | '?' trait_path | hrlb_def trait_path;

/*
=========================================
Paths
=========================================
*/
use_path
 : use_path DOUBLECOLON IDENT { $$ = $1; $$->push(consume($3)); }
 | IDENT	{ $$ = new_(ItemPath::new_relative(consume($1))); }
 | RWD_crate	{ $$ = new_(ItemPath::new_crate()); }
 | RWD_super	{ $$ = new_(ItemPath::new_super()); }
 | RWD_self	{ $$ = new_(ItemPath::new_self()); }
 | DOUBLECOLON IDENT { $$ = new_(ItemPath::new_abs()); $$->push(consume($2)); }
 ;

dlt: DOUBLELT	{ context.pushback('<'); context.pushback('<'); }

type_args
 : '<' type_exprs '>'
 | '<' type_exprs DOUBLEGT { bnf_trace(context, "Double-gt terminated type expr"); context.pushback('>'); }
 | dlt type_args
 ;

expr_path
 : ufcs_path DOUBLECOLON expr_path_segs
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
ufcs_path
 : '<' ufcs_path_tail
 | DOUBLELT ufcs_path_tail DOUBLECOLON ufcs_path_tail;
ufcs_path_tail
 : type '>'
 | type RWD_as trait_path '>'
 | type RWD_as trait_path DOUBLEGT { context.pushback('>'); }
 ;
type_path_segs
 : type_path_segs DOUBLECOLON type_path_seg
 | type_path_seg
 ;
type_path_seg
 : IDENT
 | IDENT type_args
 /*| IDENT type_args*/
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
 | RWD_dyn trait_list
 | RWD_impl trait_list
 | type_ele
 ;
type_noufcs
 : trait_list
 | RWD_dyn trait_list
 | RWD_impl trait_list
 | type_nopath
 ;
type_ele
 : type_path
 | RWD_dyn trait_path
 | RWD_impl trait_path
 | RWD_dyn '(' trait_list ')'
 | RWD_impl '(' trait_list ')'
 | type_nopath
 ;
type_nopath
 : hrlb_def opt_unsafe RWD_fn '(' fn_def_arg_list_PROTO ')' fn_def_ret
 | hrlb_def opt_unsafe RWD_extern extern_abi RWD_fn '(' fn_def_arg_list_PROTO ')' fn_def_ret
 | '_'
 | '&'  type_ele
 | '&' LIFETIME type_ele
 | DOUBLEAMP          type_ele
 | DOUBLEAMP LIFETIME type_ele
 | '&'          RWD_mut type_ele
 | '&' LIFETIME RWD_mut type_ele
 | DOUBLEAMP          RWD_mut type_ele
 | DOUBLEAMP LIFETIME RWD_mut type_ele
 | '*' RWD_const type_ele
 | '*' RWD_mut type_ele
 | '[' type ']'
 | '[' type ';' expr ']'
 | '(' ')'
 | '(' type ')'
 | '(' type ',' ')'
 | '(' type ',' type_list ')'
 | '!'
 ;
trait_list: type_path | type_path '+' trait_list_inner;
trait_list_inner: trait_list_ent | trait_list_inner '+' trait_list_ent;
trait_list_ent: trait_path | LIFETIME;
type_list: type_list ',' type | type;

/*
=========================================
Patterns
=========================================
*/
tuple_pattern: '(' ')' | '(' pattern_list ')' | '(' pattern_list ',' ')';

struct_pattern
	: expr_path '{' struct_pattern_items '}'
	| expr_path '(' pattern_list ')'
	;
struct_pattern_item
 : IDENT
 | RWD_ref IDENT
 | RWD_ref RWD_mut IDENT
 | RWD_mut IDENT
 | IDENT ':' pattern
 | DOUBLEDOT
 ;
struct_pattern_items: struct_pattern_items ',' struct_pattern_item | struct_pattern_item;

slice_pattern
 : '[' pattern_list ']'
 | '[' pattern_list ',' DOUBLEDOT ']'
 ;

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
 | value_pattern DOUBLEDOTEQ value_pattern
 | slice_pattern
 | '&' pattern
 | '&' RWD_mut pattern
 | DOUBLEAMP pattern
 | DOUBLEAMP RWD_mut pattern
 | RWD_box pattern
 ;
value_pattern
 : expr_path
 | INTEGER
 | '-' INTEGER
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
code: block	{ bnf_trace(context, "code parsed"); };

block: '{' block_contents '}';

block_contents: block_lines;
tail_expr
 : expr_noblock_NOBRACE
 | flow_control
 |
 ;
flow_control
 : RWD_return {}
 | RWD_return expr_0 {}
 | RWD_break opt_lifetime {}
 | RWD_continue opt_lifetime {}
 ;
block_lines
 :
 | attrs expr_na
 | super_attr     block_lines
 | ';' block_lines
 | attrs expr_na ';' block_lines
 | attrs expr_blocks block_lines
 | attrs block    block_lines
 | attrs item     block_lines
 | attrs RWD_let let_binding ';' block_lines
 | attrs use_path '!' IDENT tt_brace block_lines
 | attrs use_path '!' tt_brace block_lines
 | attrs use_path '!' tt_paren ';' block_lines
 | LIFETIME ':' loop_block block_lines
 ;

opt_type_annotation: | ':' type;
let_binding
 : pattern opt_type_annotation '=' expr
 | pattern opt_type_annotation
 ;

expr_list: expr_list ',' expr | expr | /* mt */;
/* expr_list_p: expr_list ',' expr | expr; */

struct_literal_ent: IDENT | IDENT ':' expr;
struct_literal_list
 : struct_literal_list ',' struct_literal_ent
 | struct_literal_ent
 ;

expr_na
 : '{' block_contents '}'
 | expr_noblock
 ;

expr
 : expr_na
 | attrs_p expr_na
 ;
expr_NOSTRLIT
 : block
 | attrs_p expr_noblock_NOSTRLIT
 |         expr_noblock_NOSTRLIT
 ;
expr_NOBRACE
 : block
 | attrs_p expr_noblock_NOBRACE
 |         expr_noblock_NOBRACE
 ;

expr_blocks
 : RWD_match expr_NOSTRLIT '{' match_arms '}'	{ }
 | RWD_if if_block
 | RWD_unsafe '{' block_contents '}' { }
 | flow_control
 | loop_block
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
match_arms: | match_arms_list match_arm_last;
match_arms_list
 :
 | match_arms_list attrs match_arm ','
 | match_arms_list attrs match_arm_brace
 ;
match_arm_last
 : attrs match_arm
 | attrs match_arm ','
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
 : match_arm_brace
 | match_arm_expr
 ;
match_arm_brace: match_patterns FATARROW '{' block_contents '}'	{ };
match_arm_expr: match_patterns FATARROW tail_expr	{ bnf_trace(context, "match_arm"); };


/* rust_expr.y.h inserted */
/* vim: ft=yacc
 */
