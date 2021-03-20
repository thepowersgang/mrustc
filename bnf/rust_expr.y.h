
assign_op: '=' | PLUSEQUAL | MINUSEQUAL | STAREQUAL | SLASHEQUAL | PERCENTEQUAL | DOUBLELTEQUAL | DOUBLEGTEQUAL | PIPEEQUAL | AMPEQUAL | CARETEQUAL;

closure_arg_list: | closure_arg_list_;
closure_arg_list_
 : closure_arg
 | closure_arg_list ',' closure_arg
closure_arg
 : pattern
 | pattern ':' type
 ;
closure_ret
 :
 | THINARROW type
 ;

#define SUFFIX_is_
#define _(v)	v
#include "rust_expr.y_tree.h"
#undef SUFFIX_is_
#undef _

#define SUFFIX_is__NOSTRLIT
#define _(v)	v##_NOSTRLIT
#include "rust_expr.y_tree.h"
#undef SUFFIX
#undef SUFFIX_is__NOSTRLIT
#undef _

#define SUFFIX_is__NOBRACE
#define _(v)	v##_NOBRACE
#include "rust_expr.y_tree.h"
#undef SUFFIX
#undef _
