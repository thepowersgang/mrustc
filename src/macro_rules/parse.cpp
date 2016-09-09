/*
 */
#include <common.hpp>
#include "../parse/common.hpp"
#include "../parse/parseerror.hpp"
#include "macro_rules.hpp"
#include "pattern_checks.hpp"

MacroRulesPtr Parse_MacroRules(TokenStream& lex);

/// A rule within a macro_rules! blcok
class MacroRule
{
public:
    ::std::vector<MacroPatEnt>  m_pattern;
    ::std::vector<MacroExpansionEnt> m_contents;
};

::std::vector<MacroPatEnt> Parse_MacroRules_Pat(TokenStream& lex, bool allow_sub, enum eTokenType open, enum eTokenType close,  ::std::vector< ::std::string>& names)
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
                
                unsigned int idx = ::std::find( names.begin(), names.end(), name ) - names.begin();
                if( idx == names.size() )
                    names.push_back( name );
                
                if(0)
                    ;
                else if( type == "tt" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_TT) );
                else if( type == "pat" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_PAT) );
                else if( type == "ident" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_IDENT) );
                else if( type == "path" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_PATH) );
                else if( type == "expr" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_EXPR) );
                else if( type == "ty" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_TYPE) );
                else if( type == "meta" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_META) );
                else if( type == "block" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_BLOCK) );
                else
                    throw ParseError::Generic(lex, FMT("Unknown fragment type '" << type << "'"));
                break; }
            case TOK_PAREN_OPEN:
                if( allow_sub )
                {
                    auto subpat = Parse_MacroRules_Pat(lex, true, TOK_PAREN_OPEN, TOK_PAREN_CLOSE, names);
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

/// Parse the contents (replacement) of a macro_rules! arm
::std::vector<MacroExpansionEnt> Parse_MacroRules_Cont(
    TokenStream& lex,
    bool allow_sub, enum eTokenType open, enum eTokenType close,
    const ::std::vector< ::std::string>& var_names,
    ::std::set<unsigned int>* var_set_ptr=nullptr
    )
{
    TRACE_FUNCTION;
    
    Token tok;
    ::std::vector<MacroExpansionEnt> ret;
    
     int    depth = 0;
    while( GET_TOK(tok, lex) != close || depth > 0 )
    {
        if( tok.type() == TOK_EOF ) {
            throw ParseError::Unexpected(lex, tok);
        }
        if( tok.type() == TOK_NULL )
            continue ;
        
        if( tok.type() == open )
        {
            DEBUG("depth++");
            depth ++;
        }
        else if( tok.type() == close )
        {
            DEBUG("depth--");
            if(depth == 0)
                ERROR(lex.getPosition(), E0000, "Unmatched " << Token(close) << " in macro content");
            depth --;
        }
        
        // `$` - Macro metavars
        if( tok.type() == TOK_DOLLAR )
        {
            GET_TOK(tok, lex);
            
            // `$(`
            if( tok.type() == TOK_PAREN_OPEN )
            {   
                if( !allow_sub )
                    throw ParseError::Unexpected(lex, tok);
                
                ::std::set<unsigned int> var_set;
                auto content = Parse_MacroRules_Cont(lex, true, TOK_PAREN_OPEN, TOK_PAREN_CLOSE, var_names, &var_set);
                // ^^ The above will eat the PAREN_CLOSE
                
                if( var_set_ptr ) {
                    for(const auto& v : var_set)
                        var_set_ptr->insert( v );
                }
                
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
                case TOK_PLUS:
                    // TODO: Ensure that +/* match up
                    ret.push_back( MacroExpansionEnt({mv$(content), joiner, mv$(var_set)}) );
                    break;
                default:
                    throw ParseError::Unexpected(lex, tok);
                }
                
            }
            else if( tok.type() == TOK_IDENT )
            {
                // Look up the named parameter in the list of param names for this arm
                unsigned int idx = ::std::find(var_names.begin(), var_names.end(), tok.str()) - var_names.begin();
                if( idx == var_names.size() )
                    ERROR(lex.getPosition(), E0000, "Macro variable $" << tok.str() << " not found");
                if( var_set_ptr ) {
                    var_set_ptr->insert( idx );
                }
                ret.push_back( MacroExpansionEnt(idx) );
            }
            else if( tok.type() == TOK_RWORD_CRATE )
            {
                ret.push_back( MacroExpansionEnt( (1<<30) | 0 ) );
            }
            else
            {
                throw ParseError::Unexpected(lex, tok);
            }
        }
        else
        {
            ret.push_back( MacroExpansionEnt( mv$(tok) ) );
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
    ::std::vector< ::std::string>   names;
    rule.m_pattern = Parse_MacroRules_Pat(lex, true, tok.type(), close,  names);
    
    GET_CHECK_TOK(tok, lex, TOK_FATARROW);

    // Replacement
    switch(GET_TOK(tok, lex))
    {
    case TOK_BRACE_OPEN:    close = TOK_BRACE_CLOSE;    break;
    case TOK_PAREN_OPEN:    close = TOK_PAREN_CLOSE;    break;
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    rule.m_contents = Parse_MacroRules_Cont(lex, true, tok.type(), close, names);

    DEBUG("Rule - ["<<rule.m_pattern<<"] => "<<rule.m_contents<<"");
    
    return rule;
}

bool patterns_are_same(const Span& sp, const MacroPatEnt& left, const MacroPatEnt& right)
{
    if( left.type > right.type )
        return patterns_are_same(sp, right, left);
    
    //if( left.name != right.name ) {
    //    TODO(sp, "Handle different binding names " << left << " != " << right);
    //}
    
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
        // TODO: Allow a loop starting with an expr?
        // - Consume, add split based on loop condition or next arm
        // - Possible problem with binding levels.
        case MacroPatEnt::PAT_EXPR:
            return true;
        default:
            ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
        }
    case MacroPatEnt::PAT_STMT:
        switch(left.type)
        {
        case MacroPatEnt::PAT_TOKEN:
            if( is_token_stmt(left.tok.type()) )
                ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
            return false;
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
    case MacroPatEnt::PAT_META:
        switch(left.type)
        {
        case MacroPatEnt::PAT_TOKEN:
            if( left.tok.type() == TOK_IDENT )
                ERROR(sp, E0000, "Incompatible macro fragments");
            return false;
        case MacroPatEnt::PAT_META:
            return true;
        default:
            ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
        }
    }
    throw "";
}

MacroRulesPatFrag split_fragment_at(MacroRulesPatFrag& frag, unsigned int remaining_count)
{
    MacroRulesPatFrag   rv;
    for(unsigned int i = remaining_count; i < frag.m_pats_ents.size(); i ++)
        rv.m_pats_ents.push_back( mv$(frag.m_pats_ents[i]) );
    frag.m_pats_ents.resize(remaining_count);
    rv.m_pattern_end = frag.m_pattern_end;   frag.m_pattern_end = ~0;
    rv.m_next_frags = mv$(frag.m_next_frags);
    return rv;
}

void enumerate_names(const ::std::vector<MacroPatEnt>& pats, ::std::vector< ::std::string>& names) {
    for( const auto& pat : pats )
    {
        if( pat.type == MacroPatEnt::PAT_LOOP ) {
            enumerate_names(pat.subpats, names);
        }
        else if( pat.name != "" ) {
            auto b = names.begin();
            auto e = names.end();
            if( ::std::find(b, e, pat.name) == e ) {
                names.push_back( pat.name );
            }
        }
    }
}

MacroRulesPtr Parse_MacroRules(TokenStream& lex)
{
    TRACE_FUNCTION_F("");
    
    Token tok;
    
    // Parse the patterns and replacements
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
    DEBUG("- " << rules.size() << " rules");
    
    MacroRulesPatFrag   root_frag;
    ::std::vector<MacroRulesArm>    rule_arms;
    
    // Re-parse the patterns into a unified form
    for(unsigned int rule_idx = 0; rule_idx < rules.size(); rule_idx ++)
    {
        auto& rule = rules[rule_idx];
        MacroRulesArm   arm = MacroRulesArm( mv$(rule.m_contents) );
        
        enumerate_names(rule.m_pattern,  arm.m_param_names);
        
        auto* cur_frag = &root_frag;
        unsigned int    frag_ofs = 0;
        for( const auto& pat : rule.m_pattern )
        {
            Span    sp(pat.tok.get_pos());
            
            // If the current position is the end of the current fragment:
            if( frag_ofs == cur_frag->m_pats_ents.size() ) {
                // But this fragment is incomplete (doesn't end a pattern, or split)
                if( cur_frag->m_pattern_end == ~0u && cur_frag->m_next_frags.size() == 0 ) {
                    // Keep pushing onto the end
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
                        cur_frag->m_next_frags.push_back( MacroRulesPatFrag() );
                        cur_frag = &cur_frag->m_next_frags.back();
                        cur_frag->m_pats_ents.push_back( pat );
                    }
                    frag_ofs = 1;
                }
            }
            // TODO: If `:expr` and `$( :expr)` are seen, split _after_ the :expr
            else if( ! patterns_are_same(sp, cur_frag->m_pats_ents[frag_ofs], pat) ) {
                // Difference, split the block.
                auto new_frag = split_fragment_at(*cur_frag, frag_ofs);
                assert( cur_frag->m_next_frags.size() == 0 );
                cur_frag->m_next_frags.push_back( mv$(new_frag) );
                
                // - Update cur_frag to a newly pushed fragment, and push this pattern to it
                cur_frag->m_next_frags.push_back( MacroRulesPatFrag() );
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
            auto new_frag = split_fragment_at(*cur_frag, frag_ofs);
            assert( cur_frag->m_next_frags.size() == 0 );
            cur_frag->m_next_frags.push_back( mv$(new_frag) );
            // Keep cur_frag the same
        }
        cur_frag->m_pattern_end = rule_idx;
        
        rule_arms.push_back( mv$(arm) );
    }
    
    // TODO: use `root_frag` above for the actual evaluation
    
    auto rv = new MacroRules();
    rv->m_pattern = mv$(root_frag);
    rv->m_rules = mv$(rule_arms);
    
    return MacroRulesPtr(rv);
}
