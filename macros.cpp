/*
 */
#include "macros.hpp"
#include "parse/parseerror.hpp"
#include "parse/tokentree.hpp"
#include "parse/common.hpp"

#define FOREACH(basetype, it, src)  for(basetype::const_iterator it = src.begin(); it != src.end(); ++ it)

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
        FOREACH(MacroRules, rule_it, rules)
        {
            // Create token stream for input tree
            TTStream    lex(input);
            ::std::map<const char*,TokenTree>   bound_tts;
            // Parse according to rules
            bool fail = false;
            FOREACH(::std::vector<MacroPatEnt>, pat_it, rule_it->m_pattern)
            {
                Token   tok;
                TokenTree   val;
                const MacroPatEnt& pat = *pat_it;
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
            if( !fail && lex.getToken().type() == TOK_EOF )
            {
                throw ParseError::Todo("Macro expansions");
            }
        }
        throw ParseError::Todo("Error when macro fails to match");
    }

    throw ParseError::Generic( ::std::string("Macro '") + name + "' was not found" );
}

Token MacroExpander::realGetToken()
{
    throw ParseError::Todo("MacroExpander");
}
