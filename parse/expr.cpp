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

AST::Pattern Parse_Pattern(TokenStream& lex)
{
    AST::Path   path;
    Token   tok;
    tok = lex.getToken();
    if( tok.type() == TOK_RWORD_REF )
    {
        throw ParseError::Todo("ref bindings");
        tok = lex.getToken();
    }
    switch( tok.type() )
    {
    case TOK_IDENT:
        // 1. Identifiers could be either a bind or a value
        // - If the path resolves to a single node, either a local enum value, or a binding
        lex.putback(tok);
        path = Parse_Path(lex, false, PATH_GENERIC_EXPR);
        if( path.length() == 1 && path[0].args().size() == 0 )
        {
            // Could be a name binding, check the next token
            GET_TOK(tok, lex);
            if(tok.type() != TOK_PAREN_OPEN) {
                lex.putback(tok);
                return AST::Pattern(AST::Pattern::TagMaybeBind(), path[0].name());
            }
            lex.putback(tok);
        }
        // otherwise, it's a value check
        if(0)
    case TOK_DOUBLE_COLON:
        // 2. Paths are enum/struct names
        {
            lex.putback(tok);
            path = Parse_Path(lex, true, PATH_GENERIC_EXPR);
        }
        switch( GET_TOK(tok, lex) )
        {
        case TOK_PAREN_OPEN: {
            // A list of internal patterns
            ::std::vector<AST::Pattern> child_pats;
            do {
                AST::Pattern pat = Parse_Pattern(lex);
                child_pats.push_back(pat);
            } while( GET_TOK(tok, lex) == TOK_COMMA );
            CHECK_TOK(tok, TOK_PAREN_CLOSE);
            return AST::Pattern(AST::Pattern::TagEnumVariant(), path, child_pats);
            }
        default:
            lex.putback(tok);
            return AST::Pattern(AST::Pattern::TagValue(), ExprNode(ExprNode::TagNamedValue(), path));
        }
        break;
    case TOK_INTEGER:
        return AST::Pattern( AST::Pattern::TagValue(), ExprNode(ExprNode::TagInteger(), tok.intval(), tok.datatype()) );
    case TOK_PAREN_OPEN:
        throw ParseError::Todo("tuple patterns");
    default:
        throw ParseError::Unexpected(tok);
    }
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

::std::vector<AST::ExprNode> Parse_ParenList(TokenStream& lex)
{
    ::std::vector<ExprNode> rv;
    Token   tok;
    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
    if( (tok = lex.getToken()).type() != TOK_PAREN_CLOSE )
    {
        lex.putback(tok);
        do {
            rv.push_back( Parse_Expr1(lex) );
        } while( (tok = lex.getToken()).type() == TOK_COMMA );
        CHECK_TOK(tok, TOK_PAREN_CLOSE);
    }
    return rv;
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
        ::std::vector< ::std::pair<AST::Pattern, ExprNode> >    arms;
        do {
            if( GET_TOK(tok, lex) == TOK_BRACE_CLOSE )
                break;
            lex.putback(tok);
            AST::Pattern    pat = Parse_Pattern(lex);
            GET_CHECK_TOK(tok, lex, TOK_FATARROW);
            AST::ExprNode   val = Parse_Expr0(lex);
            arms.push_back( ::std::make_pair(pat, val) );
        } while( GET_TOK(tok, lex) == TOK_COMMA );
        CHECK_TOK(tok, TOK_BRACE_CLOSE);
        return AST::ExprNode(ExprNode::TagMatch(), switch_val, arms);
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
    case TOK_EXCLAM_EQUAL:
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
    case TOK_EXCLAM:
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
        Token   tok;
        switch((tok = lex.getToken()).type())
        {
        case TOK_PAREN_OPEN:
            // Function call
            throw ParseError::Todo("Function call / structure literal");
            break;
        case TOK_DOT:
            // Field access
            throw ParseError::Todo("Field access");
            break;
        default:
            lex.putback(tok);
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
        path = Parse_Path(lex, false, PATH_GENERIC_EXPR);
        if(0)
    case TOK_DOUBLE_COLON:
        path = Parse_Path(lex, true, PATH_GENERIC_EXPR);
        switch( GET_TOK(tok, lex) )
        {
        case TOK_BRACE_OPEN:
            // Structure literal
            throw ParseError::Todo("Structure literal");
            break;
        case TOK_PAREN_OPEN: {
            lex.putback(tok);
            // Function call
            ::std::vector<AST::ExprNode> args = Parse_ParenList(lex);
            return ExprNode(ExprNode::TagCallPath(), path, args);
            }
        default:
            // Value
            lex.putback(tok);
            return ExprNode(ExprNode::TagNamedValue(), path);
        }
    case TOK_INTEGER:
        return ExprNode(ExprNode::TagInteger(), tok.intval(), tok.datatype());
    case TOK_FLOAT:
        throw ParseError::Todo("Float");
    default:
        throw ParseError::Unexpected(tok);
    }
}
