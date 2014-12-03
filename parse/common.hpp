#ifndef PARSE_COMMON_HPP_INCLUDED
#define PARSEERROR_HPP_INCLUDED

#define GET_TOK(tok, lex) ((tok = lex.getToken()).type())
#define GET_CHECK_TOK(tok, lex, exp) do {\
    if((tok = lex.getToken()).type() != exp) \
            throw ParseError::Unexpected(tok, Token(exp));\
} while(0)
#define CHECK_TOK(tok, exp) do {\
    if(tok.type() != exp) \
            throw ParseError::Unexpected(tok, Token(exp));\
} while(0)

extern AST::Path   Parse_Path(TokenStream& lex, bool is_abs, bool generic_ok);
extern TypeRef     Parse_Type(TokenStream& lex);
extern AST::Expr   Parse_Expr(TokenStream& lex, bool const_only);
extern AST::Expr   Parse_ExprBlock(TokenStream& lex);

#endif // PARSE_COMMON_HPP_INCLUDED
