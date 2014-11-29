/*
 */
#include "preproc.hpp"
#include "parseerror.hpp"
#include "../ast/ast.hpp"
#include "common.hpp"
#include <iostream>

using AST::ExprNode;

AST::ExprNode Parse_ExprBlockNode(TokenStream& lex);
AST::ExprNode Parse_Expr0(TokenStream& lex);
AST::ExprNode Parse_ExprBlocks(TokenStream& lex);
AST::ExprNode Parse_Expr1(TokenStream& lex);

AST::Expr Parse_Expr(TokenStream& lex, bool const_only)
{
    return AST::Expr(Parse_Expr0(lex));
}

AST::Expr Parse_ExprBlock(TokenStream& lex)
{
    return AST::Expr(Parse_ExprBlockNode(lex));
}

ExprNode Parse_ExprBlockNode(TokenStream& lex)
{
    ::std::vector<ExprNode> nodes;
    Token   tok;
    bool    trailing_value = false;
    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
    while( (tok = lex.getToken()).type() != TOK_BRACE_CLOSE )
    {
        // 1. Handle 'let'
        // 2. Handle new blocks
        // 3. Handle a sequence of expressions broken by ';'
        switch(tok.type())
        {
        case TOK_BRACE_OPEN:
            lex.putback(tok);
            nodes.push_back(Parse_ExprBlockNode(lex));
            trailing_value = true;
            break;
        case TOK_RWORD_LET:
            //ret.append();
            throw ParseError::Todo("block let");
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            trailing_value = false;
            break;
        case TOK_RWORD_LOOP:
            throw ParseError::Todo("loop");
            break;
        case TOK_RWORD_FOR:
            throw ParseError::Todo("for");
            break;
        case TOK_RWORD_WHILE:
            throw ParseError::Todo("while");
            break;
        default:
            lex.putback(tok);
            nodes.push_back(Parse_Expr0(lex));
            tok = lex.getToken();
            if(tok.type() != TOK_SEMICOLON)
            {
                if(tok.type() != TOK_BRACE_CLOSE)
                    throw ParseError::Unexpected(tok, Token(TOK_SEMICOLON));
                else
                    lex.putback(tok);
                trailing_value = true;
            }
            else{
                trailing_value = false;
            }
            break;
        }
    }
    if( trailing_value == false )
    {
        nodes.push_back(ExprNode());
    }
    return AST::ExprNode(ExprNode::TagBlock(), nodes);
}

// 0: Assign
AST::ExprNode Parse_Expr0(TokenStream& lex)
{
    AST::ExprNode rv = Parse_ExprBlocks(lex);
    Token tok = lex.getToken();
    if( tok.type() == TOK_EQUAL )
    {
        ExprNode val = Parse_Expr1(lex);
        rv = ExprNode(ExprNode::TagAssign(), rv, val);
    }
    else
    {
        lex.putback(tok);
    }
    return rv;
}

// 0.5: Blocks
AST::ExprNode Parse_ExprBlocks(TokenStream& lex)
{
    Token tok = lex.getToken();
    switch( tok.type() )
    {
    case TOK_RWORD_MATCH: {
        // 1. Get expression
        AST::ExprNode   switch_val = Parse_Expr1(lex);
        GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
        throw ParseError::Todo("match arms");
        GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
        //return AST::ExprNode(ExprNode::TagMatch, switch_val, );
        }
    case TOK_RWORD_IF:
        throw ParseError::Todo("if");
        break;
    default:
        lex.putback(tok);
        return Parse_Expr1(lex);
    }
}


#define LEFTASSOC(cur, next, cases) \
AST::ExprNode next(TokenStream& lex); \
AST::ExprNode cur(TokenStream& lex) \
{ \
    ::std::cout << ">>" << #cur << ::std::endl; \
    AST::ExprNode rv = next(lex); \
    while(true) \
    { \
        Token   tok; \
        switch((tok = lex.getToken()).type()) \
        { \
        cases \
        default: \
            ::std::cout << "<<" << #cur << ::std::endl; \
            lex.putback(tok); \
            return rv; \
        } \
    } \
}
// 1: Bool OR
LEFTASSOC(Parse_Expr1, Parse_Expr2,
    case TOK_DOUBLE_PIPE:
        throw ParseError::Todo("expr - boolean OR");
)
// 2: Bool AND
LEFTASSOC(Parse_Expr2, Parse_Expr3,
    case TOK_DOUBLE_AMP:
        throw ParseError::Todo("expr - boolean AND");
)
// 3: (In)Equality
LEFTASSOC(Parse_Expr3, Parse_Expr4,
    case TOK_DOUBLE_EQUAL:
        throw ParseError::Todo("expr - equal");
    case TOK_EXLAM_EQUAL:
        throw ParseError::Todo("expr - not equal");
)
// 4: Comparisons
LEFTASSOC(Parse_Expr4, Parse_Expr5,
    case TOK_LT:
        throw ParseError::Todo("expr - less than");
    case TOK_GT:
        throw ParseError::Todo("expr - greater than");
    case TOK_LTE:
        throw ParseError::Todo("expr - less than or equal");
    case TOK_GTE:
        throw ParseError::Todo("expr - greater than or equal");
)
// 5: Bit OR
LEFTASSOC(Parse_Expr5, Parse_Expr6,
    case TOK_PIPE:
        throw ParseError::Todo("expr - bitwise OR");
)
// 6: Bit XOR
LEFTASSOC(Parse_Expr6, Parse_Expr7,
    case TOK_CARET:
        throw ParseError::Todo("expr - bitwise XOR");
)
// 7: Bit AND
LEFTASSOC(Parse_Expr7, Parse_Expr8,
    case TOK_AMP:
        throw ParseError::Todo("expr - bitwise AND");
)
// 8: Bit Shifts
LEFTASSOC(Parse_Expr8, Parse_Expr9,
    case TOK_DOUBLE_LT:
        throw ParseError::Todo("expr - shift left");
    case TOK_DOUBLE_GT:
        throw ParseError::Todo("expr - shift right");
)
// 9: Add / Subtract
LEFTASSOC(Parse_Expr9, Parse_Expr10,
    case TOK_PLUS:
        throw ParseError::Todo("expr - add");
    case TOK_DASH:
        throw ParseError::Todo("expr - sub");
)
// 10: Cast
LEFTASSOC(Parse_Expr10, Parse_Expr11,
    case TOK_RWORD_AS:
        throw ParseError::Todo("expr - cast");
)
// 11: Times / Divide / Modulo
LEFTASSOC(Parse_Expr11, Parse_Expr12,
    case TOK_STAR:
        throw ParseError::Todo("expr - multiply");
    case TOK_SLASH:
        throw ParseError::Todo("expr - divide");
    case TOK_PERCENT:
        throw ParseError::Todo("expr - modulo");
)
// 12: Unaries
AST::ExprNode Parse_ExprFC(TokenStream& lex);
AST::ExprNode Parse_Expr12(TokenStream& lex)
{
    Token   tok;
    switch((tok = lex.getToken()).type())
    {
    case TOK_DASH:
        throw ParseError::Todo("expr - negate");
    case TOK_EXLAM:
        throw ParseError::Todo("expr - logical negate");
    case TOK_STAR:
        throw ParseError::Todo("expr - dereference");
    case TOK_RWORD_BOX:
        throw ParseError::Todo("expr - box");
    case TOK_AMP:
        throw ParseError::Todo("expr - borrow");
    default:
        lex.putback(tok);
        return Parse_ExprFC(lex);
    }
}

AST::ExprNode Parse_ExprVal(TokenStream& lex);
AST::ExprNode Parse_ExprFC(TokenStream& lex)
{
    AST::ExprNode   val = Parse_ExprVal(lex);
    while(true)
    {
        switch((tok = lex.getToken()).type())
        {
        case TOK_PAREN_OPEN:
            // Function call
            break;
        case TOK_DOT:
            // Field access
            break;
        default:
            return val;
        }
    }
}

AST::ExprNode Parse_ExprVal(TokenStream& lex)
{
    Token   tok;
    AST::Path   path;
    switch((tok = lex.getToken()).type())
    {
    case TOK_IDENT:
        // Get path
        lex.putback(tok);
        path = Parse_Path(lex, false, true);
        if(0)
    case TOK_DOUBLE_COLON:
        path = Parse_Path(lex, true, true);
        switch( (tok = lex.getToken()).type() )
        {
        case TOK_BRACE_OPEN:
            // Structure literal
            throw ParseError::Todo("Structure literal");
            break;
        case TOK_PAREN_OPEN:
            // Function call
            throw ParseError::Todo("Function call / structure literal");
            break;
        default:
            // Value
            lex.putback(tok);
            throw ParseError::Todo("Variable/Constant");
        }
    case TOK_INTEGER:
        return ExprNode(ExprNode::TagInteger(), tok.intval(), tok.datatype());
    case TOK_FLOAT:
        throw ParseError::Todo("Float");
    default:
        throw ParseError::Unexpected(tok);
    }
}
