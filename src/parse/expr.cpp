/*
 */
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
ExprNodeP Parse_Stmt(TokenStream& lex);
ExprNodeP Parse_Expr0(TokenStream& lex);
ExprNodeP Parse_IfStmt(TokenStream& lex);
ExprNodeP Parse_WhileStmt(TokenStream& lex, ::std::string lifetime);
ExprNodeP Parse_ForStmt(TokenStream& lex, ::std::string lifetime);
ExprNodeP Parse_Expr_Match(TokenStream& lex);
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

AST::Pattern Parse_PatternReal(TokenStream& lex);
AST::Pattern Parse_PatternReal1(TokenStream& lex);

AST::Pattern Parse_PatternReal(TokenStream& lex)
{
    Token   tok;
    AST::Pattern    ret = Parse_PatternReal1(lex);
    if( GET_TOK(tok, lex) == TOK_TRIPLE_DOT )
    {
        if( ret.type() != AST::Pattern::VALUE )
            throw ParseError::Generic(lex, "Using '...' with a non-value on left");
        auto    leftval = ret.take_node();
        auto    right_pat = Parse_PatternReal1(lex);
        if( right_pat.type() != AST::Pattern::VALUE )
            throw ParseError::Generic(lex, "Using '...' with a non-value on right");
        auto    rightval = right_pat.take_node();
        
        return AST::Pattern(AST::Pattern::TagValue(), ::std::move(leftval), ::std::move(rightval));
    }
    else
    {
        lex.putback(tok);
        return ret;
    }
}
AST::Pattern Parse_PatternReal1(TokenStream& lex)
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
        if( GET_TOK(tok, lex) == TOK_RWORD_MUT )
            // TODO: Actually use mutability
            return AST::Pattern( AST::Pattern::TagReference(), Parse_PatternReal(lex) );
        lex.putback(tok);
        return AST::Pattern( AST::Pattern::TagReference(), Parse_PatternReal(lex) );
    case TOK_IDENT:
        lex.putback(tok);
        return Parse_PatternReal_Path( lex, Parse_Path(lex, false, PATH_GENERIC_EXPR) );
    case TOK_DOUBLE_COLON:
        // 2. Paths are enum/struct names
        return Parse_PatternReal_Path( lex, Parse_Path(lex, true, PATH_GENERIC_EXPR) );
    case TOK_DASH:
        GET_CHECK_TOK(tok, lex, TOK_INTEGER);
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE(AST::ExprNode_Integer, -tok.intval(), tok.datatype()) );
    case TOK_INTEGER:
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE(AST::ExprNode_Integer, tok.intval(), tok.datatype()) );
    case TOK_RWORD_TRUE:
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE( AST::ExprNode_Bool, true ) );
    case TOK_RWORD_FALSE:
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE( AST::ExprNode_Bool, false ) );
    case TOK_STRING:
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE(AST::ExprNode_String, tok.str()) );
    case TOK_PAREN_OPEN:
        return AST::Pattern(AST::Pattern::TagTuple(), Parse_PatternList(lex));
    case TOK_SQUARE_OPEN:
        throw ParseError::Todo(lex, "array patterns");
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

ExprNodeP Parse_ExprBlockNode(TokenStream& lex);
ExprNodeP Parse_ExprBlockLine(TokenStream& lex, bool *expect_end);
void Parse_ExternBlock(TokenStream& lex, AST::MetaItems attrs, ::std::vector< AST::Item<AST::Function> >& imports);

ExprNodeP Parse_ExprBlockNode(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;

    ::std::vector<ExprNodeP> nodes;
    ::std::unique_ptr<AST::Module>    local_mod;
    
    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        AST::MetaItems  item_attrs;
        while( tok.type() == TOK_ATTR_OPEN )
        {
            item_attrs.push_back( Parse_MetaItem(lex) );
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            GET_TOK(tok, lex);
        }
        
        switch(tok.type())
        {
        // Items:
        // - 'use'
        case TOK_RWORD_USE:
            if( !local_mod.get() )  local_mod.reset( new AST::Module("") );
            Parse_Use(
                lex,
                [&local_mod](AST::Path p, std::string s) {
                    local_mod->imports().push_back( AST::Item<AST::Path>( ::std::move(s), ::std::move(p), false ) );
                }
                );
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            break;
        // 'extern' blocks
        case TOK_RWORD_EXTERN:
            if( !local_mod.get() )  local_mod.reset( new AST::Module("") );
            Parse_ExternBlock(lex, ::std::move(item_attrs), local_mod->functions());
            break;
        // - 'const'
        case TOK_RWORD_CONST:
            if( !local_mod.get() )  local_mod.reset( new AST::Module("") );
            {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string   name = tok.str();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            TypeRef type = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);
            auto val = Parse_Expr1(lex);
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            
            local_mod->statics().push_back( AST::Item<AST::Static>(
                ::std::move(name),
                AST::Static(AST::Static::CONST, ::std::move(type), ::std::move(val)),
                false ) );
            break;
            }
        // - 'struct'
        case TOK_RWORD_STRUCT:
            if( !local_mod.get() )  local_mod.reset( new AST::Module("") );
            Parse_Struct(*local_mod, lex, false, item_attrs);
            break;
        // - 'impl'
        case TOK_RWORD_IMPL:
            if( !local_mod.get() )  local_mod.reset( new AST::Module("") );
            local_mod->add_impl(Parse_Impl(lex, false));
            break;
        // - 'fn'
        case TOK_RWORD_FN:
            if( !local_mod.get() )  local_mod.reset( new AST::Module("") );
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            // - self not allowed, not prototype
            local_mod->add_function(
                false, tok.str(), Parse_FunctionDefWithCode(lex, "rust", ::std::move(item_attrs), false)
                );
            break;
        default: {
            lex.putback(tok);
            bool    expect_end = false;
            nodes.push_back(Parse_ExprBlockLine(lex, &expect_end));
            // Set to TRUE if there was no semicolon after a statement
            if( expect_end )
            {
                if( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
                {
                    throw ParseError::Unexpected(lex, tok, Token(TOK_BRACE_CLOSE));
                }
                lex.putback(tok);
            }
            break;
            }
        }
    }
    return NEWNODE( AST::ExprNode_Block, ::std::move(nodes) );
}

/// Parse a single line from a block
///
/// Handles:
/// - Block-level constructs (with lifetime annotations)
/// - use/extern/const/let
ExprNodeP Parse_ExprBlockLine(TokenStream& lex, bool *expect_end)
{
    TRACE_FUNCTION;
    
    Token tok;
    
    if( GET_TOK(tok, lex) == TOK_LIFETIME )
    {
        // Lifetimes can only precede loops... and blocks?
        ::std::string lifetime = tok.str();
        GET_CHECK_TOK(tok, lex, TOK_COLON);
        
        switch( GET_TOK(tok, lex) )
        {
        case TOK_RWORD_LOOP:
            return NEWNODE( AST::ExprNode_Loop, lifetime, Parse_ExprBlockNode(lex) );
        case TOK_RWORD_WHILE:
            return Parse_WhileStmt(lex, lifetime);
        case TOK_RWORD_FOR:
            return Parse_ForStmt(lex, lifetime);
        case TOK_RWORD_IF:
            return Parse_IfStmt(lex);
        case TOK_RWORD_MATCH:
            return Parse_Expr_Match(lex);
        case TOK_BRACE_OPEN:
            lex.putback(tok);
            return Parse_ExprBlockNode(lex);
    
        default:
            throw ParseError::Unexpected(lex, tok);
        }
    }
    else
    {
        switch( tok.type() )
        {
        case TOK_SEMICOLON:
            return 0;
        case TOK_BRACE_OPEN:
            lex.putback(tok);
            return Parse_ExprBlockNode(lex);
        
        // let binding
        case TOK_RWORD_LET: {
            AST::Pattern pat = Parse_Pattern(lex);
            TypeRef type;
            if( GET_TOK(tok, lex) == TOK_COLON ) {
                type = Parse_Type(lex);
                GET_CHECK_TOK(tok, lex, TOK_EQUAL);
            }
            else {
                CHECK_TOK(tok, TOK_EQUAL);
            }
            ExprNodeP val = Parse_Expr0(lex);
            return NEWNODE( AST::ExprNode_LetBinding, ::std::move(pat), ::std::move(type), ::std::move(val) );
            }
        
        // blocks that don't need semicolons
        case TOK_RWORD_LOOP:
            return NEWNODE( AST::ExprNode_Loop, "", Parse_ExprBlockNode(lex) );
        case TOK_RWORD_WHILE:
            return Parse_WhileStmt(lex, "");
        case TOK_RWORD_FOR:
            return Parse_ForStmt(lex, "");
        case TOK_RWORD_IF:
            return Parse_IfStmt(lex);
        case TOK_RWORD_MATCH:
            return Parse_Expr_Match(lex);
        case TOK_RWORD_UNSAFE: {
            auto rv = Parse_ExprBlockNode(lex);
            dynamic_cast<AST::ExprNode_Block&>(*rv).set_unsafe();
            return rv;
            }
        
        // Fall through to the statement code
        default: {
            lex.putback(tok);
            auto ret = Parse_Stmt(lex);
            if( GET_TOK(tok, lex) != TOK_SEMICOLON )
            {
                lex.putback(tok);
                *expect_end = true;
            }
            return ::std::move(ret);
            break;
            }
        }
    }
}
/// Extern block within a block
void Parse_ExternBlock(TokenStream& lex, AST::MetaItems attrs, ::std::vector< AST::Item<AST::Function> >& imports)
{
    Token tok;
    
    // - default ABI is "C"
    ::std::string    abi = "C";
    if( GET_TOK(tok, lex) == TOK_STRING ) {
        abi = tok.str();
    }
    else
        lex.putback(tok);
    
    bool is_block = false;
    if( GET_TOK(tok, lex) == TOK_BRACE_OPEN ) {
        is_block = true;
    }
    else
        lex.putback(tok);
    
    do {
        AST::MetaItems  inner_attrs;
        if( is_block )
        {
            while( GET_TOK(tok, lex) == TOK_ATTR_OPEN )
            {
                inner_attrs.push_back( Parse_MetaItem(lex) );
                GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            }
            lex.putback(tok);
        }
        else
        {
            inner_attrs = attrs;
        }
        ::std::string   name;
        switch( GET_TOK(tok, lex) )
        {
        case TOK_RWORD_FN:
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            name = tok.str();
            imports.push_back( AST::Item<AST::Function>(
                ::std::move(name),
                Parse_FunctionDef(lex, abi, AST::MetaItems(), false, true),
                false
                ) );
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            break;
        default:
            throw ParseError::Unexpected(lex, tok);
        }
    } while( is_block && LOOK_AHEAD(lex) != TOK_BRACE_CLOSE );
    if( is_block )
        GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
    else
        GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
}
/// While loop (either as a statement, or as part of an expression)
ExprNodeP Parse_WhileStmt(TokenStream& lex, ::std::string lifetime)
{
    Token   tok;
    
    if( GET_TOK(tok, lex) == TOK_RWORD_LET ) {
        auto pat = Parse_Pattern(lex);
        GET_CHECK_TOK(tok, lex, TOK_EQUAL);
        ExprNodeP val;
        {
            SET_PARSE_FLAG(lex, disallow_struct_literal);
            val = Parse_Expr0(lex);
        }
        return NEWNODE( AST::ExprNode_Loop, lifetime, AST::ExprNode_Loop::WHILELET,
            ::std::move(pat), ::std::move(val), Parse_ExprBlockNode(lex) );
    }
    else {
        lex.putback(tok);
        ExprNodeP cnd = Parse_Expr1(lex);
        return NEWNODE( AST::ExprNode_Loop, lifetime, ::std::move(cnd), Parse_ExprBlockNode(lex) );
    }
}
/// For loop (either as a statement, or as part of an expression)
ExprNodeP Parse_ForStmt(TokenStream& lex, ::std::string lifetime)
{
    Token   tok;
    
    AST::Pattern    pat = Parse_Pattern(lex);
    GET_CHECK_TOK(tok, lex, TOK_RWORD_IN);
    ExprNodeP val;
    {
        SET_PARSE_FLAG(lex, disallow_struct_literal);
        val = Parse_Expr0(lex);
    }
    return NEWNODE( AST::ExprNode_Loop, lifetime, AST::ExprNode_Loop::FOR,
            ::std::move(pat), ::std::move(val), Parse_ExprBlockNode(lex) );
}
/// Parse an 'if' statement
// Note: TOK_RWORD_IF has already been eaten
ExprNodeP Parse_IfStmt(TokenStream& lex)
{
    TRACE_FUNCTION;

    Token   tok;
    ExprNodeP cond;
    AST::Pattern    pat;
    bool if_let = false;
    
    {
        SET_PARSE_FLAG(lex, disallow_struct_literal);
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
/// "match" block
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
    ASSERT(lex, !CHECK_PARSE_FLAG(lex, disallow_struct_literal) );
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
        
        arm.m_code = Parse_Stmt(lex);
        
        arms.push_back( ::std::move(arm) );
        
        if( GET_TOK(tok, lex) == TOK_COMMA )
            continue;
        lex.putback(tok);
        
    } while( 1 );
    CHECK_TOK(tok, TOK_BRACE_CLOSE);
    
    return NEWNODE( AST::ExprNode_Match, ::std::move(switch_val), ::std::move(arms) );
}

/// Parses the 'stmt' fragment specifier
/// - Flow control
/// - Expressions
ExprNodeP Parse_Stmt(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;
    
    switch(GET_TOK(tok, lex))
    {
    case TOK_RWORD_RETURN: {
        ExprNodeP   val;
        if( GET_TOK(tok, lex) != TOK_SEMICOLON ) {
            lex.putback(tok);
            val = Parse_Expr1(lex);
        }
        else
            lex.putback(tok);
        return NEWNODE( AST::ExprNode_Flow, AST::ExprNode_Flow::RETURN, "", ::std::move(val) );
        }
    case TOK_RWORD_CONTINUE:
    case TOK_RWORD_BREAK:
        {
        AST::ExprNode_Flow::Type    type;
        switch(tok.type())
        {
        case TOK_RWORD_CONTINUE: type = AST::ExprNode_Flow::CONTINUE; break;
        case TOK_RWORD_BREAK:    type = AST::ExprNode_Flow::BREAK;    break;
        default:    throw ParseError::BugCheck(/*lex,*/ "continue/break");
        }
        ::std::string   lifetime;
        if( GET_TOK(tok, lex) == TOK_LIFETIME )
        {
            lifetime = tok.str();
            GET_TOK(tok, lex);
        }
        ExprNodeP   val;
        if( tok.type() != TOK_SEMICOLON && tok.type() != TOK_COMMA && tok.type() != TOK_BRACE_CLOSE ) {
            lex.putback(tok);
            val = Parse_Expr1(lex);
        }
        else
            lex.putback(tok);
        return NEWNODE( AST::ExprNode_Flow, type, lifetime, ::std::move(val) );
        } 
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
            rv.push_back( Parse_Expr0(lex) );
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

    ExprNodeP rv = Parse_Expr1(lex);
    auto op = AST::ExprNode_Assign::NONE;
    switch( GET_TOK(tok, lex) )
    {
    case TOK_PLUS_EQUAL:
        op = AST::ExprNode_Assign::ADD; if(0)
    case TOK_DASH_EQUAL:
        op = AST::ExprNode_Assign::SUB; if(0)
    case TOK_STAR_EQUAL:
        op = AST::ExprNode_Assign::MUL; if(0)
    case TOK_SLASH_EQUAL:
        op = AST::ExprNode_Assign::DIV; if(0)
    
    case TOK_AMP_EQUAL:
        op = AST::ExprNode_Assign::AND; if(0)
    case TOK_PIPE_EQUAL:
        op = AST::ExprNode_Assign::OR ; if(0)
    case TOK_CARET_EQUAL:
        op = AST::ExprNode_Assign::XOR; if(0)
    
    case TOK_DOUBLE_GT_EQUAL:
        op = AST::ExprNode_Assign::SHR; if(0)
    case TOK_DOUBLE_LT_EQUAL:
        op = AST::ExprNode_Assign::SHL; if(0)

    case TOK_EQUAL:
        op = AST::ExprNode_Assign::NONE;
        return NEWNODE( AST::ExprNode_Assign, op, ::std::move(rv), Parse_Expr1(lex) );
    
    default:
        lex.putback(tok);
        return rv;
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
bool Parse_IsTokValue(eTokenType tok_type)
{
    switch( tok_type )
    {
    case TOK_DOUBLE_COLON:
    case TOK_IDENT:
    case TOK_INTEGER:
    case TOK_FLOAT:
    case TOK_STRING:
    case TOK_RWORD_TRUE:
    case TOK_RWORD_FALSE:
    case TOK_RWORD_SELF:
    case TOK_RWORD_BOX:
    case TOK_PAREN_OPEN:
        return true;
    default:
        return false;
    }
    
}
ExprNodeP Parse_Expr1_5(TokenStream& lex);
// Very evil handling for '..'
ExprNodeP Parse_Expr1(TokenStream& lex)
{
    Token   tok;
    ExprNodeP (*next)(TokenStream&) = Parse_Expr1_5;
    ExprNodeP   left, right;
    
    if( GET_TOK(tok, lex) != TOK_DOUBLE_DOT )
    {
        lex.putback(tok);
        
        left = next(lex);
        
        if( GET_TOK(tok, lex) != TOK_DOUBLE_DOT )
        {
            lex.putback(tok);
            return ::std::move(left);
        }
    }
    if( Parse_IsTokValue( GET_TOK(tok, lex) ) )
    {
        lex.putback(tok);
        right = next(lex);
    }
    else
    {
        lex.putback(tok);
    }
    
    return NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::RANGE, ::std::move(left), ::std::move(right) );
}
// 1: Bool OR
LEFTASSOC(Parse_Expr1_5, Parse_Expr2,
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
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::MODULO, ::std::move(rv), next(lex));
        break;
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
        if( GET_TOK(tok, lex) == TOK_RWORD_MUT )
            return NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::REFMUT, Parse_Expr12(lex) );
        else {
            lex.putback(tok);
            return NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::REF, Parse_Expr12(lex) );
        }
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
        case TOK_SQUARE_OPEN:
            val = NEWNODE( AST::ExprNode_Index, ::std::move(val), Parse_Expr0(lex) );
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            break;
        case TOK_DOT:
            // Field access / method call
            // TODO: What about tuple indexing?
            switch(GET_TOK(tok, lex))
            {
            case TOK_IDENT: {
                AST::PathNode   path(tok.str(), {});
                switch( GET_TOK(tok, lex) )
                {
                case TOK_PAREN_OPEN:
                    lex.putback(tok);
                    val = NEWNODE( AST::ExprNode_CallMethod, ::std::move(val), ::std::move(path), Parse_ParenList(lex) );
                    break;
                case TOK_DOUBLE_COLON:
                    GET_CHECK_TOK(tok, lex, TOK_LT);
                    path.args() = Parse_Path_GenericList(lex);
                    val = NEWNODE( AST::ExprNode_CallMethod, ::std::move(val), ::std::move(path), Parse_ParenList(lex) );
                    break;
                default:
                    val = NEWNODE( AST::ExprNode_Field, ::std::move(val), ::std::string(path.name()) );
                    lex.putback(tok);
                    break;
                }
                break; }
            case TOK_INTEGER:
                val = NEWNODE( AST::ExprNode_Field, ::std::move(val), FMT(tok.intval()) );
                break;
            default:
                throw ParseError::Unexpected(lex, tok);
            }
            break;
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

ExprNodeP Parse_ExprVal_Closure(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;
    
    ::std::vector< ::std::pair<AST::Pattern, TypeRef> > args;
    
    while( GET_TOK(tok, lex) != TOK_PIPE )
    {
        lex.putback(tok);
        AST::Pattern    pat = Parse_Pattern(lex);
    
        TypeRef type;
        if( GET_TOK(tok, lex) == TOK_COLON )
            type = Parse_Type(lex);
        else
            lex.putback(tok);
        
        args.push_back( ::std::make_pair( ::std::move(pat), ::std::move(type) ) );
        
        if( GET_TOK(tok, lex) != TOK_COMMA )
            break;
    }
    CHECK_TOK(tok, TOK_PIPE);
    
    TypeRef rt;
    if( GET_TOK(tok, lex) == TOK_COLON )
        rt = Parse_Type(lex);
    else
        lex.putback(tok);
    
    auto code = Parse_Expr0(lex);
    
    return NEWNODE( AST::ExprNode_Closure, ::std::move(args), ::std::move(rt), ::std::move(code) );
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
    case TOK_BRACE_OPEN:
        lex.putback(tok);
        return Parse_ExprBlockNode(lex);
    case TOK_RWORD_LOOP:
        return NEWNODE( AST::ExprNode_Loop, "", Parse_ExprBlockNode(lex) );
    case TOK_RWORD_WHILE:
        return Parse_WhileStmt(lex, "");
    case TOK_RWORD_FOR:
        return Parse_ForStmt(lex, "");
    case TOK_RWORD_MATCH:
        return Parse_Expr_Match(lex);
    case TOK_RWORD_IF:
        return Parse_IfStmt(lex);
    case TOK_RWORD_UNSAFE: {
        auto rv = Parse_ExprBlockNode(lex);
        dynamic_cast<AST::ExprNode_Block&>(*rv).set_unsafe();
        return rv;
        }
    
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
            else
                DEBUG("Not parsing struct literal");
        default:
            // Value
            lex.putback(tok);
            return NEWNODE( AST::ExprNode_NamedValue, ::std::move(path) );
        }
    case TOK_DOUBLE_PIPE:
        lex.putback(Token(TOK_PIPE));
    case TOK_PIPE:
        return Parse_ExprVal_Closure(lex);
    case TOK_INTEGER:
        return NEWNODE( AST::ExprNode_Integer, tok.intval(), tok.datatype() );
    case TOK_FLOAT:
        return NEWNODE( AST::ExprNode_Float, tok.floatval(), tok.datatype() );
    case TOK_STRING:
        return NEWNODE( AST::ExprNode_String, tok.str() );
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
    case TOK_SQUARE_OPEN:
        if( GET_TOK(tok, lex) == TOK_SQUARE_CLOSE )
        {
            // Empty literal
            return NEWNODE( AST::ExprNode_Array, ::std::vector<ExprNodeP>() );
        }
        else
        {
            lex.putback(tok);
            auto first = Parse_Expr0(lex);
            if( GET_TOK(tok, lex) == TOK_SEMICOLON )
            {
                // Repetiion
                auto count = Parse_Expr0(lex);
                GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
                return NEWNODE( AST::ExprNode_Array, ::std::move(first), ::std::move(count) );
            }
            else
            {
                ::std::vector<ExprNodeP>    items;
                items.push_back( ::std::move(first) );
                while( tok.type() == TOK_COMMA )
                {
                    items.push_back( Parse_Expr0(lex) );
                    GET_TOK(tok, lex);
                }
                CHECK_TOK(tok, TOK_SQUARE_CLOSE);
                return NEWNODE( AST::ExprNode_Array, ::std::move(items) );
            }
        }
        throw ParseError::BugCheck(lex, "Array literal fell");
    case TOK_MACRO:
        {
        TokenTree tt = Parse_TT(lex, true);
        if( tt.is_token() ) {
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
            auto expanded_macro = Macro_Invoke(lex, name, tt);
            return Parse_Expr0(*expanded_macro);
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

/// A wrapping lexer that 
class TTLexer:
    public TokenStream
{
    TokenStream&    m_input;
    Token   m_last_token;
    ::std::vector<TokenTree>    m_output;
public:
    TTLexer(TokenStream& input):
        m_input(input)
    {
    }
    
    virtual Position getPosition() const override { return m_input.getPosition(); }
    virtual Token realGetToken() override {
        Token tok = m_input.getToken();
        m_output.push_back( TokenTree(tok) );
        return tok;
    }

    TokenTree   get_output() {
        unsigned int eat = (TokenStream::m_cache_valid ? 1 : 0) + TokenStream::m_lookahead.size();
        DEBUG(eat << " tokens were not consumed");
        assert( m_output.size() >= eat );
        assert( m_input.m_lookahead.size() == 0 );
        assert( m_input.m_cache_valid == false );
        for( unsigned int i = 0; i < eat; i ++ )
        {
            Token tok = m_output[ m_output.size() - eat + i ].tok();
            DEBUG("Unconsume " << tok);
            m_input.m_lookahead.push_back( tok );
        }
        DEBUG("- output was [" << m_output << "]");
        m_output.erase( m_output.end() - eat, m_output.end() );
        DEBUG("Returning [" << m_output << "]");
        return ::std::move(m_output);
    }
};

TokenTree Parse_TT_Type(TokenStream& lex)
{
    TRACE_FUNCTION;
    TTLexer wlex(lex);
    
    // discard result
    Parse_Type(wlex);
    
    return wlex.get_output();
}

/// Parse a token tree path
TokenTree Parse_TT_Path(TokenStream& lex, bool mode_expr)
{
    TRACE_FUNCTION;
    TTLexer wlex(lex);
    Token   tok;
    
    if( GET_TOK(tok, wlex) == TOK_DOUBLE_COLON ) {
        Parse_Path(wlex, true, (mode_expr ? PATH_GENERIC_EXPR : PATH_GENERIC_TYPE));
    }
    else {
        wlex.putback(tok);
        Parse_Path(wlex, false, (mode_expr ? PATH_GENERIC_EXPR : PATH_GENERIC_TYPE));
    }
    
    return wlex.get_output();
}
/// Parse a token tree expression
TokenTree Parse_TT_Expr(TokenStream& lex)
{
    TRACE_FUNCTION;
    TTLexer wlex(lex);
    
    Parse_Expr1(wlex);
    
    return wlex.get_output();
}
TokenTree Parse_TT_Stmt(TokenStream& lex)
{
    throw ParseError::Todo("Parse_TT_Stmt");
}
TokenTree Parse_TT_Block(TokenStream& lex)
{
    throw ParseError::Todo("Parse_TT_Block");
}
