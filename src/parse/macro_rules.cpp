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

struct UnifiedPatFrag
{
    ::std::vector<MacroPatEnt>  m_pats_ents;
    unsigned int    m_pattern_end;
    ::std::vector< UnifiedPatFrag >  m_next_frags;
    
    UnifiedPatFrag():
        m_pattern_end(~0)
    {}
    
    UnifiedPatFrag split_at(unsigned int remaining_count) {
        UnifiedPatFrag  rv;
        for(unsigned int i = remaining_count; i < m_pats_ents.size(); i ++)
            rv.m_pats_ents.push_back( mv$(m_pats_ents[i]) );
        m_pats_ents.resize(remaining_count);
        rv.m_pattern_end = m_pattern_end;   m_pattern_end = ~0;
        rv.m_next_frags = mv$(m_next_frags);
        return rv;
    }
};

struct UnifiedMacroRules
{
    UnifiedPatFrag  m_pattern;
};

bool is_token_path(eTokenType tt) {
    switch(tt)
    {
    case TOK_IDENT:
    case TOK_DOUBLE_COLON:
    case TOK_LT:
    case TOK_DOUBLE_LT:
    case TOK_RWORD_SELF:
    case TOK_RWORD_SUPER:
        return true;
    default:
        return false;
    }
}
bool is_token_pat(eTokenType tt) {
    if( is_token_path(tt) )
        return true;
    switch( tt )
    {
    case TOK_PAREN_OPEN:
    case TOK_SQUARE_OPEN:
    
    case TOK_AMP:
    case TOK_RWORD_BOX:
    case TOK_RWORD_REF:
    case TOK_RWORD_MUT:
    case TOK_STRING:
    case TOK_INTEGER:
    case TOK_CHAR:
        return true;
    default:
        return false;
    }
}
bool is_token_type(eTokenType tt) {
    if( is_token_path(tt) )
        return true;
    switch( tt )
    {
    case TOK_PAREN_OPEN:
    case TOK_SQUARE_OPEN:
    case TOK_STAR:
    case TOK_AMP:
        return true;
    default:
        return false;
    }
}
bool is_token_expr(eTokenType tt) {
    if( is_token_path(tt) )
        return true;
    switch( tt )
    {
    case TOK_AMP:
    case TOK_STAR:
    case TOK_PAREN_OPEN:
    case TOK_MACRO:
    case TOK_DASH:
    
    case TOK_INTEGER:
    case TOK_STRING:
        return true;
    default:
        return false;
    }
}

bool patterns_are_same(const Span& sp, const MacroPatEnt& left, const MacroPatEnt& right)
{
    if( left.type > right.type )
        return patterns_are_same(sp, right, left);
    
    // NOTE: left.type <= right.type
    switch(right.type)
    {
    case MacroPatEnt::PAT_TOKEN:
        assert( left.type == MacroPatEnt::PAT_TOKEN );
        return right.tok == left.tok;
    case MacroPatEnt::PAT_LOOP:
        switch(left.type)
        {
        case MacroPatEnt::PAT_TOKEN:
            // - Check for compatibility, but these two don't match
            if( patterns_are_same(sp, left, right.subpats.at(0)) == true )
                ERROR(sp, E0000, "Incompatible use of loop with matching non-loop");
            return false;
        case MacroPatEnt::PAT_LOOP:
            TODO(sp, "patterns_are_same - PAT_LOOP");
        default:
            assert( !"" );
        }
    
    case MacroPatEnt::PAT_TT:
        if( left.type == right.type )
            return true;
        ERROR(sp, E0000, "Incompatible macro fragments - " << right << " used with " << left);
        break;
    
    case MacroPatEnt::PAT_PAT:
        switch(left.type)
        {
        case MacroPatEnt::PAT_TOKEN:
            // - If this token is a valid pattern token, error
            if( is_token_pat(left.tok.type()) )
                ERROR(sp, E0000, "Incompatible macro fragments - " << right << " used with " << left);
            return false;
        case MacroPatEnt::PAT_PAT:
            return true;
        default:
            ERROR(sp, E0000, "Incompatible macro fragments - " << right << " used with " << left);
        }
        break;
    // `:ident` - Compatible with just other tokens
    case MacroPatEnt::PAT_IDENT:
        switch(left.type)
        {
        case MacroPatEnt::PAT_TOKEN:
            if( left.tok.type() == TOK_IDENT )
                ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
            return false;
        case MacroPatEnt::PAT_IDENT:
            return true;
        default:
            ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
        }
    case MacroPatEnt::PAT_PATH:
        switch(left.type)
        {
        case MacroPatEnt::PAT_TOKEN:
            if( is_token_path(left.tok.type()) )
                ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
            return false;
        case MacroPatEnt::PAT_PATH:
            return true;
        default:
            ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
        }
    case MacroPatEnt::PAT_TYPE:
        switch(left.type)
        {
        case MacroPatEnt::PAT_TOKEN:
            if( is_token_type(left.tok.type()) )
                ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
            return false;
        case MacroPatEnt::PAT_TYPE:
            return true;
        default:
            ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
        }
    case MacroPatEnt::PAT_EXPR:
        switch(left.type)
        {
        case MacroPatEnt::PAT_TOKEN:
            if( is_token_expr(left.tok.type()) )
                ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
            return false;
        case MacroPatEnt::PAT_EXPR:
            return true;
        default:
            ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
        }
    case MacroPatEnt::PAT_STMT:
        switch(left.type)
        {
        case MacroPatEnt::PAT_TOKEN:
            switch(left.tok.type())
            {
            case TOK_BRACE_OPEN:
            case TOK_RWORD_LET:
                ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
            default:
                if( is_token_expr(left.tok.type()) )
                    ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
                return false;
            }
        case MacroPatEnt::PAT_STMT:
            return true;
        default:
            ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
        }
    // Block - Expects '{' - Compatible with everything but a literal '{'
    case MacroPatEnt::PAT_BLOCK:
        switch(left.type)
        {
        case MacroPatEnt::PAT_TOKEN:
            if( left.tok.type() == TOK_BRACE_OPEN )
                ERROR(sp, E0000, "Incompatible macro fragments");
            return false;
        case MacroPatEnt::PAT_BLOCK:
            return true;
        default:
            return false;
        }
    // Matches meta/attribute fragments.
    // - Compatible with everythin but a literal #[ token
    case MacroPatEnt::PAT_META:
        switch(left.type)
        {
        case MacroPatEnt::PAT_TOKEN:
            if( left.tok.type() == TOK_ATTR_OPEN )
                ERROR(sp, E0000, "Incompatible macro fragments");
            return false;
        case MacroPatEnt::PAT_META:
            return true;
        default:
            return false;
        }
    }
    throw "";
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
    
    UnifiedPatFrag  root_frag;
    
    // Re-parse the patterns into a unified form
    for(unsigned int rule_idx = 0; rule_idx < rules.size(); rule_idx ++)
    {
        const auto& rule = rules[rule_idx];
        
        UnifiedPatFrag* cur_frag = &root_frag;
        unsigned int    frag_ofs = 0;
        for( const auto& pat : rule.m_pattern )
        {
            Span    sp(pat.tok.get_pos());
            
            if( frag_ofs == cur_frag->m_pats_ents.size() ) {
                if( cur_frag->m_pattern_end == ~0u && cur_frag->m_next_frags.size() == 0 ) {
                    cur_frag->m_pats_ents.push_back( pat );
                    frag_ofs += 1;
                }
                else {
                    // Check if any of the other paths match
                    bool found = false;
                    for( auto& next_frag : cur_frag->m_next_frags ) {
                        assert( next_frag.m_pats_ents.size() > 0 );
                        if( patterns_are_same( Span(pat.tok.get_pos()), next_frag.m_pats_ents[0], pat ) ) {
                            found = true;
                            cur_frag = &next_frag;
                            break;
                        }
                    }
                    // If not, create a new frag
                    if( ! found ) {
                        cur_frag->m_next_frags.push_back( UnifiedPatFrag() );
                        cur_frag = &cur_frag->m_next_frags.back();
                        cur_frag->m_pats_ents.push_back( pat );
                    }
                    frag_ofs = 1;
                }
            }
            else if( ! patterns_are_same(sp, cur_frag->m_pats_ents[frag_ofs], pat) ) {
                // Difference, split the block.
                auto new_frag = cur_frag->split_at(frag_ofs);
                assert( cur_frag->m_next_frags.size() == 0 );
                cur_frag->m_next_frags.push_back( mv$(new_frag) );
                
                // - Update cur_frag to a newly pushed fragment, and push this pattern to it
                cur_frag->m_next_frags.push_back( UnifiedPatFrag() );
                cur_frag = &cur_frag->m_next_frags.back();
                cur_frag->m_pats_ents.push_back( pat );
                frag_ofs = 1;
            }
            else {
                // Matches - Keep going
                frag_ofs += 1;
            }
        }
        
        // If this pattern ended before the current fragment ended
        if( frag_ofs < cur_frag->m_pats_ents.size() ) {
            // Split the current fragment
            auto new_frag = cur_frag->split_at(frag_ofs);
            assert( cur_frag->m_next_frags.size() == 0 );
            cur_frag->m_next_frags.push_back( mv$(new_frag) );
            // Keep cur_frag the same
        }
        cur_frag->m_pattern_end = rule_idx;
    }
    
    // TODO: use `root_frag` above for the actual evaluation
    
    return MacroRules( mv$(rules) );
}
