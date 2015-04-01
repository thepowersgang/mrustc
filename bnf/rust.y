%token IDENT LIFETIME STRING INTEGER FLOAT 
%token SUPER_ATTR SUB_ATTR
%token RWD_mod RWD_fn RWD_const RWD_static
%token RWD_where
%start crate

%union {
	char *text;
}

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
 : module_def
 | fn_def
/* | struct_def
 | enum_def
 | trait_def*/
 ;

module_def
 : RWD_mod IDENT '{' module_body '}'
 | RWD_mod IDENT ';'
 ;

fn_def: RWD_fn IDENT generic_def '(' fn_def_args ')' fn_def_ret where_clause '{' code '}';

fn_def_ret
 : '-' '>' type
 | ;

fn_def_args : fn_def_args fn_def_arg | ;
fn_def_arg : irrefutable_pattern ':' type;

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
path: 'p';

/*
=========================================
Types
=========================================
*/
type:
	'k';

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
	'!';
