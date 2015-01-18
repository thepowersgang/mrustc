/*
 */
#include "common.hpp"
#include "macros.hpp"
#include "parse/parseerror.hpp"
#include "parse/tokentree.hpp"
#include "parse/common.hpp"

typedef ::std::map< ::std::string, MacroRules>  t_macro_regs;

t_macro_regs g_macro_registrations;

void Macro_InitDefaults()
{
    // try!() macro
    {
        MacroRule   rule;
        rule.m_pattern.push_back( MacroPatEnt("val", MacroPatEnt::PAT_EXPR) );
        // match $rule {
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_RWORD_MATCH)) );
        rule.m_contents.push_back( MacroRuleEnt("val") );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_BRACE_OPEN)) );
        // Ok(v) => v,
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_IDENT, "Ok")) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_PAREN_OPEN)) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_IDENT, "v")) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_PAREN_CLOSE)) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_FATARROW)) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_IDENT, "v")) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_COMMA)) );
        // Err(e) => return Err(r),
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_IDENT, "Err")) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_PAREN_OPEN)) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_IDENT, "e")) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_PAREN_CLOSE)) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_FATARROW)) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_RWORD_RETURN)) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_IDENT, "Err")) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_PAREN_OPEN)) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_IDENT, "e")) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_PAREN_CLOSE)) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_COMMA)) );
        // }
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_BRACE_CLOSE)) );
        MacroRules  rules;
        rules.push_back(rule);
        g_macro_registrations.insert( make_pair(::std::string("try"), rules));
    }
}

MacroExpander Macro_Invoke(const char* name, TokenTree input)
{
    // XXX: EVIL HACK! - This should be removed when std loading is implemented
    if( g_macro_registrations.size() == 0 ) {
        Macro_InitDefaults();
    }
    // 1. Locate macro with that name
    t_macro_regs::iterator macro_reg = g_macro_registrations.find(name);
    if( macro_reg != g_macro_registrations.end() )
    {
        const MacroRules&  rules = macro_reg->second;
        // 2. Check input token tree against possible variants
        // 3. Bind names
        // 4. Return expander
        for(const auto& rule : rules)
        {
            Token   tok;
            // Create token stream for input tree
            TTStream    lex(input);
            if(GET_TOK(tok, lex) == TOK_EOF) {
                throw ParseError::Unexpected(tok);
            }
            ::std::map<const char*,TokenTree,cmp_str>   bound_tts;
            // Parse according to rules
            bool fail = false;
            for(const auto& pat : rule.m_pattern)
            {
                TokenTree   val;
                try
                {
                    switch(pat.type)
                    {
                    case MacroPatEnt::PAT_TOKEN:
                        GET_CHECK_TOK(tok, lex, pat.tok.type());
                        break;
                    case MacroPatEnt::PAT_EXPR:
                        val = Parse_TT_Expr(lex);
                        if(0)
                    case MacroPatEnt::PAT_STMT:
                        val = Parse_TT_Stmt(lex);
                        bound_tts.insert( std::make_pair(pat.name.c_str(), val) );
                        break;
                    default:
                        throw ParseError::Todo("macro pattern matching");
                    }
                }
                catch(const ParseError::Base& e)
                {
                    fail = true;
                    break;
                }
            }
            // TODO: Actually check if the final token is the closer to the first
            if( !fail && GET_TOK(tok, lex) == TOK_EOF) {
                throw ParseError::Unexpected(tok);
            }
            if( !fail && lex.getToken().type() == TOK_EOF )
            {
                return MacroExpander(rule.m_contents, bound_tts);
            }
        }
        throw ParseError::Todo("Error when macro fails to match");
    }

    throw ParseError::Generic( ::std::string("Macro '") + name + "' was not found" );
}

Token MacroExpander::realGetToken()
{
    if( m_ttstream.get() )
    {
        Token rv = m_ttstream->getToken();
        if( rv.type() != TOK_EOF )
            return rv;
        m_ttstream.reset();
    }
    if( m_ofs < m_contents.size() )
    {
        const MacroRuleEnt& ent = m_contents[m_ofs];
        m_ofs ++;
        if( ent.name.size() != 0 ) {
            // Binding!
            m_ttstream.reset( new TTStream(m_mappings.at(ent.name.c_str())) );
            return m_ttstream->getToken();
        }
        else {
            return ent.tok;
        }
        throw ParseError::Todo("MacroExpander - realGetToken");
    }
    return Token(TOK_EOF);
}
