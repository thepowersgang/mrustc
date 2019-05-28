/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * macro_rules/parse.cpp
 * - macro_rules! parsing
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

/// Parse the pattern of a macro_rules! arm
::std::vector<MacroPatEnt> Parse_MacroRules_Pat(TokenStream& lex, enum eTokenType open, enum eTokenType close,  ::std::vector< ::std::string>& names)
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
            // TODO: Allow any reserved word
            case TOK_RWORD_TYPE:
            case TOK_RWORD_PUB:
            case TOK_IDENT: {
                ::std::string   name = tok.type() == TOK_IDENT ? mv$(tok.str()) : FMT(tok);
                GET_CHECK_TOK(tok, lex, TOK_COLON);
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                ::std::string   type = mv$(tok.str());

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
                else if( type == "stmt" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_STMT) );
                else if( type == "ty" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_TYPE) );
                else if( type == "meta" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_META) );
                else if( type == "block" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_BLOCK) );
                else if( type == "item" )
                    ret.push_back( MacroPatEnt(name, idx, MacroPatEnt::PAT_ITEM) );
                else
                    ERROR(lex.point_span(), E0000, "Unknown fragment type '" << type << "'");
                break; }
            case TOK_PAREN_OPEN: {
                auto subpat = Parse_MacroRules_Pat(lex, TOK_PAREN_OPEN, TOK_PAREN_CLOSE, names);
                enum eTokenType joiner = TOK_NULL;
                GET_TOK(tok, lex);
                if( tok.type() != TOK_PLUS && tok.type() != TOK_STAR )
                {
                    DEBUG("joiner = " << tok);
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
                break; }
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
    enum eTokenType open, enum eTokenType close,
    const ::std::vector< ::std::string>& var_names,
    ::std::map<unsigned int,bool>* var_set_ptr=nullptr
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
                ERROR(lex.point_span(), E0000, "Unmatched " << Token(close) << " in macro content");
            depth --;
        }

        // `$` - Macro metavars
        if( tok.type() == TOK_DOLLAR )
        {
            GET_TOK(tok, lex);

            // `$(`
            if( tok.type() == TOK_PAREN_OPEN )
            {
                ::std::map<unsigned int, bool> var_set;
                auto content = Parse_MacroRules_Cont(lex, TOK_PAREN_OPEN, TOK_PAREN_CLOSE, var_names, &var_set);
                // ^^ The above will eat the PAREN_CLOSE

                GET_TOK(tok, lex);
                enum eTokenType joiner = TOK_NULL;
                if( tok.type() != TOK_PLUS && tok.type() != TOK_STAR )
                {
                    joiner = tok.type();
                    GET_TOK(tok, lex);
                }
                bool is_optional = (tok.type() == TOK_STAR);
                if( var_set_ptr ) {
                    for(const auto& v : var_set) {
                        // If `is_optional`: Loop may not be expanded, so var_not_opt=false
                        // Else, inherit
                        bool var_not_opt = (is_optional ? false : v.second);
                        var_set_ptr->insert( ::std::make_pair(v.first, var_not_opt) ).first->second |= var_not_opt;
                    }
                }
                DEBUG("joiner = " << Token(joiner) << ", content = " << content);
                switch(tok.type())
                {
                case TOK_PLUS:
                case TOK_STAR:
                    // TODO: Ensure that +/* match up?
                    ret.push_back( MacroExpansionEnt({mv$(content), joiner, mv$(var_set)}) );
                    break;
                default:
                    throw ParseError::Unexpected(lex, tok);
                }

            }
            //else if( tok.type() == TOK_IDENT || tok_is_rword(tok.type()) )
            else if( tok.type() == TOK_IDENT || tok.type() == TOK_RWORD_TYPE || tok.type() == TOK_RWORD_PUB )
            {
                // Look up the named parameter in the list of param names for this arm
                auto name = tok.type() == TOK_IDENT ? tok.str() : FMT(tok);
                unsigned int idx = ::std::find(var_names.begin(), var_names.end(), name) - var_names.begin();
                if( idx == var_names.size() ) {
                    // TODO: `error-chain`'s quick_error macro has an arm which refers to an undefined metavar.
                    // - Maybe emit a warning and use a marker index.
                    WARNING(lex.point_span(), W0000, "Macro variable $" << name << " not found");
                    // Emit the literal $ <name>
                    ret.push_back( MacroExpansionEnt(Token(TOK_DOLLAR)) );
                    ret.push_back( MacroExpansionEnt(mv$(tok)) );
                    continue ;
                }
                if( var_set_ptr ) {
                    var_set_ptr->insert( ::std::make_pair(idx,true) );
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

/// Parse a single arm of a macro_rules! block - `(foo) => (bar)`
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
    case TOK_SQUARE_OPEN:   close = TOK_SQUARE_CLOSE;   break;
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    // - Pattern entries
    ::std::vector< ::std::string>   names;
    rule.m_pattern = Parse_MacroRules_Pat(lex, tok.type(), close,  names);

    GET_CHECK_TOK(tok, lex, TOK_FATARROW);

    // Replacement
    switch(GET_TOK(tok, lex))
    {
    case TOK_BRACE_OPEN:    close = TOK_BRACE_CLOSE;    break;
    case TOK_PAREN_OPEN:    close = TOK_PAREN_CLOSE;    break;
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    rule.m_contents = Parse_MacroRules_Cont(lex, tok.type(), close, names);

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
    // Matches items
    case MacroPatEnt::PAT_ITEM:
        switch(left.type)
        {
        case MacroPatEnt::PAT_TOKEN:
            if( is_token_item(left.tok.type()) )
                ERROR(sp, E0000, "Incompatible macro fragments");
            return false;
        case MacroPatEnt::PAT_ITEM:
            return true;
        default:
            ERROR(sp, E0000, "Incompatible macro fragments " << right << " used with " << left);
        }
    }
    throw "";
}

// TODO: Also count the number of times each variable is used?
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

/// Parse an entire macro_rules! block into a format that exec.cpp can use
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

    ::std::vector<MacroRulesArm>    rule_arms;

    // Re-parse the patterns into a unified form
    for(auto& rule : rules)
    {
        MacroRulesArm   arm = MacroRulesArm( mv$(rule.m_pattern), mv$(rule.m_contents) );

        enumerate_names(arm.m_pattern,  arm.m_param_names);

        rule_arms.push_back( mv$(arm) );
    }

    auto rv = new MacroRules( );
    rv->m_hygiene = lex.getHygiene();
    rv->m_rules = mv$(rule_arms);

    return MacroRulesPtr(rv);
}
