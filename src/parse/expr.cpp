/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/expr.cpp
 * - Expression (i.e. code) parsing
 *
 * Start points:
 * - Parse_ExprBlockNode : Parses a block
 * - Parse_Stmt : Parse a single statement
 * - Parse_Expr0 : Parse a single expression
 */
#include "parseerror.hpp"
#include <ast/ast.hpp>
#include <ast/expr.hpp>
#include "common.hpp"
#include <iostream>
#include "tokentree.hpp"
#include "interpolated_fragment.hpp"

using AST::ExprNode;
using AST::ExprNodeP;
// TODO: Use a ProtoSpan instead of a point span?
static inline ExprNodeP mk_exprnodep(const TokenStream& lex, AST::ExprNode* en){en->set_span(lex.point_span()); return ExprNodeP(en); }
#define NEWNODE(type, ...)  mk_exprnodep(lex, new type(__VA_ARGS__))

ExprNodeP Parse_ExprBlockNode(TokenStream& lex, AST::ExprNode_Block::Type ty, Ident label=Ident(""));
//ExprNodeP Parse_ExprBlockLine_WithItems(TokenStream& lex, ::std::shared_ptr<AST::Module>& local_mod, bool& add_silence_if_end);
//ExprNodeP Parse_ExprBlockLine(TokenStream& lex, bool *add_silence);
ExprNodeP Parse_ExprBlockLine_Stmt(TokenStream& lex, bool& has_semicolon);
//ExprNodeP Parse_Stmt(TokenStream& lex);   // common.hpp
ExprNodeP Parse_Stmt_Let(TokenStream& lex);
ExprNodeP Parse_Expr0(TokenStream& lex);
ExprNodeP Parse_Expr1_5(TokenStream& lex);  // Boolean OR
ExprNodeP Parse_Expr3(TokenStream& lex);
ExprNodeP Parse_IfStmt(TokenStream& lex);
ExprNodeP Parse_WhileStmt(TokenStream& lex, Ident lifetime);
ExprNodeP Parse_ForStmt(TokenStream& lex, Ident lifetime);
ExprNodeP Parse_Expr_Match(TokenStream& lex);
ExprNodeP Parse_Expr1(TokenStream& lex);
ExprNodeP Parse_ExprFC(TokenStream& lex);
ExprNodeP Parse_ExprMacro(TokenStream& lex, AST::Path tok);

AST::Expr Parse_Expr(TokenStream& lex)
{
    return ::AST::Expr( Parse_Expr0(lex) );
}

AST::Expr Parse_ExprBlock(TokenStream& lex)
{
    return ::AST::Expr( Parse_ExprBlockNode(lex) );
}
AST::ExprNodeP Parse_ExprBlockNode(TokenStream& lex)
{
    return Parse_ExprBlockNode(lex, AST::ExprNode_Block::Type::Bare, RcString());
}
ExprNodeP Parse_ExprBlockNode(TokenStream& lex, AST::ExprNode_Block::Type ty/*=Bare*/, Ident label/*=RcString()*/)
{
    TRACE_FUNCTION;
    CLEAR_PARSE_FLAGS_EXPR(lex);
    Token   tok;

    ::std::vector<AST::ExprNode_Block::Line> lines;
    AST::AttributeList  attrs;


    auto orig_module = lex.parse_state().module;
    ::std::shared_ptr<AST::Module> local_mod;

    if( LOOK_AHEAD(lex) == TOK_INTERPOLATED_BLOCK )
    {
        GET_TOK(tok, lex);
        return tok.take_frag_node();
    }

    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);

    while( LOOK_AHEAD(lex) != TOK_BRACE_CLOSE )
    {
        Parse_ParentAttrs(lex, attrs);
        if( LOOK_AHEAD(lex) == TOK_BRACE_CLOSE )
            break;

        bool    add_silence_if_end = false;
        // `add_silence_if_end` indicates that the statement had a semicolon.
        auto rv = Parse_ExprBlockLine_WithItems(lex, local_mod, add_silence_if_end);
        if( rv ) {
            // Set to TRUE if there was no semicolon after a statement
            lines.push_back({ add_silence_if_end, mv$(rv) });
        }
        else {
            assert( !add_silence_if_end );
        }
    }
    GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);

    if( lex.parse_state().module != orig_module ) {
        DEBUG("Restore module from " << lex.parse_state().module->path() << " to " << orig_module->path() );
        lex.parse_state().module = orig_module;
    }
    auto* rv_blk = new ::AST::ExprNode_Block(ty, mv$(lines), mv$(local_mod) );
    rv_blk->m_label = label;
    auto rv = ExprNodeP(rv_blk);
    rv->set_attrs( mv$(attrs) );
    return rv;
}

/// Parse a single line in a block, handling items added to the local module
///
/// - If an item was parsed, this returns an empty ExprNodeP
ExprNodeP Parse_ExprBlockLine_WithItems(TokenStream& lex, ::std::shared_ptr<AST::Module>& local_mod, bool& add_silence_if_end)
{
    Token   tok;

    auto item_attrs = Parse_ItemAttrs(lex);
    GET_TOK(tok, lex);

    // `union Ident` - contextual keyword
    if( tok.type() == TOK_IDENT && tok.ident().name == "union" && lex.lookahead(0) == TOK_IDENT ) {
        PUTBACK(tok, lex);
        if( !local_mod ) {
            local_mod = lex.parse_state().get_current_mod().add_anon();
            DEBUG("Set module from " << lex.parse_state().module->path() << " to " << local_mod->path() );
            lex.parse_state().module = local_mod.get();
        }
        Parse_Mod_Item(lex, *local_mod, mv$(item_attrs));
        return ExprNodeP();
    }

    if( tok.type() == TOK_IDENT && tok.ident().name == "macro_rules" && lex.lookahead(0) == TOK_EXCLAM )
    {
        // Special case - create a local module if macro_rules! is seen
        // - Allows correct scoping of defined macros
        if( !local_mod ) {
            local_mod = lex.parse_state().get_current_mod().add_anon();
            DEBUG("Set module from " << lex.parse_state().module->path() << " to " << local_mod->path() );
            lex.parse_state().module = local_mod.get();
        }
    }

    switch(tok.type())
    {
    // Items:
    case TOK_RWORD_CRATE:
        if( lex.lookahead(0) == TOK_DOUBLE_COLON ) {
            break;
        }
    case TOK_INTERPOLATED_VIS:
    case TOK_INTERPOLATED_ITEM:
    case TOK_RWORD_PUB:
        // NOTE: Allowed, but doesn't do much
    case TOK_RWORD_TYPE:
    case TOK_RWORD_USE:
    case TOK_RWORD_EXTERN:
    case TOK_RWORD_STATIC:
    case TOK_RWORD_STRUCT:
    case TOK_RWORD_MACRO:
    case TOK_RWORD_ENUM:
    case TOK_RWORD_TRAIT:
    case TOK_RWORD_IMPL:
    case TOK_RWORD_FN:
    case TOK_RWORD_MOD:
        PUTBACK(tok, lex);
        if( !local_mod ) {
            local_mod = lex.parse_state().get_current_mod().add_anon();
            DEBUG("Set module from " << lex.parse_state().module->path() << " to " << local_mod->path() );
            lex.parse_state().module = local_mod.get();
        }
        Parse_Mod_Item(lex, *local_mod, mv$(item_attrs));
        return ExprNodeP();
    // 'const' - Check if the next token isn't a `{`, if so it's an item. Otherwise, fall through
    case TOK_RWORD_CONST:
        if( LOOK_AHEAD(lex) != TOK_BRACE_OPEN )
        {
            PUTBACK(tok, lex);
            if( !local_mod ) {
                local_mod = lex.parse_state().get_current_mod().add_anon();
                DEBUG("Set module from " << lex.parse_state().module->path() << " to " << local_mod->path() );
                lex.parse_state().module = local_mod.get();
            }
            Parse_Mod_Item(lex, *local_mod, mv$(item_attrs));
            return ExprNodeP();
        }
        break;
    // 'unsafe' - Check if the next token isn't a `{`, if so it's an item. Otherwise, fall through
    case TOK_RWORD_UNSAFE:
        if( LOOK_AHEAD(lex) != TOK_BRACE_OPEN )
        {
            PUTBACK(tok, lex);
            if( !local_mod ) {
                local_mod = lex.parse_state().get_current_mod().add_anon();
                DEBUG("Set module from " << lex.parse_state().module->path() << " to " << local_mod->path() );
                lex.parse_state().module = local_mod.get();
            }
            Parse_Mod_Item(lex, *local_mod, mv$(item_attrs));
            return ExprNodeP();
        }
        // fall
    default:
        break;
    }
    PUTBACK(tok, lex);
    auto rv = Parse_ExprBlockLine(lex, &add_silence_if_end);
    if( rv ) {
        rv->set_attrs( mv$(item_attrs) );
    }
    else if( item_attrs.m_items.size() > 0 ) {
        // TODO: Is this an error? - Attributes on a expression that didn't yeild a node.
        // - They should have applied to the item that was parsed?
    }
    else {
    }
    return rv;
}

/// Parse a single line from a block
///
/// Handles:
/// - Block-level constructs (with lifetime annotations)
/// - use/extern/const/let
ExprNodeP Parse_ExprBlockLine(TokenStream& lex, bool *add_silence)
{
    TRACE_FUNCTION;
    Token tok;
    ExprNodeP   ret;
    bool add_silence_ignored = false;
    if( !add_silence ) {
        add_silence = &add_silence_ignored;
    }

    if( GET_TOK(tok, lex) == TOK_LIFETIME )
    {
        // Lifetimes can only precede loops... and blocks?
        auto lifetime = tok.ident();
        GET_CHECK_TOK(tok, lex, TOK_COLON);

        switch( GET_TOK(tok, lex) )
        {
        case TOK_RWORD_LOOP:
            return NEWNODE( AST::ExprNode_Loop, lifetime, Parse_ExprBlockNode(lex) );
        case TOK_RWORD_WHILE:
            return Parse_WhileStmt(lex, lifetime);
        case TOK_RWORD_FOR:
            return Parse_ForStmt(lex, lifetime);
        // NOTE: 1.39's libsyntax uses labelled block
        case TOK_BRACE_OPEN:
            PUTBACK(tok, lex);
            ret = Parse_ExprBlockNode(lex, /*is_unsafe*/AST::ExprNode_Block::Type::Bare, lifetime);
            return ret;
        case TOK_RWORD_UNSAFE:
            ret = Parse_ExprBlockNode(lex, /*is_unsafe*/AST::ExprNode_Block::Type::Unsafe, lifetime);
            return ret;
        case TOK_RWORD_CONST:
            ret = Parse_ExprBlockNode(lex, /*is_unsafe*/AST::ExprNode_Block::Type::Const, lifetime);
            return ret;
        // TODO: Can these have labels?
        //case TOK_RWORD_IF:
        //    return Parse_IfStmt(lex);
        //case TOK_RWORD_MATCH:
        //    return Parse_Expr_Match(lex);

        default:
            throw ParseError::Unexpected(lex, tok);
        }
    }
    else
    {
        // HACK: Parse a path and look for a `macro::path! { }`, so it can be parsed as a block (instead of as an expression)
        // NOTE: This means that here is where the path parsing code ends up
        switch(tok.type())
        {
        case TOK_IDENT:
        case TOK_RWORD_CRATE:
        case TOK_RWORD_SUPER:
        case TOK_DOUBLE_COLON:
        case TOK_RWORD_SELF:
            if(tok.type() != TOK_RWORD_SELF || lex.lookahead(0) == TOK_DOUBLE_COLON) {
                PUTBACK(tok, lex);
                auto p = Parse_Path(lex, PATH_GENERIC_EXPR);
                if( lex.lookahead(0) == TOK_EXCLAM && lex.lookahead(1) == TOK_BRACE_OPEN ) {
                    GET_CHECK_TOK(tok, lex, TOK_EXCLAM);
                    auto rv = Parse_ExprMacro(lex, std::move(p));
                    // If the block is followed by `.` or `?`, it's actually an expression!
                    if( lex.lookahead(0) == TOK_DOT || lex.lookahead(0) == TOK_QMARK ) {
                        lex.putback( Token(Token::TagTakeIP(), InterpolatedFragment(InterpolatedFragment::EXPR, rv.release())) );
                        return Parse_ExprBlockLine_Stmt(lex, *add_silence);
                    }
                    return rv;
                }
                tok = Token(Token::TagTakeIP(), InterpolatedFragment(std::move(p)));
            }
            break;
        default:
            break;
        }

        switch( tok.type() )
        {
        case TOK_INTERPOLATED_BLOCK:
            return tok.take_frag_node();
        case TOK_SEMICOLON:
            // Return a NULL expression, nothing here.
            return nullptr;

        // let binding
        case TOK_RWORD_LET:
            ret = Parse_Stmt_Let(lex);
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            return ret;

        // Blocks that don't need semicolons
        // NOTE: If these are followed by a small set of tokens (`.` and `?`) then they are actually the start of an expression
        // HACK: Parse here, but if the next token is one of the set store in a TOK_INTERPOLATED_EXPR and invoke the statement parser
        case TOK_RWORD_LOOP:
            ret = NEWNODE( AST::ExprNode_Loop, "", Parse_ExprBlockNode(lex) );
            if(0)
        case TOK_RWORD_WHILE:
            ret = Parse_WhileStmt(lex, Ident(""));
            if(0)
        case TOK_RWORD_FOR:
            ret = Parse_ForStmt(lex, Ident(""));
            if(0)
        case TOK_RWORD_IF:
            ret = Parse_IfStmt(lex);
            if(0)
        case TOK_RWORD_MATCH:
            ret = Parse_Expr_Match(lex);
            if(0)
        case TOK_RWORD_UNSAFE:
            ret = Parse_ExprBlockNode(lex, AST::ExprNode_Block::Type::Unsafe);
            if(0)
        case TOK_RWORD_CONST:
            ret = Parse_ExprBlockNode(lex, AST::ExprNode_Block::Type::Const);
            if(0)
        case TOK_BRACE_OPEN:
            { PUTBACK(tok, lex); ret = Parse_ExprBlockNode(lex); }

            // If the block is followed by `.` or `?`, it's actually an expression!
            if( lex.lookahead(0) == TOK_DOT || lex.lookahead(0) == TOK_QMARK ) {
                lex.putback( Token(Token::TagTakeIP(), InterpolatedFragment(InterpolatedFragment::EXPR, ret.release())) );
                return Parse_ExprBlockLine_Stmt(lex, *add_silence);
            }

            if( LOOK_AHEAD(lex) == TOK_SEMICOLON ) {
                GET_TOK(tok, lex);
                *add_silence = true;
            }

            return ret;

        // Flow control
        case TOK_RWORD_DO:
            // `do yeet`
        case TOK_RWORD_RETURN:
        case TOK_RWORD_YIELD:
        case TOK_RWORD_CONTINUE:
        case TOK_RWORD_BREAK: {
            PUTBACK(tok, lex);
            auto ret = Parse_Stmt(lex);
            if( LOOK_AHEAD(lex) == TOK_EOF ) {
            }
            else if( GET_TOK(tok, lex) != TOK_SEMICOLON ) {
                CHECK_TOK(tok, TOK_BRACE_CLOSE);
                PUTBACK(tok, lex);
            }
            else {
                // return/continue/break don't need silencing
            }
            return ret;
            }
        // TODO: if this expression captures a block, then treat it as a statement.
        // Otherwise, interpret as normal expression
        // HACK: Just treat a leading `:expr` as a statement (rust-lang/rust #78829) (ref: rustc-1.39.0-src\vendor\indexmap\src\map.rs:1139)
        //case TOK_INTERPOLATED_EXPR:
        //    DEBUG(":expr");
        //    if( dynamic_cast<AST::ExprNode_Block .frag_node()
        //    PUTBACK(tok, lex);
        //    return Parse_Stmt(lex);

        default:
            PUTBACK(tok, lex);
            return Parse_ExprBlockLine_Stmt(lex, *add_silence);
        }
    }
}

ExprNodeP Parse_ExprBlockLine_Stmt(TokenStream& lex, bool& has_semicolon)
{
    Token tok;

    bool is_paren = lex.lookahead(0) == TOK_PAREN_OPEN;

    auto ret = Parse_Stmt(lex);

    // If `ret` is a braced macro call, don't require the semicolon (to remove the hackiness above)
    // - Don't trigger this when parens are present
    if( const auto* mac = dynamic_cast<AST::ExprNode_Macro*>(&*ret) ) {
        if( !is_paren && mac->m_is_braced ) {
            return ret;
        }
    }

    // If this expression statement wasn't followed by a semicolon, then it's yielding its value out of the block.
    // - I.e. The block should be ending
    if( !lex.getTokenIf(TOK_SEMICOLON) ) {
        // - Allow TOK_EOF for macro expansion.
        switch(lex.lookahead(0))
        {
        case TOK_EOF:
        case TOK_BRACE_CLOSE:
            break;
        default:
            GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
            break;
        }
    }
    else {
        has_semicolon = true;
    }
    return ret;
}

std::vector<AST::IfLet_Condition> Parse_IfLetChain(TokenStream& lex)
{
    Token   tok;
    std::vector<AST::IfLet_Condition>   conditions;
    bool had_pat = false;
    do {
        if( lex.getTokenIf(TOK_RWORD_LET) ) {
            lex.getTokenIf(TOK_PIPE);
            auto pat = Parse_Pattern(lex, AllowOrPattern::Yes);
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);
            ExprNodeP val;
            {
                SET_PARSE_FLAG(lex, disallow_struct_literal);
                val = Parse_Expr3(lex); // This is just after `||` and `&&`
            }
            conditions.push_back(AST::IfLet_Condition { box$(pat), std::move(val) });
            had_pat = true;
        }
        else {
            ExprNodeP val;
            {
                SET_PARSE_FLAG(lex, disallow_struct_literal);
                val = Parse_Expr3(lex); // This is just after `||` and `&&`
            }

            // Chain boolean expressions to simplify downstream representation
            if( conditions.size() > 0 && !conditions.back().opt_pat ) {
                conditions.back().value = NEWNODE(AST::ExprNode_BinOp, AST::ExprNode_BinOp::BOOLAND,
                    ::std::move( conditions.back().value ),
                    ::std::move( val )
                    );
            }
            else {
                conditions.push_back(AST::IfLet_Condition { std::unique_ptr<AST::Pattern>(), std::move(val) });
            }
        }
    } while( lex.getTokenIf(TOK_DOUBLE_AMP) );

    if( lex.lookahead(0) == TOK_DOUBLE_PIPE ) {
        if( had_pat ) {
            TODO(lex.point_span(), "lazy boolean or in let chains not yet implemented (not yet valid rust, at 1.75)");
        }
        else {
            // Fall back to parsing as a standard expression
            auto prev = ::std::move(conditions[0].value);
            for(size_t i = 1; i < conditions.size(); i ++) {
                prev = NEWNODE(AST::ExprNode_BinOp, AST::ExprNode_BinOp::BOOLAND, ::std::move(prev), ::std::move(conditions[i].value));
            }
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_PIPE);
            SET_PARSE_FLAG(lex, disallow_struct_literal);
            auto n = Parse_Expr1_5(lex);    // Boolean or
            auto rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::BOOLOR, ::std::move(prev), ::std::move(n) );

            conditions.clear();
            conditions.push_back(AST::IfLet_Condition { std::unique_ptr<AST::Pattern>(), std::move(rv) });
        }
    }

    return conditions;
}

/// While loop (either as a statement, or as part of an expression)
ExprNodeP Parse_WhileStmt(TokenStream& lex, Ident lifetime)
{
    if( TARGETVER_LEAST_1_74 || lex.lookahead(0) == TOK_RWORD_LET ) {
        // TODO: Pattern list (same as match)?
        auto conditions = Parse_IfLetChain(lex);
        return NEWNODE( AST::ExprNode_WhileLet, lifetime, ::std::move(conditions), Parse_ExprBlockNode(lex) );
    }
    else {
        ExprNodeP cnd;
        {
            SET_PARSE_FLAG(lex, disallow_struct_literal);
            cnd = Parse_Expr1(lex);
        }
        return NEWNODE( AST::ExprNode_Loop, lifetime, ::std::move(cnd), Parse_ExprBlockNode(lex) );
    }
}
/// For loop (either as a statement, or as part of an expression)
ExprNodeP Parse_ForStmt(TokenStream& lex, Ident lifetime)
{
    CLEAR_PARSE_FLAGS_EXPR(lex);
    Token   tok;

    // Irrefutable pattern
    auto pat = Parse_Pattern(lex, AllowOrPattern::Yes);
    GET_CHECK_TOK(tok, lex, TOK_RWORD_IN);
    ExprNodeP val;
    {
        SET_PARSE_FLAG(lex, disallow_struct_literal);
        val = Parse_Expr0(lex);
    }
    return NEWNODE( AST::ExprNode_Loop, lifetime, ::std::move(pat), ::std::move(val), Parse_ExprBlockNode(lex) );
}
/// Parse an 'if' statement
// Note: TOK_RWORD_IF has already been eaten
ExprNodeP Parse_IfStmt(TokenStream& lex)
{
    TRACE_FUNCTION;

    Token   tok;
    ExprNodeP cond;
    std::vector<AST::IfLet_Condition>   conditions;

    {
        SET_PARSE_FLAG(lex, disallow_struct_literal);
        if( TARGETVER_LEAST_1_74 || lex.lookahead(0) == TOK_RWORD_LET ) {
            conditions = Parse_IfLetChain(lex);
            // If the conditions are just booleans, emit as `if`
            if( conditions.size() == 1 && !conditions[0].opt_pat ) {
                cond = std::move(conditions[0].value);
                conditions.clear();
            }
        }
        else {
            // TODO: `if foo && let bar` is valid
            cond = Parse_Expr0(lex);
        }
    }

    // Contents
    ExprNodeP code = Parse_ExprBlockNode(lex);

    // Handle else:
    ExprNodeP altcode;
    if( lex.getTokenIf(TOK_RWORD_ELSE) )
    {
        // Recurse for 'else if'
        if( lex.getTokenIf(TOK_RWORD_IF) ) {
            altcode = Parse_IfStmt(lex);
        }
        // - or get block
        else {
            altcode = Parse_ExprBlockNode(lex);
        }
    }
    // - or nothing
    else {
    }

    if( !cond )
        return NEWNODE( AST::ExprNode_IfLet, ::std::move(conditions), ::std::move(code), ::std::move(altcode) );
    else
        return NEWNODE( AST::ExprNode_If, ::std::move(cond), ::std::move(code), ::std::move(altcode) );
}
/// "match" block
ExprNodeP Parse_Expr_Match(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token tok;

    CLEAR_PARSE_FLAGS_EXPR(lex);
    // 1. Get expression
    ExprNodeP   switch_val;
    {
        SET_PARSE_FLAG(lex, disallow_struct_literal);
        switch_val = Parse_Expr1(lex);
    }
    //ASSERT(lex, !CHECK_PARSE_FLAG(lex, disallow_struct_literal) );
    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);

    ::std::vector< AST::ExprNode_Match_Arm >    arms;
    do {
        if( lex.getTokenIf(TOK_BRACE_CLOSE, tok) ) {
            break;
        }
        AST::ExprNode_Match_Arm    arm;

        arm.m_attrs = Parse_ItemAttrs(lex);

        // HACK: Questionably valid, but 1.29 librustc/hir/lowering.rs needs this
        lex.getTokenIf(TOK_PIPE);

        do {
            // Refutable pattern
            arm.m_patterns.push_back( Parse_Pattern(lex, AllowOrPattern::No) );
        } while( GET_TOK(tok, lex) == TOK_PIPE );

        if( tok.type() == TOK_RWORD_IF )
        {
            arm.m_guard = Parse_IfLetChain(lex);
            GET_TOK(tok, lex);
        }
        CHECK_TOK(tok, TOK_FATARROW);

        arm.m_code = Parse_Stmt(lex);

        arms.push_back( ::std::move(arm) );

        // Match arms don't need a trailing comma (TODO: Only if braced)
        lex.getTokenIf(TOK_COMMA);
    } while( 1 );
    CHECK_TOK(tok, TOK_BRACE_CLOSE);

    return NEWNODE( AST::ExprNode_Match, ::std::move(switch_val), ::std::move(arms) );
}

/// "do catch" block
ExprNodeP Parse_Expr_Try(TokenStream& lex)
{
    TRACE_FUNCTION;
    //Token   tok;

    auto inner = Parse_ExprBlockNode(lex);
    //TODO(lex.point_span(), "do catch");
    return NEWNODE(AST::ExprNode_Try, ::std::move(inner));
}

ExprNodeP Parse_FlowControl(TokenStream& lex, AST::ExprNode_Flow::Type type)
{
    Token   tok;
    Ident lifetime = Ident("");
    // continue/break can specify a target
    if(type == AST::ExprNode_Flow::CONTINUE || type == AST::ExprNode_Flow::BREAK )
    {
        if( lex.lookahead(0) == TOK_LIFETIME )
        {
            GET_TOK(tok, lex);
            lifetime = tok.ident();
        }
    }
    // Return value
    // TODO: Should this prevent `continue value;`?
    ExprNodeP   val;
    switch(LOOK_AHEAD(lex))
    {
    case TOK_EOF:
    case TOK_SEMICOLON:
    case TOK_COMMA:
    case TOK_BRACE_CLOSE:
    case TOK_PAREN_CLOSE:
    case TOK_SQUARE_CLOSE:
        break;
    default:
        val = Parse_Expr0(lex);
        break;
    }
    return NEWNODE( AST::ExprNode_Flow, type, std::move(lifetime), ::std::move(val) );
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
    case TOK_INTERPOLATED_STMT:
        return tok.take_frag_node();
    // Duplicated here for the :stmt pattern fragment.
    case TOK_RWORD_LET:
        return Parse_Stmt_Let(lex);
    case TOK_RWORD_YIELD:
        return Parse_FlowControl(lex, AST::ExprNode_Flow::YIELD);
    case TOK_RWORD_CONTINUE:
        return Parse_FlowControl(lex, AST::ExprNode_Flow::CONTINUE);
    case TOK_RWORD_BREAK:
        return Parse_FlowControl(lex, AST::ExprNode_Flow::BREAK);
    case TOK_RWORD_RETURN:
        return Parse_FlowControl(lex, AST::ExprNode_Flow::RETURN);
    case TOK_BRACE_OPEN:
        PUTBACK(tok, lex);
        return Parse_ExprBlockNode(lex);
    case TOK_RWORD_IF:
    case TOK_RWORD_WHILE:
    case TOK_RWORD_FOR:
    case TOK_RWORD_LOOP:
    case TOK_RWORD_MATCH: {
        PUTBACK(tok, lex);
        SET_PARSE_FLAG(lex, disallow_call_or_index);
        return Parse_ExprFC(lex);
        }
    //case TOK_RWORD_DO:
    //    if( lex.lookahead(0) == "yeet" ) {
    //        return Parse_FlowControl(lex, AST::ExprNode_Flow::YEET);
    //    }
    default:
        PUTBACK(tok, lex);
        return Parse_Expr0(lex);
    }
}

ExprNodeP Parse_Stmt_Let(TokenStream& lex)
{
    Token   tok;
    AST::Pattern pat = Parse_Pattern(lex, AllowOrPattern::Yes);   // irrefutable
    TypeRef type { lex.point_span() };
    if( lex.getTokenIf(TOK_COLON) ) {
        type = Parse_Type(lex);
    }
    ExprNodeP val;
    ExprNodeP else_arm;
    if( lex.getTokenIf(TOK_EQUAL) ) {
        val = Parse_Expr0(lex);
        if( lex.getTokenIf(TOK_RWORD_ELSE) ) {
            else_arm = Parse_ExprBlockNode(lex);
        }
    }
    return NEWNODE( AST::ExprNode_LetBinding, ::std::move(pat), mv$(type), ::std::move(val), ::std::move(else_arm) );
}

::std::vector<ExprNodeP> Parse_ParenList(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;

    CLEAR_PARSE_FLAGS_EXPR(lex);

    ::std::vector<ExprNodeP> rv;
    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
    if( !lex.getTokenIf(TOK_PAREN_CLOSE) )
    {
        do {
            if( lex.getTokenIf(TOK_PAREN_CLOSE, tok) ) {
                break;
            }
            rv.push_back( Parse_Expr0(lex) );
        } while( GET_TOK(tok, lex) == TOK_COMMA );
        CHECK_TOK(tok, TOK_PAREN_CLOSE);
    }
    return rv;
}

// 0: Assign
ExprNodeP Parse_Expr0(TokenStream& lex)
{
    //TRACE_FUNCTION;
    Token tok;

    CLEAR_PARSE_FLAG(lex, disallow_call_or_index);

    auto expr_attrs = Parse_ItemAttrs(lex);

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
    case TOK_PERCENT_EQUAL:
        op = AST::ExprNode_Assign::MOD; if(0)

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
        rv = NEWNODE( AST::ExprNode_Assign, op, ::std::move(rv), Parse_Expr0(lex) );
        rv->set_attrs(mv$(expr_attrs));
        return rv;

    default:
        PUTBACK(tok, lex);
        rv->set_attrs(mv$(expr_attrs));
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
            PUTBACK(tok, lex); \
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
    case TOK_CSTRING:
    case TOK_BYTESTRING:
    case TOK_UNDERSCORE:
    case TOK_RWORD_TRUE:
    case TOK_RWORD_FALSE:
    case TOK_RWORD_SELF:
    case TOK_RWORD_SUPER:
    case TOK_RWORD_CRATE:
    case TOK_RWORD_BOX:
    case TOK_RWORD_IN:
    case TOK_PAREN_OPEN:
    case TOK_SQUARE_OPEN:

    case TOK_INTERPOLATED_PATH:
    case TOK_INTERPOLATED_EXPR:

    case TOK_PIPE:
    case TOK_EXCLAM:
    case TOK_DASH:
    case TOK_STAR:
    case TOK_AMP:
        return true;
    default:
        return false;
    }
}
ExprNodeP Parse_Expr1_1(TokenStream& lex);
ExprNodeP Parse_Expr1(TokenStream& lex)
{
    Token   tok;
    ExprNodeP (*next)(TokenStream&) = Parse_Expr1_1;

    auto dest = next(lex);
    if( lex.lookahead(0) == TOK_THINARROW_LEFT )
    {
        GET_TOK(tok, lex);
        auto val = Parse_Expr1(lex);
        return NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::PLACE_IN, mv$(dest), mv$(val));
    }
    else
    {
        return dest;
    }
}
ExprNodeP Parse_Expr1_2(TokenStream& lex);
// Very evil handling for '..'
ExprNodeP Parse_Expr1_1(TokenStream& lex)
{
    Token   tok;
    ExprNodeP (*next)(TokenStream&) = Parse_Expr1_2;
    ExprNodeP   left, right;

    // Inclusive range to a value
    if( GET_TOK(tok, lex) == TOK_TRIPLE_DOT || (TARGETVER_LEAST_1_29 && tok.type() == TOK_DOUBLE_DOT_EQUAL) ) {
        right = next(lex);
        return NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::RANGE_INC, nullptr, mv$(right) );
    }
    else {
        PUTBACK(tok, lex);
    }

    // Exclusive ranges
    // - If NOT `.. <VAL>`, parse a leading value
    if( !lex.getTokenIf(TOK_DOUBLE_DOT, tok) )
    {
        left = next(lex);

        // - If NOT `<VAL> ..`, return the value
        if( !lex.getTokenIf(TOK_DOUBLE_DOT, tok) )
        {
            return ::std::move(left);
        }
    }
    assert( tok.type() == TOK_DOUBLE_DOT );
    // If the next token is part of a value, parse that value
    if( Parse_IsTokValue( LOOK_AHEAD(lex) ) )
    {
        right = next(lex);
    }
    else
    {
        // Otherwise, leave `right` as nullptr
    }

    return NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::RANGE, ::std::move(left), ::std::move(right) );
}
LEFTASSOC(Parse_Expr1_2, Parse_Expr1_5,
    case TOK_TRIPLE_DOT:
        rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::RANGE_INC, mv$(rv), next(lex) );
        break;
    case TOK_DOUBLE_DOT_EQUAL:
        if( TARGETVER_LEAST_1_29 )
        {
            rv = NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::RANGE_INC, mv$(rv), next(lex) );
            break;
        }
        // Fall through
)
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
// 10: Times / Divide / Modulo
LEFTASSOC(Parse_Expr10, Parse_Expr11,
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
// 11: Cast
LEFTASSOC(Parse_Expr11, Parse_Expr12,
    case TOK_RWORD_AS:
        rv = NEWNODE( AST::ExprNode_Cast, ::std::move(rv), Parse_Type(lex, false) );
        break;
)
// 12: Type Ascription
ExprNodeP Parse_Expr13(TokenStream& lex);
ExprNodeP Parse_Expr12(TokenStream& lex)
{
    Token   tok;
    auto rv = Parse_Expr13(lex);
    if( lex.getTokenIf(TOK_COLON) )
    {
        rv = NEWNODE( AST::ExprNode_TypeAnnotation, mv$(rv), Parse_Type(lex) );
    }
    return rv;
}
// 13: Unaries
ExprNodeP Parse_Expr13(TokenStream& lex)
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
    case TOK_RWORD_IN: {
        ExprNodeP   dest;
        {
            SET_PARSE_FLAG(lex, disallow_struct_literal);
            dest = Parse_Expr1(lex);
        }
        auto val = Parse_ExprBlockNode(lex);
        return NEWNODE( AST::ExprNode_BinOp, AST::ExprNode_BinOp::PLACE_IN, mv$(dest), mv$(val));
        }
    case TOK_DOUBLE_AMP:
        // HACK: Split && into & &
        lex.putback( Token(TOK_AMP) );
    case TOK_AMP:
        if( lex.lookahead(0) == TOK_IDENT )
        {
            GET_TOK(tok, lex);
            if(tok.ident() == "raw") {
                if( lex.lookahead(0) == TOK_RWORD_MUT ) {
                    GET_TOK(tok, lex);
                    return NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::RawBorrowMut, Parse_Expr12(lex) );
                }
                else if( lex.lookahead(0) == TOK_RWORD_CONST ) {
                    GET_TOK(tok, lex);
                    return NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::RawBorrow, Parse_Expr12(lex) );
                }
                else {
                }
            }
            PUTBACK(tok, lex);
        }
        if( lex.getTokenIf(TOK_RWORD_MUT) ) {
            return NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::REFMUT, Parse_Expr12(lex) );
        }
        else {
            return NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::REF, Parse_Expr12(lex) );
        }
    default:
        PUTBACK(tok, lex);
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
        // Expression method call
        case TOK_PAREN_OPEN:
            if( CHECK_PARSE_FLAG(lex, disallow_call_or_index) ) {
                PUTBACK(tok, lex);
                return val;
            }
            PUTBACK(tok, lex);
            val = NEWNODE( AST::ExprNode_CallObject, ::std::move(val), Parse_ParenList(lex) );
            break;
        case TOK_SQUARE_OPEN:
            if( CHECK_PARSE_FLAG(lex, disallow_call_or_index) ) {
                PUTBACK(tok, lex);
                return val;
            }
            val = NEWNODE( AST::ExprNode_Index, ::std::move(val), Parse_Expr0(lex) );
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            break;

        case TOK_QMARK:
            val = NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::QMARK, mv$(val) );
            break;

        case TOK_DOT:
            // Field access / method call / tuple index
            switch(GET_TOK(tok, lex))
            {
            case TOK_IDENT: {
                AST::PathNode   pn( tok.ident().name , {});
                switch( GET_TOK(tok, lex) )
                {
                case TOK_PAREN_OPEN:
                    PUTBACK(tok, lex);
                    val = NEWNODE( AST::ExprNode_CallMethod, ::std::move(val), ::std::move(pn), Parse_ParenList(lex) );
                    break;
                case TOK_DOUBLE_COLON:
                    if( lex.getTokenIf(TOK_DOUBLE_LT) ) {
                        lex.putback(Token(TOK_LT));
                    }
                    else {
                        GET_CHECK_TOK(tok, lex, TOK_LT);
                    }
                    pn.args() = Parse_Path_GenericList(lex);
                    val = NEWNODE( AST::ExprNode_CallMethod, ::std::move(val), ::std::move(pn), Parse_ParenList(lex) );
                    break;
                default:
                    val = NEWNODE( AST::ExprNode_Field, ::std::move(val), pn.name() );
                    PUTBACK(tok, lex);
                    break;
                }
                break; }
            case TOK_INTEGER:
                val = NEWNODE( AST::ExprNode_Field, ::std::move(val), RcString::new_interned(FMT(tok.intval())) );
                break;
            case TOK_RWORD_AWAIT:
                val = NEWNODE( AST::ExprNode_UniOp, AST::ExprNode_UniOp::AWait, ::std::move(val) );
                break;
            default:
                throw ParseError::Unexpected(lex, mv$(tok));
            }
            break;
        default:
            PUTBACK(tok, lex);
            return val;
        }
    }
}

ExprNodeP Parse_ExprVal_StructLiteral(TokenStream& lex, AST::Path path)
{
    TRACE_FUNCTION;
    Token   tok;

    // #![feature(relaxed_adts)]
    if( LOOK_AHEAD(lex) == TOK_INTEGER )
    {
        ::std::map<unsigned int, ExprNodeP> nodes;
        while( GET_TOK(tok, lex) == TOK_INTEGER )
        {
            unsigned int ofs = static_cast<unsigned int>(tok.intval().truncate_u64());
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            ExprNodeP   val = Parse_Stmt(lex);
            if( ! nodes.insert( ::std::make_pair(ofs, mv$(val)) ).second ) {
                ERROR(lex.point_span(), E0000, "Duplicate index");
            }

            if( GET_TOK(tok,lex) == TOK_BRACE_CLOSE )
                break;
            CHECK_TOK(tok, TOK_COMMA);
        }
        CHECK_TOK(tok, TOK_BRACE_CLOSE);

        ::std::vector<ExprNodeP>    items;
        unsigned int i = 0;
        for(auto& p : nodes)
        {
            if( p.first != i ) {
                ERROR(lex.point_span(), E0000, "Missing index " << i);
            }
            items.push_back( mv$(p.second) );
            i ++;
        }

        return NEWNODE( AST::ExprNode_CallPath, mv$(path), mv$(items) );
    }

    // Braced structure literal
    // - A series of 0 or more pairs of <ident>: <expr>,
    // - '..' <expr>
    ::AST::ExprNode_StructLiteral::t_values items;
    while( GET_TOK(tok, lex) == TOK_IDENT || tok.type() == TOK_HASH )
    {
        ::AST::AttributeList attrs;    // Note: Parse_ItemAttrs uses lookahead, so can't use it here.
        if( tok.type() == TOK_HASH )
        {
            PUTBACK(tok, lex);
            attrs = Parse_ItemAttrs(lex);
            GET_TOK(tok, lex);
        }
        CHECK_TOK(tok, TOK_IDENT);
        auto h = tok.ident().hygiene;
        auto name = tok.ident().name;

        ExprNodeP   val;
        if( lex.lookahead(0) != TOK_COLON )
        {
            val = NEWNODE( AST::ExprNode_NamedValue, ::AST::Path::new_relative( h, { ::AST::PathNode(name) }) );
        }
        else
        {
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            val = Parse_Expr0(lex);
        }
        items.push_back(::AST::ExprNode_StructLiteral::Ent { mv$(attrs), mv$(name), mv$(val) });

        if( GET_TOK(tok,lex) == TOK_BRACE_CLOSE )
            break;
        CHECK_TOK(tok, TOK_COMMA);
    }
    ExprNodeP    base_val;
    if( tok.type() == TOK_DOUBLE_DOT )
    {
        if( lex.getTokenIf(TOK_BRACE_CLOSE) ) {
            return NEWNODE( AST::ExprNode_StructLiteralPattern, path, ::std::move(items) );
        }
        else {
            // default
            base_val = Parse_Expr0(lex);
        }
        GET_TOK(tok, lex);
    }
    CHECK_TOK(tok, TOK_BRACE_CLOSE);

    return NEWNODE( AST::ExprNode_StructLiteral, path, ::std::move(base_val), ::std::move(items) );
}

ExprNodeP Parse_ExprVal_Closure(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;

    GET_TOK(tok, lex);

    // [`static`]
    bool is_immovable = false;
    if(tok == TOK_RWORD_STATIC) {
        GET_TOK(tok, lex);
        is_immovable = true;
    }

    // [`move`]
    bool is_move = false;
    if(tok == TOK_RWORD_MOVE) {
        GET_TOK(tok, lex);
        is_move = true;
    }


    ::std::vector< ::std::pair<AST::Pattern, TypeRef> > args;
    if( tok == TOK_DOUBLE_PIPE )
    {
        // `||` - Empty argument list
    }
    else if( tok == TOK_PIPE )
    {
        // `|...|` - Arguments present
        while( GET_TOK(tok, lex) != TOK_PIPE )
        {
            PUTBACK(tok, lex);
            // Irrefutable pattern
            AST::Pattern    pat = Parse_Pattern(lex, AllowOrPattern::No);

            TypeRef type { lex.point_span() };
            if( lex.getTokenIf(TOK_COLON) ) {
                type = Parse_Type(lex);
            }

            args.push_back( ::std::make_pair( ::std::move(pat), ::std::move(type) ) );

            if( GET_TOK(tok, lex) != TOK_COMMA )
                break;
        }
        CHECK_TOK(tok, TOK_PIPE);
    }
    else
    {
        throw ParseError::Unexpected(lex, tok, {TOK_PIPE, TOK_DOUBLE_PIPE, TOK_RWORD_MOVE, TOK_RWORD_STATIC});
    }

    auto rt = TypeRef(lex.point_span());
    if( lex.getTokenIf(TOK_THINARROW) ) {

        auto bang_sp = lex.point_span();
        if( lex.getTokenIf(TOK_EXCLAM) ) {
            rt = TypeRef(TypeRef::TagInvalid(), bang_sp);
        }
        else {
            rt = Parse_Type(lex);
        }
    }

    auto code = Parse_Expr0(lex);

    return NEWNODE( AST::ExprNode_Closure, ::std::move(args), ::std::move(rt), ::std::move(code), is_move, is_immovable );
}

ExprNodeP Parse_ExprVal_Inner(TokenStream& lex)
{
    TRACE_FUNCTION;

    Token   tok;
    AST::Path   path;
    switch( GET_TOK(tok, lex) )
    {
    case TOK_BRACE_OPEN:
        PUTBACK(tok, lex);
        return Parse_ExprBlockNode(lex);

    case TOK_INTERPOLATED_EXPR:
    case TOK_INTERPOLATED_BLOCK:
        return tok.take_frag_node();


    // Return/break/continue/... also parsed here (but recurses back up to actually handle them)
    case TOK_RWORD_RETURN:
    case TOK_RWORD_YIELD:
    case TOK_RWORD_CONTINUE:
    case TOK_RWORD_BREAK:
        PUTBACK(tok, lex);
        return Parse_Stmt(lex);


    case TOK_LIFETIME:
        PUTBACK(tok, lex);
        return Parse_ExprBlockLine(lex, nullptr);

    case TOK_RWORD_LOOP:
        return NEWNODE( AST::ExprNode_Loop, "", Parse_ExprBlockNode(lex) );
    case TOK_RWORD_WHILE:
        return Parse_WhileStmt(lex, Ident(""));
    case TOK_RWORD_FOR:
        return Parse_ForStmt(lex, Ident(""));
    case TOK_RWORD_TRY: // Only emitted in 2018
        return Parse_Expr_Try(lex);
    case TOK_RWORD_DO:
        GET_TOK(tok, lex);
        // `do catch` (1.29) - stabilised later as `try`
        if( tok.type() == TOK_IDENT && tok.ident().name == "catch" )
        {
            return Parse_Expr_Try(lex);
        }
        // `do yeet` (1.74) - Not stabilised (as of 2024-04)
        else if( tok.type() == TOK_IDENT && tok.ident().name == "yeet" )
        {
            return Parse_FlowControl(lex, AST::ExprNode_Flow::YEET);
        }
        else
        {
            throw ParseError::Unexpected(lex, tok);
        }
    case TOK_RWORD_MATCH:
        return Parse_Expr_Match(lex);
    case TOK_RWORD_IF:
        return Parse_IfStmt(lex);
    case TOK_RWORD_ASYNC: {
        bool is_move = lex.getTokenIf(TOK_RWORD_MOVE);
        return NEWNODE(AST::ExprNode_AsyncBlock, Parse_ExprBlockNode(lex, AST::ExprNode_Block::Type::Bare), is_move);
    }
    case TOK_RWORD_UNSAFE:
        return Parse_ExprBlockNode(lex, AST::ExprNode_Block::Type::Unsafe);
    case TOK_RWORD_CONST:
        return Parse_ExprBlockNode(lex, AST::ExprNode_Block::Type::Const);

    // Paths
    // `self` can be a value, or start a path
    case TOK_RWORD_SELF:
        if( LOOK_AHEAD(lex) != TOK_DOUBLE_COLON ) {
            static const RcString rcstring_self = RcString::new_interned("self");
            return NEWNODE( AST::ExprNode_NamedValue, AST::Path(rcstring_self) );
        }
        // Fall through to normal paths
    case TOK_DOUBLE_LT:
    case TOK_LT:
    case TOK_RWORD_CRATE:
    case TOK_RWORD_SUPER:
    case TOK_DOUBLE_COLON:
    case TOK_IDENT:
    case TOK_INTERPOLATED_PATH:
        PUTBACK(tok, lex);
        path = Parse_Path(lex, PATH_GENERIC_EXPR);

        DEBUG("path = " << path << ", lookahead=" << Token::typestr(lex.lookahead(0)));
        switch( GET_TOK(tok, lex) )
        {
        case TOK_EXCLAM:
            return Parse_ExprMacro(lex, mv$(path));
        case TOK_PAREN_OPEN:
            // Function call
            PUTBACK(tok, lex);
            return NEWNODE( AST::ExprNode_CallPath, ::std::move(path), Parse_ParenList(lex) );
        case TOK_BRACE_OPEN:
            if( !CHECK_PARSE_FLAG(lex, disallow_struct_literal) )
                return Parse_ExprVal_StructLiteral(lex, ::std::move(path));
            else
                DEBUG("Not parsing struct literal");
            // Value
            PUTBACK(tok, lex);
            return NEWNODE( AST::ExprNode_NamedValue, ::std::move(path) );
        // `builtin # <name>` - seems to be a 1.74 era hack to extend syntax
        // - Only `builtin # offset_of( <ty>, <field>[, <subfield>]... )` is implemented
        //   > mrustc translates this to an intrinsic call with the fields as string/integer arguments (simple to pass through)
        case TOK_HASH:
            if( path.is_trivial() && path.as_trivial() == "builtin" ) {
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                if( tok.ident() == "offset_of" ) {
                    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
                    auto ty = Parse_Type(lex);
                    std::vector<AST::ExprNodeP> args;
                    do {
                        GET_CHECK_TOK(tok, lex, TOK_COMMA);
                        if( lex.lookahead(0) == TOK_INTERPOLATED_EXPR ) {
                            GET_CHECK_TOK(tok, lex, TOK_INTERPOLATED_EXPR);
                            const auto* expr = &tok.frag_node();
                            std::vector<AST::ExprNodeP>  expr_args;
                            for(;;) {
                                if( const auto* n = dynamic_cast<const AST::ExprNode_NamedValue*>(expr) ) {
                                    expr_args.push_back( NEWNODE(AST::ExprNode_String, n->m_path.as_trivial().c_str(), {}) );
                                    break;
                                }
                                else if( const auto* n = dynamic_cast<const AST::ExprNode_Field*>(expr) ) {
                                    expr_args.push_back( NEWNODE(AST::ExprNode_String, n->m_name.c_str(), {}) );
                                    expr = &*n->m_obj;
                                }
                                else {
                                    TODO(lex.point_span(), "offset_of - " << *expr);
                                }
                            }
                            while(!expr_args.empty()) {
                                args.push_back( std::move(expr_args.back()) );
                                expr_args.pop_back();
                            }
                        }
                        else if( lex.lookahead(0) == TOK_INTEGER ) {
                            GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                            args.push_back( NEWNODE( AST::ExprNode_Integer, tok.intval(), tok.datatype() ) );
                        }
                        else {
                            GET_CHECK_TOK(tok, lex, TOK_IDENT);
                            args.push_back( NEWNODE( AST::ExprNode_String, tok.ident().name.c_str(), tok.ident().hygiene ) );
                        }
                    } while( lex.lookahead(0) == TOK_COMMA );
                    GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);

                    // TODO: How to emit this, maybe as a hacky intrinsic?
                    // ::"#intrinsics"::offset_of::<T>("field1",...)
                    // - Fiddly
                    path = AST::Path(RcString::new_interned("#intrinsics"), {AST::PathNode("offset_of")});
                    path.nodes().back().args().m_entries.push_back( std::move(ty) );
                    return NEWNODE(AST::ExprNode_CallPath, std::move(path), std::move(args));
                }
                else {
                    TODO(lex.point_span(), "`builtin #` support - " << tok.ident());
                }
            }
        default:
            // Value
            PUTBACK(tok, lex);
            return NEWNODE( AST::ExprNode_NamedValue, ::std::move(path) );
        }
    // Closures
    case TOK_RWORD_STATIC:
    case TOK_RWORD_MOVE:
    case TOK_PIPE:
    case TOK_DOUBLE_PIPE:
        PUTBACK(tok, lex);
        return Parse_ExprVal_Closure(lex);

    case TOK_UNDERSCORE:
        return NEWNODE( AST::ExprNode_WildcardPattern );
    case TOK_INTEGER:
        return NEWNODE( AST::ExprNode_Integer, tok.intval(), tok.datatype() );
    case TOK_FLOAT:
        return NEWNODE( AST::ExprNode_Float, tok.floatval(), tok.datatype() );
    case TOK_STRING:
        return NEWNODE( AST::ExprNode_String, tok.str(), tok.str_hygiene() );
    case TOK_BYTESTRING:
        return NEWNODE( AST::ExprNode_ByteString, tok.str() );
    case TOK_CSTRING:
        return NEWNODE( AST::ExprNode_CString, tok.str() );
    case TOK_RWORD_TRUE:
        return NEWNODE( AST::ExprNode_Bool, true );
    case TOK_RWORD_FALSE:
        return NEWNODE( AST::ExprNode_Bool, false );
    case TOK_PAREN_OPEN:
        if( lex.getTokenIf(TOK_PAREN_CLOSE) )
        {
            DEBUG("Unit");
            return NEWNODE( AST::ExprNode_Tuple, ::std::vector<ExprNodeP>() );
        }
        else
        {
            CLEAR_PARSE_FLAGS_EXPR(lex);

            ExprNodeP rv = Parse_Expr0(lex);
            if( GET_TOK(tok, lex) == TOK_COMMA ) {
                ::std::vector<ExprNodeP> ents;
                ents.push_back( ::std::move(rv) );
                do {
                    if( lex.getTokenIf(TOK_PAREN_CLOSE, tok) ) {
                        break;
                    }
                    ents.push_back( Parse_Expr0(lex) );
                } while( GET_TOK(tok, lex) == TOK_COMMA );
                rv = NEWNODE( AST::ExprNode_Tuple, ::std::move(ents) );
            }
            CHECK_TOK(tok, TOK_PAREN_CLOSE);
            return rv;
        }
    case TOK_SQUARE_OPEN:
        if( lex.getTokenIf(TOK_SQUARE_CLOSE) )
        {
            // Empty literal
            return NEWNODE( AST::ExprNode_Array, ::std::vector<ExprNodeP>() );
        }
        else
        {
            CLEAR_PARSE_FLAGS_EXPR(lex);
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
                    if( GET_TOK(tok, lex) == TOK_SQUARE_CLOSE )
                        break;
                    else
                        PUTBACK(tok, lex);
                    items.push_back( Parse_Expr0(lex) );
                    GET_TOK(tok, lex);
                }
                CHECK_TOK(tok, TOK_SQUARE_CLOSE);
                return NEWNODE( AST::ExprNode_Array, ::std::move(items) );
            }
        }
        throw ParseError::BugCheck(lex, "Array literal fell");
    default:
        throw ParseError::Unexpected(lex, tok);
    }
}
ExprNodeP Parse_ExprVal(TokenStream& lex)
{
    auto attrs = Parse_ItemAttrs(lex);
    auto rv = Parse_ExprVal_Inner(lex);
    rv->set_attrs( std::move(attrs) );
    return rv;
}
ExprNodeP Parse_ExprMacro(TokenStream& lex, AST::Path path)
{
    Token   tok;

    RcString ident;
    if( lex.getTokenIf(TOK_IDENT, tok) ) {
        ident = tok.ident().name;
    }

    bool is_macro = (path.is_trivial() && path.as_trivial() == "macro_rules");

    bool is_braced = lex.lookahead(0) == TOK_BRACE_OPEN;

    if(is_macro)
        lex.push_hygine();
    TokenTree tt = Parse_TT(lex, true);
    if( tt.is_token() ) {
        throw ParseError::Unexpected(lex, tt.tok());
    }
    if(is_macro)
        lex.pop_hygine();

    DEBUG("name=" << path << ", ident=" << ident << ", tt=" << tt);
    return NEWNODE(AST::ExprNode_Macro, mv$(path), mv$(ident), mv$(tt), is_braced);
}

// Token Tree Parsing
TokenTree Parse_TT(TokenStream& lex, bool unwrapped)
{
    TokenTree   rv;
    TRACE_FUNCTION_FR("", rv);

    auto edition = lex.get_edition();
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
    case TOK_PAREN_CLOSE:
    case TOK_SQUARE_CLOSE:
    case TOK_BRACE_CLOSE:
        throw ParseError::Unexpected(lex, tok);
    default:
        rv = TokenTree(edition, lex.get_hygiene(), mv$(tok) );
        DEBUG(rv);
        return rv;
    }

    ::std::vector<TokenTree>   items;
    if( !unwrapped )
        items.push_back( TokenTree(edition, lex.get_hygiene(), mv$(tok)) );
    while(GET_TOK(tok, lex) != closer && tok.type() != TOK_EOF)
    {
        if( tok.type() == TOK_NULL )
            throw ParseError::Unexpected(lex, tok);
        PUTBACK(tok, lex);
        items.push_back(Parse_TT(lex, false));
    }
    if( !unwrapped )
        items.push_back( TokenTree(lex.get_edition(), lex.get_hygiene(), mv$(tok)) );
    rv = TokenTree(edition, lex.get_hygiene(), mv$(items));
    DEBUG(rv);
    return rv;
}
