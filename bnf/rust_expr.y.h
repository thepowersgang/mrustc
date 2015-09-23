
assign_op: '=' | PLUSEQUAL | MINUSEQUAL | STAREQUAL | SLASHEQUAL;

#define SUFFIX_is_
#define _(v)	v
#include "rust_expr.y_tree.h"
#undef SUFFIX_is_
#undef _

#define SUFFIX_is__NOSTRLIT
#define _(v)	v##_NOSTRLIT
#include "rust_expr.y_tree.h"
#undef SUFFIX
#undef _
