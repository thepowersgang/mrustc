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
ExprNodeP Parse_IfStmt(TokenStream& lex);
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

AST::Pattern Parse_PatternReal_Path(TokenStream& lex, AST::Path path);
AST::Pattern Parse_PatternReal(TokenStream& lex);


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

    Token   tok;
    tok = lex.getToken();
    
    bool expect_bind = false;
    bool is_mut = false;
    bool is_ref = false;
    // 1. Mutablity + Reference
    if( tok.type() == TOK_RWORD_REF )
    {
        is_ref = true;
        expect_bind = true;
        tok = lex.getToken();
    }
    if( tok.type() == TOK_RWORD_MUT )
    {
        is_mut = true;
        expect_bind = true;
        tok = lex.getToken();
    }
    
    ::std::string   bind_name;
    // If a 'ref' or 'mut' annotation was seen, the next name must be a binding name
    if( expect_bind )
    {
        CHECK_TOK(tok, TOK_IDENT);
        bind_name = tok.str();
        // If there's no '@' after it, it's a name binding only (_ pattern)
        if( GET_TOK(tok, lex) != TOK_AT )
        {
            lex.putback(tok);
            return AST::Pattern(AST::Pattern::TagBind(), bind_name);
        }
        
        tok = lex.getToken();
    }
    // Otherwise, handle MaybeBind
    else if( tok.type() == TOK_IDENT )
    {
        lex.putback(tok);
        AST::Path path = Parse_Path(lex, false, PATH_GENERIC_EXPR);
        // - If the path is trivial
        if( path.size() == 1 && path[0].args().size() == 0 )
        {
            switch( GET_TOK(tok, lex) )
            {
            //  - If the next token after that is '@', use as bind name and expect an actual pattern
            case TOK_AT:
                bind_name = path[0].name();
                GET_TOK(tok, lex);
                // - Fall though
                break;
            //  - Else, if the next token is  a '(' or '{', treat as a struct/enum
            case TOK_BRACE_OPEN:
            case TOK_PAREN_OPEN:
                lex.putback(tok);
                return Parse_PatternReal_Path(lex, path);
            //  - Else, treat as a MaybeBind
            default:
                lex.putback(tok);
                return AST::Pattern(AST::Pattern::TagMaybeBind(), path[0].name());
            }
        }
        else
        {
            // non-trivial path, has to be a pattern (not a bind)
            return Parse_PatternReal_Path(lex, path);
        }
    }
    
    lex.putback(tok);
    AST::Pattern pat = Parse_PatternReal(lex);
    pat.set_bind(bind_name, is_ref, is_mut);
    return ::std::move(pat);
}
AST::Pattern Parse_PatternReal(TokenStream& lex)
{
    TRACE_FUNCTION;
    
    Token   tok;
    AST::Path   path;
    
    switch( GET_TOK(tok, lex) )
    {
    case TOK_UNDERSCORE:
        return AST::Pattern( );
    case TOK_AMP:
        DEBUG("Ref");
        return AST::Pattern( AST::Pattern::TagReference(), Parse_PatternReal(lex) );
    case TOK_IDENT:
        lex.putback(tok);
        return Parse_PatternReal_Path( lex, Parse_Path(lex, false, PATH_GENERIC_EXPR) );
    case TOK_DOUBLE_COLON:
        // 2. Paths are enum/struct names
        return Parse_PatternReal_Path( lex, Parse_Path(lex, true, PATH_GENERIC_EXPR) );
    case TOK_INTEGER:
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE(AST::ExprNode_Integer, tok.intval(), tok.datatype()) );
    case TOK_STRING:
        throw ParseError::Todo("string patterns");
    case TOK_PAREN_OPEN:
        return AST::Pattern(AST::Pattern::TagTuple(), Parse_PatternList(lex));
    case TOK_SQUARE_OPEN:
        throw ParseError::Todo("array patterns");
    default:
        throw ParseError::Unexpected(lex, tok);
    }
}
AST::Pattern Parse_PatternReal_Path(TokenStream& lex, AST::Path path)
{
    Token   tok;
    
    switch( GET_TOK(tok, lex) )
    {
    case TOK_PAREN_OPEN:
        return AST::Pattern(AST::Pattern::TagEnumVariant(), ::std::move(path), Parse_PatternList(lex));
    default:
        lex.putback(tok);
        return AST::Pattern(AST::Pattern::TagValue(), NEWNODE(AST::ExprNode_NamedValue, ::std::move(path)));
    }
}

::std::vector<AST::Pattern> Parse_PatternList(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token tok;
    ::std::vector<AST::Pattern> child_pats;
    do {
        AST::Pattern pat = Parse_Pattern(lex);
        DEBUG("pat = " << pat);
        child_pats.push_back( ::std::move(pat) );
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
        ExprNodeP val = Parse_ExprBlocks(lex);
        opt_semicolon = false;
        return NEWNODE( AST::ExprNode_LetBinding, ::std::move(pat), ::std::move(type), ::std::move(val) );
        }
    case TOK_RWORD_RETURN:
        return NEWNODE( AST::ExprNode_Return, Parse_Expr1(lex) );
    case TOK_RWORD_LOOP:
        throw ParseError::Todo("loop");
    case TOK_RWORD_FOR:
        throw ParseError::Todo("for");
    case TOK_RWORD_WHILE:
        throw ParseError::Todo("while");
    case TOK_RWORD_IF:
        opt_semicolon = true;
        return Parse_IfStmt(lex);
    default:
        lex.putback(tok);
        return Parse_Expr0(lex);
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
    switch( GET_TOK(tok, lex) )
    {
    case TOK_EQUAL:
        return NEWNODE( AST::ExprNode_Assign, AST::ExprNode_Assign::NONE, ::std::move(rv), Parse_Expr1(lex) );
    case TOK_PLUS_EQUAL:
        return NEWNODE( AST::ExprNode_Assign, AST::ExprNode_Assign::ADD, ::std::move(rv), Parse_Expr1(lex) );
    case TOK_DASH_EQUAL:
        return NEWNODE( AST::ExprNode_Assign, AST::ExprNode_Assign::SUB, ::std::move(rv), Parse_Expr1(lex) );
    default:
        lex.putback(tok);
        return rv;
    }
}

/// Parse an 'if' statement
// Note: TOK_RWORD_IF has already been eaten
ExprNodeP Parse_IfStmt(TokenStream& lex)
{
    TRACE_FUNCTION;
    SET_PARSE_FLAG(lex, disallow_struct_literal);

    Token   tok;
    ExprNodeP cond;
    AST::Pattern    pat;
    bool if_let = false;
    if( GET_TOK(tok, lex) == TOK_RWORD_LET ) {
        if_let = true;
        pat = Parse_Pattern(lex);
        GET_CHECK_TOK(tok, lex, TOK_EQUAL);
        cond = Parse_Expr0(lex);
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

    if( if_let )
        return NEWNODE( AST::ExprNode_IfLet, ::std::move(pat), ::std::move(cond), ::std::move(code), ::std::move(altcode) );
    else
        return NEWNODE( AST::ExprNode_If, ::std::move(cond), ::std::move(code), ::std::move(altcode) );
}

ExprNodeP Parse_Expr_Match(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token tok;
    
    // 1. Get expression
    ExprNodeP   switch_val;
    {
        SET_PARSE_FLAG(lex, disallow_struct_literal);
        switch_val = Parse_Expr1(lex);
    }
    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
    
    ::std::vector< AST::ExprNode_Match::Arm >    arms;
    do {
        if( GET_TOK(tok, lex) == TOK_BRACE_CLOSE )
            break;
        lex.putback(tok);
        AST::ExprNode_Match::Arm    arm;
        do {
            arm.m_patterns.push_back( Parse_Pattern(lex) );
        } while( GET_TOK(tok, lex) == TOK_PIPE );
        
        if( tok.type() == TOK_RWORD_IF )
        {
            arm.m_cond = Parse_Expr1(lex);
            GET_TOK(tok, lex);
        }
        CHECK_TOK(tok, TOK_FATARROW);
        
        bool opt_semicolon = false;
        arm.m_code = Parse_Stmt(lex, opt_semicolon);
        
        arms.push_back( ::std::move(arm) );
        
        if( GET_TOK(tok, lex) == TOK_COMMA )
            continue;
        lex.putback(tok);
        
    } while( 1 );
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
    case TOK_BRACE_OPEN:
        lex.putback(tok);
        return Parse_ExprBlockNode(lex);
    case TOK_RWORD_MATCH:
        return Parse_Expr_Match(lex);
    case TOK_RWORD_IF:
        return Parse_IfStmt(lex);
    case TOK_RWORD_UNSAFE: {
        auto rv = Parse_ExprBlockNode(lex);
        dynamic_cast<AST::ExprNode_Block&>(*rv).set_unsafe();
        return rv;
        }
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
            /*::std::cout << "<<" << #cur << ::std::endl; */\
            lex.putback(tok); \
            return rv; \
        } \
    } \
}
// 1: Bool OR
LEFTASSOC(Parse_Expr1, Parse_Expr2,
    case TOK_DOUBLE_PIPE:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::BOOLOR, ::std::move(rv), next(lex));
        break;
)
// 2: Bool AND
LEFTASSOC(Parse_Expr2, Parse_Expr3,
    case TOK_DOUBLE_AMP:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::BOOLAND, ::std::move(rv), next(lex));
        break;
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
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::CMPLT, ::std::move(rv), next(lex));
        break;
    case TOK_GT:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::CMPGT, ::std::move(rv), next(lex));
        break;
    case TOK_LTE:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::CMPLTE, ::std::move(rv), next(lex));
        break;
    case TOK_GTE:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::CMPGTE, ::std::move(rv), next(lex));
        break;
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
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::ADD, ::std::move(rv), next(lex));
        break;
    case TOK_DASH:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::SUB, ::std::move(rv), next(lex));
        break;
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
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::MULTIPLY, ::std::move(rv), next(lex));
        break;
    case TOK_SLASH:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::DIVIDE, ::std::move(rv), next(lex));
        break;
    case TOK_PERCENT:
        throw ParseError::Todo("expr - modulo");
)
// 12: Unaries
ExprNodeP Parse_ExprFC(TokenStream& lex);
ExprNodeP Parse_Expr12(TokenStream& lex)
{
    Token   tok;
    switch(GET_TOK(tok, lex))
    {
    case TOK_DASH:
        return NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::NEGATE, Parse_Expr12(lex) );
    case TOK_EXCLAM:
        return NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::INVERT, Parse_Expr12(lex) );
    case TOK_STAR:
        return NEWNODE( AST::ExprNode_Deref, Parse_Expr12(lex) );
    case TOK_RWORD_BOX:
        return NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::BOX, Parse_Expr12(lex) );
    case TOK_AMP:
        return NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::REF, Parse_Expr12(lex) );
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

ExprNodeP Parse_ExprVal_StructLiteral(TokenStream& lex, AST::Path path)
{
    TRACE_FUNCTION;
    Token   tok;

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

ExprNodeP Parse_FormatArgs(TokenStream& lex)
{
    TRACE_FUNCTION;
    
    Token   tok;
    
    GET_CHECK_TOK(tok, lex, TOK_STRING);
    ::std::string fmt = tok.str();
    
    ::std::vector<ExprNodeP>    nodes;
    
    while( GET_TOK(tok, lex) == TOK_COMMA )
    {
        // TODO: Support named
        auto exp = NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::REF, Parse_Expr1(lex) );
        
        // ( &arg as *const _, &<arg as Trait>::fmt as fn(*const (), &mut Formatter) )
        //nodes.push_back( NEWNODE( AST::ExprNode_Cast, TypeRef
    }
    
    //return NEWNODE( AST::ExprNode_ArrayLiteral, ::std::move(nodes) );
    DEBUG("TODO: Proper support for format_args!");
    return NEWNODE( AST::ExprNode_Tuple, ::std::vector<ExprNodeP>() );
}

ExprNodeP Parse_ExprVal(TokenStream& lex)
{
    TRACE_FUNCTION;
    
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
        case TOK_PAREN_OPEN:
            // Function call
            lex.putback(tok);
            return NEWNODE( AST::ExprNode_CallPath, ::std::move(path), Parse_ParenList(lex) );
        case TOK_BRACE_OPEN:
            if( !CHECK_PARSE_FLAG(lex, disallow_struct_literal) )
                return Parse_ExprVal_StructLiteral(lex, ::std::move(path));
        default:
            // Value
            lex.putback(tok);
            return NEWNODE( AST::ExprNode_NamedValue, ::std::move(path) );
        }
    case TOK_INTEGER:
        return NEWNODE( AST::ExprNode_Integer, tok.intval(), tok.datatype() );
    case TOK_FLOAT:
        return NEWNODE( AST::ExprNode_Float, tok.floatval(), tok.datatype() );
    case TOK_RWORD_TRUE:
        return NEWNODE( AST::ExprNode_Bool, true );
    case TOK_RWORD_FALSE:
        return NEWNODE( AST::ExprNode_Bool, false );
    case TOK_RWORD_SELF:
        return NEWNODE( AST::ExprNode_NamedValue, AST::Path(AST::Path::TagLocal(), "self") );
    case TOK_PAREN_OPEN:
        if( GET_TOK(tok, lex) == TOK_PAREN_CLOSE )
        {
            DEBUG("Unit");
            return NEWNODE( AST::ExprNode_Tuple, ::std::vector<ExprNodeP>() );
        }
        else
        {
            CLEAR_PARSE_FLAG(lex, disallow_struct_literal);
            lex.putback(tok);
            
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
            return rv;
        }
    case TOK_MACRO:
        {
        TokenTree tt = Parse_TT(lex, true);
        if( tt.size() == 0 ) {
            throw ParseError::Unexpected(lex, tt.tok());
        }
        ::std::string name = tok.str();
        
        if( name == "format_args" )
        {
            TTStream    slex(tt);
            return Parse_FormatArgs(slex);
        }
        else
        {
            MacroExpander expanded_macro = Macro_Invoke(name.c_str(), tt);
            return Parse_Expr0(expanded_macro);
        }
        }
    default:
        throw ParseError::Unexpected(lex, tok);
    }
}

// Token Tree Parsing
TokenTree Parse_TT(TokenStream& lex, bool unwrapped)
{
    TRACE_FUNCTION;
    
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
    case TOK_EOF:
    case TOK_NULL:
        throw ParseError::Unexpected(lex, tok);
    default:
        return TokenTree(tok);
    }

    ::std::vector<TokenTree>   items;
    if( !unwrapped )
        items.push_back(tok);
    while(GET_TOK(tok, lex) != closer && tok.type() != TOK_EOF)
    {
        if( tok.type() == TOK_NULL )
            throw ParseError::Unexpected(lex, tok);
        lex.putback(tok);
        items.push_back(Parse_TT(lex, false));
    }
    if( !unwrapped )
        items.push_back(tok);
    return TokenTree(items);
}

TokenTree Parse_TT_Type(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;
    
    ::std::vector<TokenTree>    ret;
    switch(GET_TOK(tok, lex))
    {
    case TOK_AMP:
        throw ParseError::Todo("TokenTree type &-ptr");
    case TOK_STAR:
        throw ParseError::Todo("TokenTree type *-ptr");
    case TOK_DOUBLE_COLON:
    case TOK_IDENT:
        lex.putback(tok);
        return Parse_TT_Path(lex, false);
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    return TokenTree(ret);
}

TokenTree Parse_TT_Path(TokenStream& lex, bool mode_expr)
{
    TRACE_FUNCTION;
    
    Token   tok;
    
    ::std::vector<TokenTree>    ret;
    if( GET_TOK(tok, lex) == TOK_DOUBLE_COLON ) {
        ret.push_back(TokenTree(tok));
    }
    else {
        lex.putback(tok);
    }
    
    for(;;)
    {
        // Expect an ident
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        ret.push_back(TokenTree(tok));
        // If mode is expr, check for a double colon here
        if( mode_expr )
        {
            if( GET_TOK(tok, lex) != TOK_DOUBLE_COLON )
                break;
            ret.push_back( TokenTree(tok) );
        }
        
        if( GET_TOK(tok, lex) == TOK_LT )
        {
            do {
                ret.push_back( TokenTree(tok) );
                ret.push_back(Parse_TT_Type(lex));
            } while(GET_TOK(tok, lex) == TOK_COMMA);
            if( tok.type() != TOK_GT )
            {
                if(tok.type() == TOK_DOUBLE_GT) {
                    ret.push_back(TokenTree(Token(TOK_GT)));
                    lex.putback(Token(TOK_GT));
                }
                else {
                    CHECK_TOK(tok, TOK_GT);
                }
            }
            else {
                ret.push_back(TokenTree(tok));
            }
            
            if( GET_TOK(tok, lex) != TOK_DOUBLE_COLON )
                break;
            ret.push_back(TokenTree(tok));
        }
        else
        {
            lex.putback(tok);
            
            if( !mode_expr )
            {
                if( GET_TOK(tok, lex) != TOK_DOUBLE_COLON )
                    break;
                ret.push_back(TokenTree(tok));
            }
        }
    }
    lex.putback(tok);
    return TokenTree(ret);
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
        return Parse_TT(lex, false);

    case TOK_IDENT:
    case TOK_DOUBLE_COLON: {
        lex.putback(tok);
        TokenTree inner = Parse_TT_Path(lex, true);
        if(GET_TOK(tok, lex) == TOK_BRACE_OPEN) {
            lex.putback(tok);
            ret.push_back(inner);
            ret.push_back(Parse_TT(lex, false));
        }
        else {
            lex.putback(tok);
            return inner;
        }
        break; }
    case TOK_RWORD_SELF:
    case TOK_INTEGER:
    case TOK_FLOAT:
    case TOK_STRING:
        return TokenTree(tok);
    case TOK_RWORD_MATCH:
        ret.push_back(TokenTree(tok));
        ret.push_back(Parse_TT(lex, false));
        break;
    case TOK_RWORD_IF:
        ret.push_back(TokenTree(tok));
        ret.push_back(Parse_TT_Expr(lex));
        if( GET_TOK(tok, lex) == TOK_RWORD_ELSE ) {
            ret.push_back(TokenTree(tok));
            ret.push_back(Parse_TT(lex, false));
        }
        else {
            lex.putback(tok);
        }
        break;
    default:
        // Oh, fail :(
        throw ParseError::Unexpected(lex, tok);
    }
    return TokenTree(ret);
}
/// Parse a token tree expression
TokenTree Parse_TT_Expr(TokenStream& lex)
{
    TRACE_FUNCTION;
    
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
        case TOK_SLASH:
        case TOK_STAR:
        case TOK_PERCENT:
            ret.push_back(tok);
            ret.push_back(Parse_TT_Val(lex));
            break;
        case TOK_PAREN_OPEN:
            lex.putback(tok);
            ret.push_back(Parse_TT(lex, false));
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
                ret.push_back(Parse_TT(lex, false));
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
