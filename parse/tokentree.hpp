#ifndef TOKENTREE_HPP_INCLUDED
#define TOKENTREE_HPP_INCLUDED

#include "lex.hpp"


extern TokenTree Parse_TT(TokenStream& lex);
extern TokenTree Parse_TT_Expr(TokenStream& lex);

#endif // TOKENTREE_HPP_INCLUDED
