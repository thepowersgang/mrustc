%token IDENT LIFETIME STRING INTEGER FLOAT 
%token SUPER_ATTR SUB_ATTR
%token DOUBLECOLON
%token RWD_mod RWD_fn RWD_const RWD_static RWD_use
%token RWD_as RWD_mut RWD_where
%token RWD_self RWD_super
%start crate

%union {
	char *text;
}

%debug

%%

/*
==========================
Root
==========================
*/
crate : super_attrs module_body;

super_attrs
 : super_attrs super_attr
 | ;

module_body
 : attrs item module_body
 | ;

attrs
 : attrs attr
 | ;

super_attr : SUPER_ATTR meta_items ']'
attr : SUB_ATTR meta_items ']';
meta_items: meta_item | meta_items ',' meta_item | ;
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
 : RWD_mod module_def
 | RWD_fn fn_def
 | RWD_use use_def
 | RWD_static static_def
 ;
/* | struct_def
 | enum_def
 | trait_def
 ;*/

module_def
 : IDENT '{' module_body '}'
 | IDENT ';'
 ;

fn_def: IDENT generic_def '(' fn_def_args ')' fn_def_ret where_clause '{' code '}';

fn_def_ret
 : '-' '>' type
 | ;

fn_def_args : fn_def_args fn_def_arg | ;
fn_def_arg : irrefutable_pattern ':' type;

use_def
 : path RWD_as IDENT ';'
 | path '*' ';'
 | path '{' use_picks '}' ';'
 | path ';'
/* | RWD_use error ';' */
 ;
use_picks
 : use_picks ',' path_item
 | path_item
 ;
path_item: IDENT | RWD_self;


static_def
 : IDENT ':' type '=' expr ';'
 | RWD_mut IDENT ':' type '=' expr ';'
 ;

/* Generic paramters */
generic_def : '<' generic_def_list '>' | ;
generic_def_list : generic_def_one | generic_def_list ',' generic_def_one | ;
generic_def_one
 : IDENT '=' type ':' bounds
 | IDENT '=' type
 | IDENT ':' bounds
 | IDENT ;

where_clause: RWD_where where_clauses;
where_clauses
	: where_clause_ent ',' where_clauses
	| where_clause_ent;
where_clause_ent
	: type ':' bounds;
bounds: bounds '+' bound | bound;
bound: LIFETIME | path;

/*
=========================================
Paths
=========================================
*/
path
 : path DOUBLECOLON IDENT
 | IDENT;

type_path
 : ufcs_path DOUBLECOLON IDENT
 | DOUBLECOLON type_path_segs
 | type_path_segs
 ;
ufcs_path: '<' type RWD_as type_path '>';
type_path_segs
 : type_path_segs DOUBLECOLON IDENT
 | type_path_segs DOUBLECOLON IDENT '<' type_exprs '>'
 | IDENT '<' type_exprs '>'
 | IDENT
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
	| struct_pattern
	;

tuple_pattern: '(' ')';

bind_pattern: IDENT;

struct_pattern
	: path '{' '}'
	| path tuple_pattern
	;

//refutable_pattern: ;


/*
=========================================
Expressions!
=========================================
*/
code:
	expr;
expr:
	'!'
