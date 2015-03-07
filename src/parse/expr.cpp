/*
 */
#include "preproc.hpp"
#include "parseerror.hpp"
#include "../ast/ast.hpp"
#include "common.hpp"
#include "../macros.hpp"
#include <iostream>
#include "tokentree.hpp"

typedef ::std::unique_ptr<AST::ExprNode>    ExprNodeP;
#define NEWNODE(type, ...)  ExprNodeP(new type(__VA_ARGS__))
using AST::ExprNode;

ExprNodeP Parse_ExprBlockNode(TokenStream& lex);
ExprNodeP Parse_Stmt(TokenStream& lex, bool& opt_semicolon);
ExprNodeP Parse_Expr0(TokenStream& lex);
ExprNodeP Parse_ExprBlocks(TokenStream& lex);
ExprNodeP Parse_Expr1(TokenStream& lex);

AST::Expr Parse_Expr(TokenStream& lex, bool const_only)
{
    return AST::Expr(Parse_Expr0(lex));
}

AST::Expr Parse_ExprBlock(TokenStream& lex)
{
    return AST::Expr(Parse_ExprBlockNode(lex));
}

::std::vector<AST::Pattern> Parse_PatternList(TokenStream& lex);

/// Parse a pattern
///
/// Examples:
/// - `Enum::Variant(a)`
/// - `(1, a)`
/// - `1 ... 2`
/// - `"string"`
/// - `mut x`
/// - `mut x @ 1 ... 2`
AST::Pattern Parse_Pattern(TokenStream& lex)
{
    TRACE_FUNCTION;

    AST::Path   path;
    Token   tok;
    tok = lex.getToken();
    
    bool expect_bind = false;
    bool is_mut = false;
    bool is_ref = false;
    // 1. Mutablity + Reference
    if( tok.type() == TOK_RWORD_REF )
    {
        throw ParseError::Todo("ref bindings");
        is_ref = true;
        expect_bind = true;
        tok = lex.getToken();
    }
    if( tok.type() == TOK_RWORD_MUT )
    {
        throw ParseError::Todo("mut bindings");
        is_mut = true;
        expect_bind = true;
        tok = lex.getToken();
    }
    
    ::std::string   bind_name;
    if( expect_bind )
    {
        CHECK_TOK(tok, TOK_IDENT);
        bind_name = tok.str();
        if( GET_TOK(tok, lex) != TOK_AT )
        {
            lex.putback(tok);
            return AST::Pattern(AST::Pattern::TagBind(), bind_name);
        }
    }
    
    // TODO: If the next token is an ident, parse as a path
    if( !expect_bind && tok.type() == TOK_IDENT )
    {
        lex.putback(tok);
        path = Parse_Path(lex, false, PATH_GENERIC_EXPR);
        // - If the path is trivial
        if( path.size() == 1 && path[0].args().size() == 0 )
        {
            switch( GET_TOK(tok, lex) )
            {
            //  - If the next token after that is '@', use as bind name and expect an actual pattern
            case TOK_AT:
                bind_name = path[0].name();
                GET_TOK(tok, lex);
                break;
            //  - Else, if the next token is  a '(' or '{', treat as a struct/enum
            case TOK_BRACE_OPEN:
                throw ParseError::Todo("Parse_Pattern - Structure patterns");
            case TOK_PAREN_OPEN:
                return AST::Pattern(AST::Pattern::TagEnumVariant(), ::std::move(path), Parse_PatternList(lex));
            //  - Else, treat as a MaybeBind
            default:
                lex.putback(tok);
                return AST::Pattern(AST::Pattern::TagMaybeBind(), path[0].name());
            }
        }
        else
        {
            switch(GET_TOK(tok, lex))
            {
            case TOK_BRACE_OPEN:
                throw ParseError::Todo("Parse_Pattern - Structure patterns");
            case TOK_PAREN_OPEN:
                return AST::Pattern(AST::Pattern::TagEnumVariant(), ::std::move(path), Parse_PatternList(lex));
            default:
                lex.putback(tok);
                return AST::Pattern(AST::Pattern::TagMaybeBind(), path[0].name());
            }
        }
    }
    
    
    switch( tok.type() )
    {
    case TOK_IDENT:
        lex.putback(tok);
        path = Parse_Path(lex, false, PATH_GENERIC_EXPR);
        if( 0 )
    case TOK_DOUBLE_COLON:
        // 2. Paths are enum/struct names
        {
            path = Parse_Path(lex, true, PATH_GENERIC_EXPR);
        }
        switch( GET_TOK(tok, lex) )
        {
        case TOK_PAREN_OPEN: {
            // A list of internal patterns
            ::std::vector<AST::Pattern> child_pats;
            do {
                child_pats.push_back( Parse_Pattern(lex) );
            } while( GET_TOK(tok, lex) == TOK_COMMA );
            CHECK_TOK(tok, TOK_PAREN_CLOSE);
            return AST::Pattern(AST::Pattern::TagEnumVariant(), ::std::move(path), ::std::move(child_pats));
            }
        default:
            lex.putback(tok);
            return AST::Pattern(AST::Pattern::TagValue(), NEWNODE(AST::ExprNode_NamedValue, ::std::move(path)));
        }
        break;
    case TOK_INTEGER:
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE(AST::ExprNode_Integer, tok.intval(), tok.datatype()) );
    case TOK_STRING:
        throw ParseError::Todo("string patterns");
    case TOK_PAREN_OPEN:
        // This may also have to handle range expressions? (and other complexities)
        throw ParseError::Todo("tuple patterns");
    default:
        throw ParseError::Unexpected(tok);
    }
    throw ParseError::BugCheck("Parse_Pattern should early return");
}

::std::vector<AST::Pattern> Parse_PatternList(TokenStream& lex)
{
    Token tok;
    ::std::vector<AST::Pattern> child_pats;
    do {
        child_pats.push_back( Parse_Pattern(lex) );
    } while( GET_TOK(tok, lex) == TOK_COMMA );
    CHECK_TOK(tok, TOK_PAREN_CLOSE);
    return child_pats;
}

ExprNodeP Parse_ExprBlockNode(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;

    ::std::vector<ExprNodeP> nodes;
    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
    
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        lex.putback(tok);
        bool    opt_semicolon = false;
        // NOTE: This semicolon handling is SHIT.
        nodes.push_back(Parse_Stmt(lex, opt_semicolon));
        if( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
        {
            if( !opt_semicolon )
            {
                CHECK_TOK(tok, TOK_SEMICOLON);
            }
            else
                lex.putback(tok);
        }
        else
        {
            goto pass_value;
        }
    }
    nodes.push_back(nullptr);
pass_value:
    return NEWNODE( AST::ExprNode_Block, ::std::move(nodes) );
}

ExprNodeP Parse_Stmt(TokenStream& lex, bool& opt_semicolon)
{
    TRACE_FUNCTION;

    Token   tok;
    // 1. Handle 'let'
    // 2. Handle new blocks
    // 3. Handle a sequence of expressions broken by ';'
    switch(GET_TOK(tok, lex))
    {
    case TOK_BRACE_OPEN:
        lex.putback(tok);
        opt_semicolon = true;
        return Parse_ExprBlockNode(lex);
    case TOK_RWORD_LET: {
        //ret.append();
        AST::Pattern pat = Parse_Pattern(lex);
        TypeRef type;
        if( GET_TOK(tok, lex) == TOK_COLON ) {
            type = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);
        }
        else {
            CHECK_TOK(tok, TOK_EQUAL);
        }
        ExprNodeP val = Parse_Expr1(lex);
        opt_semicolon = false;
        return NEWNODE( AST::ExprNode_LetBinding, ::std::move(pat), ::std::move(type), ::std::move(val) );
        }
    case TOK_RWORD_RETURN:
        return NEWNODE( AST::ExprNode_Return, Parse_Expr1(lex) );
    case TOK_RWORD_LOOP:
        throw ParseError::Todo("loop");
        break;
    case TOK_RWORD_FOR:
        throw ParseError::Todo("for");
        break;
    case TOK_RWORD_WHILE:
        throw ParseError::Todo("while");
        break;
    default: {
        lex.putback(tok);
        opt_semicolon = true;
        return Parse_Expr0(lex);
        }
    }

}

::std::vector<ExprNodeP> Parse_ParenList(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;

    ::std::vector<ExprNodeP> rv;
    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
    if( GET_TOK(tok, lex) != TOK_PAREN_CLOSE )
    {
        lex.putback(tok);
        do {
            rv.push_back( Parse_Expr1(lex) );
        } while( GET_TOK(tok, lex) == TOK_COMMA );
        CHECK_TOK(tok, TOK_PAREN_CLOSE);
    }
    return rv;
}

// 0: Assign
ExprNodeP Parse_Expr0(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token tok;

    ExprNodeP rv = Parse_ExprBlocks(lex);
    if( GET_TOK(tok, lex) == TOK_EQUAL )
    {
        ExprNodeP val = Parse_Expr1(lex);
        rv = NEWNODE( AST::ExprNode_Assign, ::std::move(rv), ::std::move(val) );
    }
    else
    {
        lex.putback(tok);
    }
    return rv;
}

/// Parse an 'if' statement
// Note: TOK_RWORD_IF has already been eaten
ExprNodeP Parse_IfStmt(TokenStream& lex)
{
    TRACE_FUNCTION;

    Token   tok;
    ExprNodeP cond;
    if( GET_TOK(tok, lex) == TOK_RWORD_LET ) {
        throw ParseError::Todo("if let");
    }
    else {
        lex.putback(tok);
        cond = Parse_Expr0(lex);
    }

    // Contents
    ExprNodeP code = Parse_ExprBlockNode(lex);

    // Handle else:
    ExprNodeP altcode;
    if( GET_TOK(tok, lex) == TOK_RWORD_ELSE )
    {
        // Recurse for 'else if'
        if( GET_TOK(tok, lex) == TOK_RWORD_IF ) {
            altcode = Parse_IfStmt(lex);
        }
        // - or get block
        else {
            lex.putback(tok);
            altcode = Parse_ExprBlockNode(lex);
        }
    }
    // - or nothing
    else {
        lex.putback(tok);
    }

    return NEWNODE( AST::ExprNode_If, ::std::move(cond), ::std::move(code), ::std::move(altcode) );
}

ExprNodeP Parse_Expr_Match(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token tok;
    
    // 1. Get expression
    ExprNodeP   switch_val = Parse_Expr1(lex);
    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
    
    ::std::vector< ::std::pair<AST::Pattern, ExprNodeP> >    arms;
    do {
        if( GET_TOK(tok, lex) == TOK_BRACE_CLOSE )
            break;
        lex.putback(tok);
        AST::Pattern    pat = Parse_Pattern(lex);
        
        GET_CHECK_TOK(tok, lex, TOK_FATARROW);
        
        bool opt_semicolon = false;
        ExprNodeP   val = Parse_Stmt(lex, opt_semicolon);
        
        arms.push_back( ::std::make_pair( ::std::move(pat), ::std::move(val) ) );
    } while( GET_TOK(tok, lex) == TOK_COMMA );
    CHECK_TOK(tok, TOK_BRACE_CLOSE);
    
    return NEWNODE( AST::ExprNode_Match, ::std::move(switch_val), ::std::move(arms) );
}

// 0.5: Blocks
ExprNodeP Parse_ExprBlocks(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token tok;
    switch( GET_TOK(tok, lex) )
    {
    case TOK_RWORD_MATCH:
        return Parse_Expr_Match(lex);
    case TOK_RWORD_IF:
        // TODO: if let
        return Parse_IfStmt(lex);
    default:
        lex.putback(tok);
        return Parse_Expr1(lex);
    }
}


#define LEFTASSOC(cur, _next, cases) \
ExprNodeP _next(TokenStream& lex); \
ExprNodeP cur(TokenStream& lex) \
{ \
    ExprNodeP (*next)(TokenStream&) = _next;\
    ExprNodeP rv = next(lex); \
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
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::CMPEQU, ::std::move(rv), next(lex));
        break;
    case TOK_EXCLAM_EQUAL:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::CMPNEQU, ::std::move(rv), next(lex));
        break;
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
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::BITOR, ::std::move(rv), next(lex));
        break;
)
// 6: Bit XOR
LEFTASSOC(Parse_Expr6, Parse_Expr7,
    case TOK_CARET:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::BITXOR, ::std::move(rv), next(lex));
        break;
)
// 7: Bit AND
LEFTASSOC(Parse_Expr7, Parse_Expr8,
    case TOK_AMP:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::BITAND, ::std::move(rv), next(lex));
        break;
)
// 8: Bit Shifts
LEFTASSOC(Parse_Expr8, Parse_Expr9,
    case TOK_DOUBLE_LT:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::SHL, ::std::move(rv), next(lex));
        break;
    case TOK_DOUBLE_GT:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::SHR, ::std::move(rv), next(lex));
        break;
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
        rv = NEWNODE( AST::ExprNode_Cast, ::std::move(rv), Parse_Type(lex) );
        break;
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
ExprNodeP Parse_ExprFC(TokenStream& lex);
ExprNodeP Parse_Expr12(TokenStream& lex)
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

ExprNodeP Parse_ExprVal(TokenStream& lex);
ExprNodeP Parse_ExprFC(TokenStream& lex)
{
    ExprNodeP   val = Parse_ExprVal(lex);
    while(true)
    {
        Token   tok;
        switch(GET_TOK(tok, lex))
        {
        case TOK_PAREN_OPEN:
            // Expression method call
            lex.putback(tok);
            val = NEWNODE( AST::ExprNode_CallObject, ::std::move(val), Parse_ParenList(lex) );
            break;
        case TOK_DOT: {
            // Field access / method call
            // TODO: What about tuple indexing?
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            switch( GET_TOK(tok, lex) )
            {
            case TOK_PAREN_OPEN:
                lex.putback(tok);
                val = NEWNODE( AST::ExprNode_CallMethod, ::std::move(val), AST::PathNode(name, {}), Parse_ParenList(lex) );
                break;
            case TOK_DOUBLE_COLON:
                throw ParseError::Todo("method calls - generic");
            default:
                val = NEWNODE( AST::ExprNode_Field, ::std::move(val), ::std::string(name) );
                lex.putback(tok);
                break;
            }
            break; }
        default:
            lex.putback(tok);
            return val;
        }
    }
}

ExprNodeP Parse_ExprVal(TokenStream& lex)
{
    Token   tok;
    AST::Path   path;
    switch( GET_TOK(tok, lex) )
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
        case TOK_BRACE_OPEN: {
            // Braced structure literal
            // - A series of 0 or more pairs of <ident>: <expr>,
            // - '..' <expr>
            ::std::vector< ::std::pair< ::std::string, ::std::unique_ptr<AST::ExprNode>> >  items;
            while( GET_TOK(tok, lex) == TOK_IDENT )
            {
                ::std::string   name = tok.str();
                GET_CHECK_TOK(tok, lex, TOK_COLON);
                ExprNodeP   val = Parse_Expr0(lex);
                items.push_back( ::std::make_pair(::std::move(name), ::std::move(val)) );
                if( GET_TOK(tok,lex) == TOK_BRACE_CLOSE )
                    break;
                CHECK_TOK(tok, TOK_COMMA);
            }
            ExprNodeP    base_val;
            if( tok.type() == TOK_DOUBLE_DOT )
            {
                // default
                base_val = Parse_Expr0(lex);
                GET_TOK(tok, lex);
            }
            CHECK_TOK(tok, TOK_BRACE_CLOSE);
            return NEWNODE( AST::ExprNode_StructLiteral, path, ::std::move(base_val), ::std::move(items) );
            }
        case TOK_PAREN_OPEN: {
            lex.putback(tok);
            // Function call
            ::std::vector<ExprNodeP> args = Parse_ParenList(lex);
            return NEWNODE( AST::ExprNode_CallPath, ::std::move(path), ::std::move(args) );
            }
        default:
            // Value
            lex.putback(tok);
            return NEWNODE( AST::ExprNode_NamedValue, ::std::move(path) );
        }
    case TOK_INTEGER:
        return NEWNODE( AST::ExprNode_Integer, tok.intval(), tok.datatype() );
    case TOK_FLOAT:
        throw ParseError::Todo("Float");
    case TOK_RWORD_SELF:
        return NEWNODE( AST::ExprNode_NamedValue, AST::Path(AST::Path::TagLocal(), "self") );
    case TOK_PAREN_OPEN: {
        ExprNodeP rv = Parse_Expr0(lex);
        if( GET_TOK(tok, lex) == TOK_COMMA ) {
            ::std::vector<ExprNodeP> ents;
            ents.push_back( ::std::move(rv) );
            do {
                if( GET_TOK(tok, lex) == TOK_PAREN_CLOSE )
                    break;
                lex.putback(tok);
                ents.push_back( Parse_Expr0(lex) );
            } while( GET_TOK(tok, lex) == TOK_COMMA );
            rv = NEWNODE( AST::ExprNode_Tuple, ::std::move(ents) );
        }
        CHECK_TOK(tok, TOK_PAREN_CLOSE);
        return rv; }
    case TOK_MACRO:
        //return NEWNODE( AST::ExprNode_Macro, tok.str(), Parse_TT(lex) );
        {
        MacroExpander expanded_macro = Macro_Invoke(tok.str().c_str(), Parse_TT(lex));
        return Parse_Expr0(expanded_macro);
        }
    default:
        throw ParseError::Unexpected(tok);
    }
}

// Token Tree Parsing
TokenTree Parse_TT(TokenStream& lex)
{
    Token tok = lex.getToken();
    eTokenType  closer = TOK_PAREN_CLOSE;
    switch(tok.type())
    {
    case TOK_PAREN_OPEN:
        closer = TOK_PAREN_CLOSE;
        break;
    case TOK_SQUARE_OPEN:
        closer = TOK_SQUARE_CLOSE;
        break;
    case TOK_BRACE_OPEN:
        closer = TOK_BRACE_CLOSE;
        break;
    default:
        return TokenTree(tok);
    }

    ::std::vector<TokenTree>   items;
    items.push_back(tok);
    while(GET_TOK(tok, lex) != closer && tok.type() != TOK_EOF)
    {
        lex.putback(tok);
        items.push_back(Parse_TT(lex));
    }
    items.push_back(tok);
    return TokenTree(items);
}

TokenTree Parse_TT_Path(TokenStream& lex)
{
    throw ParseError::Todo("TokenTree path");
}
/// Parse a token tree path
TokenTree Parse_TT_Val(TokenStream& lex)
{
    Token   tok;
    ::std::vector<TokenTree>    ret;
    switch(GET_TOK(tok, lex))
    {
    case TOK_PAREN_OPEN:
        lex.putback(tok);
        return Parse_TT(lex);

    case TOK_IDENT:
    case TOK_DOUBLE_COLON: {
        lex.putback(tok);
        TokenTree inner = Parse_TT_Path(lex);
        if(GET_TOK(tok, lex) == TOK_BRACE_OPEN) {
            lex.putback(tok);
            ret.push_back(inner);
            ret.push_back(Parse_TT(lex));
        }
        else {
            lex.putback(tok);
            return inner;
        }
        break; }
    case TOK_RWORD_SELF:
        return TokenTree(tok);
    case TOK_RWORD_MATCH:
        ret.push_back(TokenTree(tok));
        ret.push_back(Parse_TT(lex));
        break;
    case TOK_RWORD_IF:
        ret.push_back(TokenTree(tok));
        ret.push_back(Parse_TT(lex));
        if( GET_TOK(tok, lex) == TOK_RWORD_ELSE ) {
            ret.push_back(TokenTree(tok));
            ret.push_back(Parse_TT(lex));
        }
        else {
            lex.putback(tok);
        }
        break;
    default:
        // Oh, fail :(
        throw ParseError::Unexpected(tok);
    }
    return TokenTree(ret);
}
/// Parse a token tree expression
TokenTree Parse_TT_Expr(TokenStream& lex)
{
    Token   tok;
    ::std::vector<TokenTree>    ret;

    ret.push_back(Parse_TT_Val(lex));
    // 1. Get left associative blocks until nothing matches
    bool cont = true;
    while(cont)
    {
        switch(GET_TOK(tok, lex))
        {
        case TOK_PLUS:
        case TOK_DASH:
            ret.push_back(tok);
            ret.push_back(Parse_TT_Val(lex));
            break;
        case TOK_DOT:
            ret.push_back(tok);
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ret.push_back(tok);
            switch(GET_TOK(tok, lex))
            {
            case TOK_DOUBLE_COLON:
                throw ParseError::Todo("Generic type params in TT expr");
            case TOK_PAREN_OPEN:
                lex.putback(tok);
                ret.push_back(Parse_TT(lex));
                break;
            default:
                lex.putback(tok);
                break;
            }
            break;
        default:
            lex.putback(tok);
            cont = false;
            break;
        }
    }
    return TokenTree(ret);

}
TokenTree Parse_TT_Stmt(TokenStream& lex)
{
    throw ParseError::Todo("Parse_TT_Stmt");
}
TokenTree Parse_TT_Block(TokenStream& lex)
{
    throw ParseError::Todo("Parse_TT_Block");
}
