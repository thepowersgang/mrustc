#include <common.hpp>
#include "../parse/common.hpp"
#include "../parse/parseerror.hpp"
#include "../macros.hpp"

MacroRules Parse_MacroRules(TokenStream& lex);

::std::vector<MacroPatEnt> Parse_MacroRules_Pat(TokenStream& lex, bool allow_sub, enum eTokenType open, enum eTokenType close)
{
    TRACE_FUNCTION;
    Token tok;
    
    ::std::vector<MacroPatEnt>  ret;
    
     int    depth = 0;
    while( GET_TOK(tok, lex) != close || depth > 0 )
    {
        if( tok.type() == open )
        {
            depth ++;
        }
        else if( tok.type() == close )
        {
            if(depth == 0)
                throw ParseError::Generic(FMT("Unmatched " << Token(close) << " in macro pattern"));
            depth --;
        }
        
        switch(tok.type())
        {
        case TOK_DOLLAR:
            switch( GET_TOK(tok, lex) )
            {
            case TOK_IDENT: {
                ::std::string   name = tok.str();
                GET_CHECK_TOK(tok, lex, TOK_COLON);
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                ::std::string   type = tok.str();
                if(0)
                    ;
                else if( type == "tt" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_TT) );
                else if( type == "pat" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_PAT) );
                else if( type == "ident" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_IDENT) );
                else if( type == "path" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_PATH) );
                else if( type == "expr" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_EXPR) );
                else if( type == "ty" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_TYPE) );
                else if( type == "meta" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_META) );
                else if( type == "block" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_BLOCK) );
                else
                    throw ParseError::Generic(lex, FMT("Unknown fragment type '" << type << "'"));
                break; }
            case TOK_PAREN_OPEN:
                if( allow_sub )
                {
                    auto subpat = Parse_MacroRules_Pat(lex, true, TOK_PAREN_OPEN, TOK_PAREN_CLOSE);
                    enum eTokenType joiner = TOK_NULL;
                    GET_TOK(tok, lex);
                    if( tok.type() != TOK_PLUS && tok.type() != TOK_STAR )
                    {
                        DEBUG("Joiner = " << tok);
                        joiner = tok.type();
                        GET_TOK(tok, lex);
                    }
                    DEBUG("tok = " << tok);
                    switch(tok.type())
                    {
                    case TOK_PLUS:
                        DEBUG("$()+ " << subpat);
                        ret.push_back( MacroPatEnt(Token(joiner), true, ::std::move(subpat)) );
                        break;
                    case TOK_STAR:
                        DEBUG("$()* " << subpat);
                        ret.push_back( MacroPatEnt(Token(joiner), false, ::std::move(subpat)) );
                        break;
                    default:
                        throw ParseError::Unexpected(lex, tok);
                    }
                }
                else
                {
                    throw ParseError::Generic(lex, FMT("Nested repetitions in macro"));
                }
                break;
            default:
                throw ParseError::Unexpected(lex, tok);
            }
            break;
        case TOK_EOF:
            throw ParseError::Unexpected(lex, tok);
        default:
            ret.push_back( MacroPatEnt(tok) );
            break;
        }
    }
    
    return ret;
}

::std::vector<MacroRuleEnt> Parse_MacroRules_Cont(TokenStream& lex, bool allow_sub, enum eTokenType open, enum eTokenType close)
{
    TRACE_FUNCTION;
    
    Token tok;
    ::std::vector<MacroRuleEnt> ret;
    
     int    depth = 0;
    while( GET_TOK(tok, lex) != close || depth > 0 )
    {
        if( tok.type() == TOK_EOF ) {
            throw ParseError::Unexpected(lex, tok);
        }
        if( tok.type() == TOK_NULL )    continue ;
        
        if( tok.type() == open )
        {
            DEBUG("depth++");
            depth ++;
        }
        else if( tok.type() == close )
        {
            DEBUG("depth--");
            if(depth == 0)
                throw ParseError::Generic(FMT("Unmatched " << Token(close) << " in macro content"));
            depth --;
        }
        
        if( tok.type() == TOK_DOLLAR )
        {
            GET_TOK(tok, lex);
            
            if( tok.type() == TOK_PAREN_OPEN )
            {   
                if( !allow_sub )
                    throw ParseError::Unexpected(lex, tok);
                
                auto content = Parse_MacroRules_Cont(lex, true, TOK_PAREN_OPEN, TOK_PAREN_CLOSE);
                
                GET_TOK(tok, lex);
                enum eTokenType joiner = TOK_NULL;
                if( tok.type() != TOK_PLUS && tok.type() != TOK_STAR )
                {
                    joiner = tok.type();
                    GET_TOK(tok, lex);
                }
                DEBUG("joiner = " << Token(joiner) << ", content = " << content);
                switch(tok.type())
                {
                case TOK_STAR:
                    ret.push_back( MacroRuleEnt(joiner, ::std::move(content)) );
                    break;
                case TOK_PLUS:
                    // TODO: Ensure that the plusses match
                    ret.push_back( MacroRuleEnt(joiner, ::std::move(content)) );
                    break;
                default:
                    throw ParseError::Unexpected(lex, tok);
                }
                
            }
            else if( tok.type() == TOK_IDENT )
            {
                ret.push_back( MacroRuleEnt(tok.str()) );
            }
            else if( tok.type() == TOK_RWORD_CRATE )
            {
                ret.push_back( MacroRuleEnt("*crate") );
            }
            else
            {
                throw ParseError::Unexpected(lex, tok);
            }
        }
        else
        {
            ret.push_back( MacroRuleEnt(tok) );
        }
    }
    
    return ret;
}

MacroRule Parse_MacroRules_Var(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token tok;
    
    MacroRule   rule;
    
    // Pattern
    enum eTokenType close;
    switch(GET_TOK(tok, lex))
    {
    case TOK_BRACE_OPEN:    close = TOK_BRACE_CLOSE;    break;
    case TOK_PAREN_OPEN:    close = TOK_PAREN_CLOSE;    break;
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    // - Pattern entries
    rule.m_pattern = Parse_MacroRules_Pat(lex, true, tok.type(), close);
    
    GET_CHECK_TOK(tok, lex, TOK_FATARROW);

    // Replacement
    switch(GET_TOK(tok, lex))
    {
    case TOK_BRACE_OPEN:    close = TOK_BRACE_CLOSE;    break;
    case TOK_PAREN_OPEN:    close = TOK_PAREN_CLOSE;    break;
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    rule.m_contents = Parse_MacroRules_Cont(lex, true, tok.type(), close);

    DEBUG("Rule - ["<<rule.m_pattern<<"] => "<<rule.m_contents<<"");
    
    return rule;
}

MacroRules Parse_MacroRules(TokenStream& lex)
{
    TRACE_FUNCTION_F("");
    
    Token tok;
    
    ::std::vector<MacroRule>    rules;
    while( GET_TOK(tok, lex) != TOK_EOF )
    {
        lex.putback(tok);
        
        rules.push_back( Parse_MacroRules_Var(lex) );
        if(GET_TOK(tok, lex) != TOK_SEMICOLON) {
            CHECK_TOK(tok, TOK_EOF);
            break;
        }
    }
    
    return MacroRules( mv$(rules) );
}
