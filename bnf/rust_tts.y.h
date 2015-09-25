#define _C(v)	v { $$ = v; }
#define _T(v)	v { $$ = v; }
tt_tok
 : _T(IDENT) | _T(STRING) | _T(CHARLIT) | _T(LIFETIME) | _T(INTEGER) | _T(MACRO)
 | _C('+') | _C('*') | _C('/') | _C(',') | _C(';')
 | _T(RWD_self) | _T(RWD_super) | _T(RWD_mut) | _T(RWD_ref) | _T(RWD_let) | _T(RWD_where)
 | _T(RWD_for ) | _T(RWD_while) | _T(RWD_loop) | _T(RWD_if) | _T(RWD_else) | _T(RWD_match) | _T(RWD_return)
 | _T(RWD_impl) | _T(RWD_pub  ) | _T(RWD_struct) | _T(RWD_enum) | _T(RWD_fn) | _T(RWD_type) | _T(RWD_static) | _T(RWD_const)
 | _C('!') | _T(EXCLAMEQUAL)
 | _C('-') | _T(THINARROW)
 | _C('&') | _T(DOUBLEAMP)
 | _C(':') | _T(DOUBLECOLON)
 | _C('|') | _T(DOUBLEPIPE)
 | _C('=') | _T(DOUBLEEQUAL) | _T(FATARROW)
 | _C('<') | _T(DOUBLELT)    | _T(LTEQUAL)
 | _C('>') | _T(DOUBLEGT)    | _T(GTEQUAL)
 | _C('.') | _T(DOUBLEDOT)   | _T(TRIPLEDOT)
 | _C('$') | _C('#') | _C('@')
 ;
#undef _
