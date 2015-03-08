/*
 */
#include "common.hpp"
#include "macros.hpp"
#include "parse/parseerror.hpp"
#include "parse/tokentree.hpp"
#include "parse/common.hpp"
#include "ast/ast.hpp"

typedef ::std::map< ::std::string, MacroRules>  t_macro_regs;

t_macro_regs g_macro_registrations;
const LList<AST::Module*>*  g_macro_module;

void Macro_SetModule(const LList<AST::Module*>& mod)
{
    g_macro_module = &mod;
}

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
    
    // panic!() "macro"
    {
        MacroRule   rule;
        rule.m_pattern.push_back( MacroPatEnt(Token(TOK_NULL), {
            MacroPatEnt("tt", MacroPatEnt::PAT_TT), 
            } ) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_PAREN_OPEN)) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_PAREN_CLOSE)) );
        
        
        MacroRules  rules;
        rules.push_back(rule);
        g_macro_registrations.insert( make_pair(::std::string("panic"), rules));
    }
}

typedef ::std::map<const char*,TokenTree,cmp_str>   single_tts_t;
typedef ::std::multimap<const char*,TokenTree,cmp_str>   repeated_tts_t;

void Macro_HandlePattern(TTStream& lex, const MacroPatEnt& pat, bool rep, single_tts_t& bound_tts, repeated_tts_t& rep_bound_tts)
{
    Token   tok;
    TokenTree   val;
    switch(pat.type)
    {
    case MacroPatEnt::PAT_TOKEN:
        DEBUG("Token " << pat.tok);
        GET_CHECK_TOK(tok, lex, pat.tok.type());
        break;
    case MacroPatEnt::PAT_LOOP:
    //case MacroPatEnt::PAT_OPTLOOP:
        if( rep )
        {
            throw ParseError::BugCheck("Nested macro loop");
        }
        else
        {
            DEBUG("Loop");
            for(;;)
            {
                DEBUG("Try");
                TTStream    saved = lex;
                try {
                    Macro_HandlePattern(lex, pat.subpats[0], true, bound_tts, rep_bound_tts);
                }
                catch(const ParseError::Base& e) {
                    DEBUG("Breakout");
                    lex = saved;
                    break;
                }
                for( unsigned int i = 1; i < pat.subpats.size(); i ++ )
                {
                    Macro_HandlePattern(lex, pat.subpats[i], true, bound_tts, rep_bound_tts);
                }
                DEBUG("succ");
                if( pat.tok.type() != TOK_NULL )
                {
                    if( GET_TOK(tok, lex) != pat.tok.type() )
                    {
                        lex.putback(tok);
                        break;
                    }
                }
            }
            DEBUG("Done");
        }
        break;
    
    case MacroPatEnt::PAT_TT:
        DEBUG("TT");
        if( GET_TOK(tok, lex) == TOK_EOF )
            throw ParseError::Unexpected(lex, TOK_EOF);
        else
            lex.putback(tok);
        val = Parse_TT(lex);
        if(0)
    case MacroPatEnt::PAT_EXPR:
        val = Parse_TT_Expr(lex);
        if(0)
    case MacroPatEnt::PAT_STMT:
        val = Parse_TT_Stmt(lex);
        if(0)
    case MacroPatEnt::PAT_PATH:
        val = Parse_TT_Path(lex);
        if(0)
    case MacroPatEnt::PAT_BLOCK:
        val = Parse_TT_Block(lex);
        if(0)
    case MacroPatEnt::PAT_IDENT:
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            val = TokenTree(tok);
        }
        if(rep)
            rep_bound_tts.insert( std::make_pair(pat.name.c_str(), val) );
        else
            bound_tts.insert( std::make_pair(pat.name.c_str(), val) );
        break;
    
    //default:
    //    throw ParseError::Todo("full macro pattern matching");
    }

}

MacroExpander Macro_InvokeInt(const MacroRules& rules, TokenTree input)
{
    // 2. Check input token tree against possible variants
    // 3. Bind names
    // 4. Return expander
    for(const auto& rule : rules)
    {
        Token   tok;
        // Create token stream for input tree
        TTStream    lex(input);
        if(GET_TOK(tok, lex) == TOK_EOF) {
            throw ParseError::Unexpected(lex, tok);
        }
        ::std::map<const char*,TokenTree,cmp_str>   bound_tts;
        ::std::multimap<const char*,TokenTree,cmp_str>  rep_bound_tts;
        // Parse according to rules
        bool fail = false;
        try
        {
            for(const auto& pat : rule.m_pattern)
            {
                Macro_HandlePattern(lex, pat, false, bound_tts, rep_bound_tts);
            }
        }
        catch(const ParseError::Base& e)
        {
            DEBUG("Parse of rule failed - " << e.what());
            fail = true;
        }
        // TODO: Actually check if the final token is the closer to the first
        if( !fail )
        {
            if( GET_TOK(tok, lex) != TOK_EOF)
                throw ParseError::Unexpected(lex, tok);
            if( lex.getToken().type() == TOK_EOF )
                return MacroExpander(rule.m_contents, bound_tts);
        }
    }
    DEBUG("");
    throw ParseError::Todo("Error when macro fails to match");
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
        return Macro_InvokeInt(macro_reg->second, input);
    }
    
    for( auto ent = g_macro_module; ent; ent = ent->m_prev )
    {
        const AST::Module& mm = *ent->m_item;
        for( const auto &m : mm.macros() )
        {
            DEBUG("" << m.name);
            if( m.name == name )
            {
                return Macro_InvokeInt(m.data, input);
            }
        }
        
        for( const auto& mi : mm.macro_imports_res() )
        {
            DEBUG("" << mi.name);
            if( mi.name == name )
            {
                return Macro_InvokeInt(*mi.data, input);
            }
        }
    }

    throw ParseError::Generic( ::std::string("Macro '") + name + "' was not found" );
}

Position MacroExpander::getPosition() const
{
    return Position("Macro", 0);
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

SERIALISE_TYPE_S(MacroRule, {
    s.item(m_pattern);
    s.item(m_contents);
});


void operator%(Serialiser& s, MacroPatEnt::Type c) {
    switch(c) {
    #define _(ns,v) case ns v: s << #v; return;
    _(MacroPatEnt::,PAT_TOKEN);
    _(MacroPatEnt::,PAT_TT);
    _(MacroPatEnt::,PAT_EXPR);
    _(MacroPatEnt::,PAT_LOOP);
    //_(MacroPatEnt::,PAT_OPTLOOP);
    _(MacroPatEnt::,PAT_STMT);
    _(MacroPatEnt::,PAT_PATH);
    _(MacroPatEnt::,PAT_BLOCK);
    _(MacroPatEnt::,PAT_IDENT);
    #undef _
    }
    s << "--TODO--";
}
void operator%(::Deserialiser& s, MacroPatEnt::Type& c) {
    ::std::string   n;
    s.item(n);
    #define _(v) else if(n == #v) c = MacroPatEnt::v
    if(0) ;
    _(PAT_TOKEN);
    _(PAT_TT);
    _(PAT_EXPR);
    _(PAT_LOOP);
    //_(PAT_OPTLOOP);
    _(PAT_STMT);
    _(PAT_PATH);
    _(PAT_BLOCK);
    _(PAT_IDENT);
    else
        throw ::std::runtime_error( FMT("No conversion for '" << n << "'") );
    #undef _
}
SERIALISE_TYPE_S(MacroPatEnt, {
    s % type;
    s.item(name);
    s.item(tok);
    s.item(subpats);
});

SERIALISE_TYPE_S(MacroRuleEnt, {
    s.item(name);
    s.item(tok);
});
