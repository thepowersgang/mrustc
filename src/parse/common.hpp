#ifndef PARSE_COMMON_HPP_INCLUDED
#define PARSE_COMMON_HPP_INCLUDED
#include <iostream>

#define GET_TOK(tok, lex) ((tok = lex.getToken()).type())
#define GET_CHECK_TOK(tok, lex, exp) do {\
    if((tok = lex.getToken()).type() != exp) \
            throw ParseError::Unexpected(lex, tok, Token(exp));\
} while(0)
#define CHECK_TOK(tok, exp) do {\
    if(tok.type() != exp) \
            throw ParseError::Unexpected(lex, tok, Token(exp));\
} while(0)

enum eParsePathGenericMode
{
    PATH_GENERIC_NONE,
    PATH_GENERIC_EXPR,
    PATH_GENERIC_TYPE
};

extern AST::Path   Parse_Path(TokenStream& lex, bool is_abs, eParsePathGenericMode generic_mode);
extern TypeRef     Parse_Type(TokenStream& lex);
extern AST::Expr   Parse_Expr(TokenStream& lex, bool const_only);
extern AST::Expr   Parse_ExprBlock(TokenStream& lex);

class TraceLog
{
    static unsigned int depth;
    const char* m_tag;
public:
    TraceLog(const char* tag): m_tag(tag) { indent(); ::std::cout << ">> " << m_tag << ::std::endl; }
    ~TraceLog() { outdent(); ::std::cout << "<< " << m_tag << ::std::endl; }
private:
    void indent()
    {
        for(unsigned int i = 0; i < depth; i ++)
            ::std::cout << " ";
        depth ++;
    }
    void outdent()
    {
        depth --;
        for(unsigned int i = 0; i < depth; i ++)
            ::std::cout << " ";
    }
};
#define TRACE_FUNCTION  TraceLog _tf_(__func__)

#endif // PARSE_COMMON_HPP_INCLUDED
