_(expr_noblock): _(expr_assign);

_(expr_assign)
 : _(expr_0)
 | _(expr_0) assign_op _(expr_assign)
 ;

_(expr_0): _(expr_range);

_(expr_range)
 : _(expr_range_n)
 | _(expr_range_n) DOUBLEDOT
 | _(expr_range_n) DOUBLEDOT _(expr_range_n)
 |                 DOUBLEDOT _(expr_range_n)
 | DOUBLEDOT
 | _(expr_range_n) DOUBLEDOTEQ _(expr_range_n)
 ;
_(expr_range_n): _(expr_bor);

_(expr_bor)
 : _(expr_band)
 | _(expr_bor) DOUBLEPIPE _(expr_band) { }
 ;
_(expr_band)
 : _(expr_equ)
 | _(expr_band) DOUBLEAMP _(expr_equ) { }
 ;
_(expr_equ)
 : _(expr_cmp)
 | _(expr_equ) DOUBLEEQUAL _(expr_cmp)
 | _(expr_equ) EXCLAMEQUAL _(expr_cmp)
 ;
_(expr_cmp)
 : _(expr_cmp_n)
 | _(expr_cmp) '<' _(expr_cmp_n)	{}
 | _(expr_cmp) '>' _(expr_cmp_n)	{}
 | _(expr_cmp) GTEQUAL _(expr_cmp_n)	{}
 | _(expr_cmp) LTEQUAL _(expr_cmp_n)	{}
 ;
_(expr_cmp_n): _(expr_or);

_(expr_or)
 : _(expr_and)
 | _(expr_or) '|' _(expr_and) { }
 ;
_(expr_and)
 : _(expr_xor)
 | _(expr_and) '&' _(expr_xor) { }
 ;
_(expr_xor)
 : _(expr_8)
 | _(expr_xor) '^' _(expr_8) { }
 ;
_(expr_8)
 : _(expr_9)
 | _(expr_8) DOUBLELT _(expr_9) {}
 | _(expr_8) DOUBLEGT _(expr_9) {}
 ;
_(expr_9)
 : _(expr_10)
 | _(expr_9) '+' _(expr_10) {}
 | _(expr_9) '-' _(expr_10) {}
 ;
/* 10: Times/Div/Modulo */
_(expr_10)
 : _(expr_10n)
 | _(expr_10) '*' _(expr_10n) {}
 | _(expr_10) '/' _(expr_10n) {}
 | _(expr_10) '%' _(expr_10n) {}
 ;
_(expr_10n): _(expr_cast);
/* 11: Cast */
_(expr_cast)
 : _(expr_12)
 | _(expr_cast) RWD_as type_ele { bnf_trace(context, "expr:cast"); }
 ;
_(expr_12)
 : _(expr_fc)
 | '-' _(expr_12)
 | '!' _(expr_12)
 | '*' _(expr_12)
/* | RWD_box expr_12 */
 | '&' _(expr_12)
// | '&' block /*HACK*/
// | '&' RWD_mut block
 | '&' RWD_mut _(expr_12)
 | DOUBLEAMP _(expr_12) { }
 | DOUBLEAMP RWD_mut _(expr_12) { }
 | RWD_box _(expr)
 ;

_(expr_fc)
 : _(expr_value)
 | _(expr_fc) '(' expr_list ')'
 | _(expr_fc) '[' expr ']'
 | _(expr_fc) '.' INTEGER
 | _(expr_fc) '.' expr_path_seg '(' expr_list ')'
 | _(expr_fc) '.' expr_path_seg
 | _(expr_fc) '?'
 ;

_(expr_value)
 : CHARLIT | INTEGER | FLOAT | STRING
 | expr_blocks
 | expr_path '(' expr_list opt_comma ')'	{ bnf_trace(context, "function call"); }
#ifndef SUFFIX_is__NOSTRLIT
 | expr_path '{' struct_literal_list '}'	{ bnf_trace(context, "struct literal"); }
 | expr_path '{' struct_literal_list ',' '}'	{ bnf_trace(context, "struct literal"); }
 | expr_path '{' struct_literal_list ',' DOUBLEDOT expr_0 '}'	{ bnf_trace(context, "struct literal (with default)"); }
#endif
 | expr_path
 | RWD_self
 | '(' expr ')'
#ifndef SUFFIX_is__NOBRACE
 | block
#endif
 | '(' ')'
 | '(' expr_list ',' ')'
 | '(' expr ',' expr_list opt_comma ')'
 | '[' ']'
 | '[' expr_list ']'
 | '[' expr_list ',' ']'
 | '[' expr ';' expr ']'
 | macro_braced   { bnf_trace(context, "Expr macro invocation"); }
 | macro_nonbrace { bnf_trace(context, "Expr macro invocation"); }
 | _(closure)
 | RWD_move _(closure)
 ;
_(closure)
 : '|' closure_arg_list '|' closure_ret expr
 | DOUBLEPIPE closure_ret expr
 ;
