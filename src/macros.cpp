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

TokenTree   g_crate_path_tt = TokenTree({
    TokenTree(Token(TOK_DOUBLE_COLON)),
    TokenTree(Token(TOK_STRING, "--CRATE--")),
    });

class MacroExpander:
    public TokenStream
{
public:
    // MultiMap (layer, name) -> TokenTree
    // - Multiple values are only allowed for layer>0
    typedef ::std::pair<unsigned,const char*>   t_mapping_key;

    struct cmp_mk {
        bool operator()(const t_mapping_key& a, const t_mapping_key& b) const {
            return a.first < b.first || ::std::strcmp(a.second, b.second) < 0;
        }
    };
    typedef ::std::multimap<t_mapping_key, TokenTree, cmp_mk>    t_mappings;

private:
    const TokenTree&    m_crate_path;
    const ::std::vector<MacroRuleEnt>&  m_root_contents;
    const t_mappings    m_mappings;
    
    /// Layer states : Index and Iteration
    ::std::vector< ::std::pair<size_t,size_t> >   m_offsets;
    
    /// Cached pointer to the current layer
    const ::std::vector<MacroRuleEnt>*  m_cur_ents;  // For faster lookup.
    /// Iteration counts for each layer
    ::std::vector<size_t>   m_layer_counts;

    ::std::unique_ptr<TTStream>   m_ttstream;
    
public:
    MacroExpander(const MacroExpander& x):
        m_crate_path(x.m_crate_path),
        m_root_contents(x.m_root_contents),
        m_mappings(x.m_mappings),
        m_offsets({ {0,0} }),
        m_cur_ents(&m_root_contents)
    {
        prep_counts();
    }
    MacroExpander(const ::std::vector<MacroRuleEnt>& contents, t_mappings mappings, const TokenTree& crate_path):
        m_crate_path(crate_path),
        m_root_contents(contents),
        m_mappings(mappings),
        m_offsets({ {0,0} }),
        m_cur_ents(&m_root_contents)
    {
        prep_counts();
    }

    virtual Position getPosition() const override;
    virtual Token realGetToken() override;
private:
    const ::std::vector<MacroRuleEnt>* getCurLayer() const;
    void prep_counts();
};

class MacroToken:
    public TokenStream
{
    Token   m_tok;
public:
    MacroToken(Token tok);
    virtual Position getPosition() const override;
    virtual Token realGetToken() override;
};

class MacroStringify:
    public TokenStream
{
    Token   m_tok;
public:
    MacroStringify(const TokenTree& input);
    virtual Position getPosition() const override;
    virtual Token realGetToken() override;
};

::std::unique_ptr<TokenStream> Macro_Invoke_Concat(const TokenTree& input, enum eTokenType exp);
::std::unique_ptr<TokenStream> Macro_Invoke_Cfg(const TokenTree& input);

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
        rule.m_pattern.push_back( MacroPatEnt(Token(TOK_NULL), false, {
            MacroPatEnt("tt", MacroPatEnt::PAT_TT), 
            } ) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_PAREN_OPEN)) );
        rule.m_contents.push_back( MacroRuleEnt(Token(TOK_PAREN_CLOSE)) );
        
        
        MacroRules  rules;
        rules.push_back(rule);
        g_macro_registrations.insert( make_pair(::std::string("panic"), rules));
    }
}

void Macro_HandlePattern(TTStream& lex, const MacroPatEnt& pat, unsigned int layer, MacroExpander::t_mappings& bound_tts)
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
        if( layer )
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
                    Macro_HandlePattern(lex, pat.subpats[0], layer+1, bound_tts);
                }
                catch(const ParseError::Base& e) {
                    DEBUG("Breakout");
                    lex = saved;
                    break;
                }
                for( unsigned int i = 1; i < pat.subpats.size(); i ++ )
                {
                    Macro_HandlePattern(lex, pat.subpats[i], layer+1, bound_tts);
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
        val = Parse_TT(lex, false);
        if(0)
    case MacroPatEnt::PAT_TYPE:
        val = Parse_TT_Type(lex);
        if(0)
    case MacroPatEnt::PAT_EXPR:
        val = Parse_TT_Expr(lex);
        if(0)
    case MacroPatEnt::PAT_STMT:
        val = Parse_TT_Stmt(lex);
        if(0)
    case MacroPatEnt::PAT_PATH:
        val = Parse_TT_Path(lex, false);    // non-expr mode
        if(0)
    case MacroPatEnt::PAT_BLOCK:
        val = Parse_TT_Block(lex);
        if(0)
    case MacroPatEnt::PAT_IDENT:
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            val = TokenTree(tok);
        }
        bound_tts.insert( ::std::make_pair( ::std::make_pair(layer, pat.name.c_str()), ::std::move(val) ) );
        break;
    
    //default:
    //    throw ParseError::Todo("full macro pattern matching");
    }

}

::std::unique_ptr<TokenStream> Macro_InvokeInt(const char *name, const MacroRules& rules, TokenTree input)
{
    TRACE_FUNCTION;
    
    // 2. Check input token tree against possible variants
    // 3. Bind names
    // 4. Return expander
    int i = 0;
    for(const auto& rule : rules)
    {
        Token   tok;
        // Create token stream for input tree
        TTStream    lex(input);
        /*
        enum eTokenType close;
        switch( GET_TOK(tok, lex) )
        {
        case TOK_PAREN_OPEN:    close = TOK_PAREN_CLOSE;    break;
        case TOK_BRACE_OPEN:    close = TOK_BRACE_CLOSE;    break;
        default:
            throw ParseError::Unexpected(lex, tok);
        }
        */
        MacroExpander::t_mappings   bound_tts;
        // Parse according to rules
        try
        {
            for(const auto& pat : rule.m_pattern)
            {
                Macro_HandlePattern(lex, pat, 0, bound_tts);
            }
            
            //GET_CHECK_TOK(tok, lex, close);
            GET_CHECK_TOK(tok, lex, TOK_EOF);
            DEBUG( rule.m_contents.size() << " rule contents bound to " << bound_tts.size() << " values - " << name );
            return ::std::unique_ptr<TokenStream>( (TokenStream*)new MacroExpander(rule.m_contents, bound_tts, g_crate_path_tt) );
        }
        catch(const ParseError::Base& e)
        {
            DEBUG("Parse of rule " << i << " of " << name <<" failed - " << e.what());
        }
        i ++;
    }
    DEBUG("");
    throw ParseError::Todo("Error when macro fails to match");
}

::std::unique_ptr<TokenStream> Macro_Invoke(const TokenStream& olex, const ::std::string& name, TokenTree input)
{
    // XXX: EVIL HACK! - This should be removed when std loading is implemented
    if( g_macro_registrations.size() == 0 ) {
        Macro_InitDefaults();
    }
   
    if( name == "concat_idents" ) {
        return Macro_Invoke_Concat(input, TOK_IDENT);
    }
    else if( name == "concat_strings" ) {
        return Macro_Invoke_Concat(input, TOK_STRING);
    }
    else if( name == "cfg" ) {
        return Macro_Invoke_Cfg(input);
    }
    else if( name == "stringify" ) {
        return ::std::unique_ptr<TokenStream>( (TokenStream*)new MacroStringify(input) );
    }
     
    // Look for macro in builtins
    t_macro_regs::iterator macro_reg = g_macro_registrations.find(name);
    if( macro_reg != g_macro_registrations.end() )
    {
        return Macro_InvokeInt(macro_reg->first.c_str(), macro_reg->second, input);
    }
    
    // Search import list
    for( auto ent = g_macro_module; ent; ent = ent->m_prev )
    {
        const AST::Module& mm = *ent->m_item;
        for( const auto &m : mm.macros() )
        {
            DEBUG("" << m.name);
            if( m.name == name )
            {
                return Macro_InvokeInt(m.name.c_str(), m.data, input);
            }
        }
        
        for( const auto& mi : mm.macro_imports_res() )
        {
            DEBUG("" << mi.name);
            if( mi.name == name )
            {
                return Macro_InvokeInt(mi.name.c_str(), *mi.data, input);
            }
        }
    }

    throw ParseError::Generic(olex, FMT("Macro '" << name << "' was not found") );
}

Position MacroExpander::getPosition() const
{
    return Position("Macro", 0);
}
Token MacroExpander::realGetToken()
{
    if( m_ttstream.get() )
    {
        DEBUG("TTStream set");
        Token rv = m_ttstream->getToken();
        if( rv.type() != TOK_EOF )
            return rv;
        m_ttstream.reset();
    }
    //DEBUG("ofs " << m_offsets << " < " << m_root_contents.size());
    
    // Check offset of lowest layer
    while(m_offsets.size() > 0)
    {
        assert(m_offsets.size() > 0);
        unsigned int layer = m_offsets.size() - 1;
        // - If that layer has hit its limit
        const auto& ents = *m_cur_ents;
        size_t idx = m_offsets.back().first;
        m_offsets.back().first ++;
    
        //DEBUG("ents = " << ents);
        
        // Check if limit has been reached
        if( idx < ents.size() )
        {
            const auto& ent = ents[idx];
            // - If not, just handle the next entry
            // Check type of entry
            if( ent.name != "" )
            {
                // - Name
                // HACK: Handle $crate with a special name
                if( ent.name == "*crate" ) {
                    m_ttstream.reset( new TTStream(m_crate_path) );
                    return m_ttstream->getToken();
                }
                else {
                    const size_t iter_idx = m_offsets.back().second;
                    const auto tt_i = m_mappings.equal_range( ::std::make_pair(layer, ent.name.c_str()) );
                    if( tt_i.first == tt_i.second )
                        throw ParseError::Generic( FMT("Cannot find mapping name: " << ent.name << " for layer " << layer) );
                    
                    size_t i = 0;
                    for( auto it = tt_i.first; it != tt_i.second; it ++ )
                    {
                        if( i == iter_idx )
                        {
                            m_ttstream.reset( new TTStream(it->second) );
                            return m_ttstream->getToken();
                        }
                        i ++;
                    }
                    throw ParseError::Generic( FMT("Cannot find mapping name: " << ent.name << " for layer " << layer) );
                }
            }
            else if( ent.subpats.size() != 0 )
            {
                // New layer
                // - Push an offset
                m_offsets.push_back( ::std::make_pair(0, 0) );
                // - Save the current layer
                m_cur_ents = getCurLayer();
                // - Restart loop for new layer
            }
            else
            {
                // Raw token
                return ent.tok;
            }
            // Fall through for loop
        }
        else
        {
            // - Otherwise, restart/end loop and fall through
            unsigned int layer_max = m_layer_counts.at(layer);
            if( m_offsets.back().second + 1 < layer_max )
            {
                DEBUG("Restart layer");
                m_offsets.back().first = 0;
                m_offsets.back().second ++;
                // Fall through and restart layer
            }
            else
            {
                DEBUG("Terminate layer");
                // Terminate loop, fall through to lower layers
                m_offsets.pop_back();
                // - Special case: End of macro, avoid issues
                if( m_offsets.size() == 0 )
                    break;
                m_cur_ents = getCurLayer();
            }
        }
    } // while( m_offsets NONEMPTY )
    
    DEBUG("EOF");
    return Token(TOK_EOF);
}

/// Count the number of names at each layer
void MacroExpander::prep_counts()
{
    struct TMP {
        size_t  count;
        const char *first_name;
    };
    ::std::vector<TMP>  counts;
    
    for( const auto& ent : m_mappings )
    {
        unsigned int layer = ent.first.first;
        const char *name = ent.first.second;
    
        if( layer >= counts.size() )
        {
            counts.resize(layer+1);
        }
        auto& l = counts[layer];
        if( l.first_name == NULL || ::std::strcmp(l.first_name, name) == 0 )
        {
            l.first_name = name;
            l.count += 1;
        }
    }
    
    for( const auto& l : counts )
    {
        m_layer_counts.push_back(l.count);
    }
}
const ::std::vector<MacroRuleEnt>* MacroExpander::getCurLayer() const
{
    assert( m_offsets.size() > 0 );
    const ::std::vector<MacroRuleEnt>* ents = &m_root_contents;
    for( unsigned int i = 0; i < m_offsets.size()-1; i ++ )
    {
        unsigned int ofs = m_offsets[i].first;
        //DEBUG(i << " ofs=" << ofs << " / " << ents->size());
        assert( ofs > 0 && ofs <= ents->size() );
        ents = &(*ents)[ofs-1].subpats;
        //DEBUG("ents = " << ents);
    }
    return ents;
}

::std::unique_ptr<TokenStream> Macro_Invoke_Concat(const TokenTree& input, enum eTokenType exp)
{
    Token   tok;
    TTStream    lex(input);
    ::std::string   val;
    do
    {
        GET_CHECK_TOK(tok, lex, exp);
        val += tok.str();
    } while( GET_TOK(tok, lex) == TOK_COMMA );
    CHECK_TOK(tok, TOK_EOF);
    
    
    return ::std::unique_ptr<TokenStream>( new MacroToken( Token(exp, val) ) );
}

::std::unique_ptr<TokenStream> Macro_Invoke_Cfg(const TokenTree& input)
{
    Token   tok;
    TTStream    lex(input);
    
    GET_CHECK_TOK(tok, lex, TOK_IDENT);
    ::std::string var = tok.str();
    
    if( GET_TOK(tok, lex) == TOK_EQUAL )
    {
        GET_CHECK_TOK(tok, lex, TOK_STRING);
        ::std::string val = tok.str();
        GET_CHECK_TOK(tok, lex, TOK_EOF);
        return ::std::unique_ptr<TokenStream>( new MacroToken( Token(TOK_RWORD_FALSE) ) );
    }
    else
    {
        CHECK_TOK(tok, TOK_EOF);
        return ::std::unique_ptr<TokenStream>( new MacroToken( Token(TOK_RWORD_FALSE) ) );
    }
}

MacroToken::MacroToken(Token tok):
    m_tok(tok)
{
}
Position MacroToken::getPosition() const
{
    return Position("macro", 0);
}
Token MacroToken::realGetToken()
{
    Token ret = m_tok;
    m_tok = Token(TOK_EOF);
    return ret;
}
MacroStringify::MacroStringify(const TokenTree& input)
{
    throw ParseError::Todo("Stringify");
}
Position MacroStringify::getPosition() const
{
    return Position("stringify", 0);
}
Token MacroStringify::realGetToken()
{
    Token ret = m_tok;
    m_tok = Token(TOK_EOF);
    return ret;
}
SERIALISE_TYPE_S(MacroRule, {
    s.item(m_pattern);
    s.item(m_contents);
});


void operator%(Serialiser& s, MacroPatEnt::Type c) {
    switch(c) {
    #define _(v) case MacroPatEnt::v: s << #v; return
    _(PAT_TOKEN);
    _(PAT_TT);
    _(PAT_EXPR);
    _(PAT_LOOP);
    //_(PAT_OPTLOOP);
    _(PAT_STMT);
    _(PAT_PATH);
    _(PAT_BLOCK);
    _(PAT_IDENT);
    #undef _
    }
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
    s.item(subpats);
});
