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

class ParameterMappings
{
    // MultiMap (layer, name) -> TokenTree
    // - Multiple values are only allowed for layer>0
    typedef ::std::pair<unsigned, unsigned> t_mapping_block;
    struct t_mapping_key {
        t_mapping_block block;
        const char*     name;
        
        bool operator<(const t_mapping_key& x) const {
            if( block < x.block )   return true;
            if( block > x.block )   return false;
            int cmp = ::std::strcmp(name, x.name);
            if( cmp < 0 )   return true;
            if( cmp > 0 )   return false;
            // Equal
            return false;
        }
        friend ::std::ostream& operator<<(::std::ostream& os, const t_mapping_key& x) {
            os << "(" << x.block.first << ", " << x.block.second << " - '"<<x.name<<"')";
            return os;
        }
    };

    ::std::vector<unsigned int> m_layer_indexes;
    ::std::multimap<t_mapping_key, TokenTree>   m_inner;
    ::std::map<t_mapping_block, unsigned int> m_counts;
    
public:
    ParameterMappings()
    {
    }
    
    const ::std::multimap<t_mapping_key, TokenTree>& inner_() const {
        return m_inner;
    }
    
    size_t layer_count() const {
        return m_layer_indexes.size();
    }
    
    void prep_layer(unsigned int layer) {
        // Need to ensure that [layer] is valid for insert
        while( m_layer_indexes.size() <= layer )
            m_layer_indexes.push_back(0);
    }
    void inc_layer(unsigned int layer) {
        m_layer_indexes.at(layer) ++;
    }
    
    void insert(unsigned int layer, const char *name, TokenTree data) {
        unsigned int iteration = (layer > 0 ? m_layer_indexes.at(layer-1) : 0);
        m_inner.insert( ::std::make_pair(
            t_mapping_key { {layer, iteration}, name },
            ::std::move(data)
            ) );
    }
    
    void calculate_counts()
    {
        assert( m_counts.size() == 0 );
        auto ins_it = m_counts.begin();
        const char* name = nullptr;
        for( const auto& p : m_inner )
        {
            // If the first iteration, or the block changes
            if( ins_it == m_counts.end() || ins_it->first < p.first.block )
            {
                ins_it = m_counts.insert(ins_it, ::std::make_pair(p.first.block, 1));
                name = p.first.name;
            }
            else if( ::std::strcmp(name, p.first.name) == 0 )
            {
                ins_it->second ++;
            }
            else
            {
                // Ignore, the name has changed
            }
        }
    }
    
    unsigned int iter_count(unsigned int layer, unsigned int parent_iteration) const {
        t_mapping_block block = {layer, parent_iteration};
        DEBUG("block = " << block);
        const auto it = m_counts.find( block );
        if( it == m_counts.end() ) {
            DEBUG("m_counts = " << m_counts);
            return 0;
        }
        return it->second;
    }
    
    const TokenTree* get(unsigned int layer, unsigned int iteration, const char *name, unsigned int idx) const
    {
        const auto it_range = m_inner.equal_range( t_mapping_key { {layer, iteration}, name } );
        if( it_range.first == it_range.second ) {
            DEBUG("m_mappings = " << m_inner);
            return nullptr;
        }
        
        size_t i = 0;
        for( auto it = it_range.first; it != it_range.second; it ++ )
        {
            if( i == idx )
            {
                DEBUG(name << " #" << i);
                return &it->second;
            }
            i ++;
        }
        DEBUG("Not enough mappings for name " << name << " at layer " << layer << " PI " << iteration);
        return nullptr;
    }
};

class MacroExpander:
    public TokenStream
{
public:

private:
    const TokenStream&  m_olex;
    const ::std::string m_crate_name;
    const ::std::vector<MacroRuleEnt>&  m_root_contents;
    const ParameterMappings m_mappings;
    
    /// Layer states : Index and Iteration
    ::std::vector< ::std::pair<size_t,size_t> >   m_offsets;
    ::std::vector< size_t > m_layer_iters;
    
    /// Cached pointer to the current layer
    const ::std::vector<MacroRuleEnt>*  m_cur_ents;  // For faster lookup.

    Token   m_next_token;   // used for inserting a single token into the stream
    ::std::unique_ptr<TTStream>   m_ttstream;
    
public:
    MacroExpander(const MacroExpander& x):
        m_olex(x.m_olex),
        m_crate_name(x.m_crate_name),
        m_root_contents(x.m_root_contents),
        m_mappings(x.m_mappings),
        m_offsets({ {0,0} }),
        m_cur_ents(&m_root_contents)
    {
        prep_counts();
    }
    MacroExpander(const TokenStream& olex, const ::std::vector<MacroRuleEnt>& contents, ParameterMappings mappings, ::std::string crate_name):
        m_olex(olex),
        m_crate_name(crate_name),
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
    const MacroRuleEnt& getCurLayerEnt() const;
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
const LList<AST::Module*>* Macro_GetModule()
{
    return g_macro_module;
}

void Macro_InitDefaults()
{
}

bool Macro_TryPattern(TTStream& lex, const MacroPatEnt& pat)
{
    DEBUG("pat = " << pat);
    switch(pat.type)
    {
    case MacroPatEnt::PAT_TOKEN: {
        Token tok = lex.getToken();
        if( tok != pat.tok ) {
            lex.putback(tok);
            return false;
        }
        else {
            lex.putback(tok);
            return true;
        }
        }
    case MacroPatEnt::PAT_LOOP:
        if( pat.name == "*" )
            return true;
        return Macro_TryPattern(lex, pat.subpats[0]);
    case MacroPatEnt::PAT_BLOCK:
        return LOOK_AHEAD(lex) == TOK_BRACE_OPEN;
    case MacroPatEnt::PAT_IDENT:
        return LOOK_AHEAD(lex) == TOK_IDENT;
    case MacroPatEnt::PAT_TT:
        return LOOK_AHEAD(lex) != TOK_EOF;
    case MacroPatEnt::PAT_PATH:
        return LOOK_AHEAD(lex) == TOK_IDENT
            || LOOK_AHEAD(lex) == TOK_DOUBLE_COLON
            || LOOK_AHEAD(lex) == TOK_RWORD_SUPER;
    case MacroPatEnt::PAT_TYPE:
        try {
            TTStream slex = lex;
            Parse_TT_Type(slex);
            return true;
        }
        catch( const CompileError::Base& e ) {
            return false;
        }
    case MacroPatEnt::PAT_EXPR:
        return Parse_IsTokValue( LOOK_AHEAD(lex) );
    case MacroPatEnt::PAT_STMT:
        try {
            TTStream slex = lex;
            Parse_TT_Stmt(slex);
            return true;
        }
        catch( const CompileError::Base& e ) {
            return false;
        }
    case MacroPatEnt::PAT_PAT:
        try {
            TTStream slex = lex;
            Parse_TT_Pattern(slex);
            return true;
        }
        catch( const CompileError::Base& e ) {
            return false;
        }
    }
    throw ParseError::Todo(lex, FMT("Macro_TryPattern : " << pat));
}

bool Macro_HandlePattern(TTStream& lex, const MacroPatEnt& pat, unsigned int layer, ParameterMappings& bound_tts)
{
    TRACE_FUNCTION_F("layer = " << layer);
    Token   tok;
    TokenTree   val;
    
    if( !Macro_TryPattern(lex, pat) ) {
        DEBUG("FAIL");
        return false;
    }
    
    switch(pat.type)
    {
    case MacroPatEnt::PAT_TOKEN:
        DEBUG("Token " << pat.tok);
        GET_CHECK_TOK(tok, lex, pat.tok.type());
        break;
    case MacroPatEnt::PAT_LOOP:
    //case MacroPatEnt::PAT_OPTLOOP:
        {
        unsigned int match_count = 0;
        DEBUG("Loop");
        for(;;)
        {
            bound_tts.prep_layer(layer+1);
            if( ! Macro_TryPattern(lex, pat.subpats[0]) )
            {
                DEBUG("break");
                break;
            }
            for( unsigned int i = 0; i < pat.subpats.size(); i ++ )
            {
                if( !Macro_HandlePattern(lex, pat.subpats[i], layer+1, bound_tts) ) {
                    DEBUG("Ent " << i << " failed");
                    return false;
                }
            }
            bound_tts.inc_layer(layer+1);
            match_count += 1;
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
        DEBUG("Done (" << match_count << " matches)");
        break; }
    
    case MacroPatEnt::PAT_TT:
        DEBUG("TT");
        if( GET_TOK(tok, lex) == TOK_EOF )
            throw ParseError::Unexpected(lex, TOK_EOF);
        else
            lex.putback(tok);
        val = Parse_TT(lex, false);
        if(0)
    case MacroPatEnt::PAT_PAT:
        val = Parse_TT_Pattern(lex);
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
        bound_tts.insert( layer, pat.name.c_str(), ::std::move(val) );
        break;
    
    //default:
    //    throw ParseError::Todo("full macro pattern matching");
    }
    return true;
}

::std::unique_ptr<TokenStream> Macro_InvokeInt(const TokenStream& olex, const char *name, const MacroRules& rules, TokenTree input)
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
        ParameterMappings   bound_tts;
        // Parse according to rules
        try
        {
            for(const auto& pat : rule.m_pattern)
            {
                if( !Macro_HandlePattern(lex, pat, 0, bound_tts) )
                    throw ParseError::Generic(lex, "Macro pattern failed");
            }
            
            //GET_CHECK_TOK(tok, lex, close);
            GET_CHECK_TOK(tok, lex, TOK_EOF);
            DEBUG( rule.m_contents.size() << " rule contents bound to " << bound_tts.inner_().size() << " values - " << name );
            for( const auto& v : bound_tts.inner_() )
            {
                DEBUG("- " << v.first << " = [" << v.second << "]");
            }
            
            // Count the number of repetitions
            bound_tts.calculate_counts();
            
            DEBUG("TODO: Obtain crate name correctly");
            TokenStream* ret_ptr = new MacroExpander(olex, rule.m_contents, bound_tts, "");
            // HACK! Disable nested macro expansion
            //ret_ptr->parse_state().no_expand_macros = true;
            
            return ::std::unique_ptr<TokenStream>( ret_ptr );
        }
        catch(const CompileError::Base& e)
        {
            DEBUG("Parse of rule " << i << " of " << name <<" failed - " << e.what());
        }
        i ++;
    }
    DEBUG("");
    throw ParseError::Todo(olex, "Error when macro fails to match");
}

::std::unique_ptr<TokenStream> Macro_Invoke(const TokenStream& olex, const ::std::string& name, TokenTree input)
{
    DEBUG("Invoke " << name << " from " << olex.getPosition());
    // XXX: EVIL HACK! - This should be removed when std loading is implemented
    if( g_macro_registrations.size() == 0 ) {
        Macro_InitDefaults();
    }
   
    if( name == "concat_idents" ) {
        return Macro_Invoke_Concat(input, TOK_IDENT);
    }
    else if( name == "concat_strings" || name == "concat" ) {
        return Macro_Invoke_Concat(input, TOK_STRING);
    }
    else if( name == "cfg" ) {
        return Macro_Invoke_Cfg(input);
    }
    else if( name == "stringify" ) {
        return ::std::unique_ptr<TokenStream>( (TokenStream*)new MacroStringify(input) );
    }
    else if( name == "file" ) {
        const ::std::string& pos = olex.getPosition().filename;
        return ::std::unique_ptr<TokenStream>( (TokenStream*)new MacroToken(Token(TOK_STRING, pos)) );
    }
    else if( name == "line" ) {
        auto pos = olex.getPosition().line;
        return ::std::unique_ptr<TokenStream>( (TokenStream*)new MacroToken(Token((uint64_t)pos, CORETYPE_U32)) );
    }
     
    // Look for macro in builtins
    t_macro_regs::iterator macro_reg = g_macro_registrations.find(name);
    if( macro_reg != g_macro_registrations.end() )
    {
        return Macro_InvokeInt(olex, macro_reg->first.c_str(), macro_reg->second, input);
    }
    
    // Search import list
    for( auto ent = g_macro_module; ent; ent = ent->m_prev )
    {
        const AST::Module& mm = *ent->m_item;
        DEBUG("Module '" << mm.name() << "'");
        for( unsigned int i = mm.macros().size(); i --; )
        {
            const auto& m = mm.macros()[i];
            DEBUG("- [local] " << m.name);
            if( m.name == name )
            {
                return Macro_InvokeInt(olex, m.name.c_str(), m.data, input);
            }
        }
        
        for( const auto& mi : mm.macro_imports_res() )
        {
            DEBUG("- [imp]" << mi.name);
            if( mi.name == name )
            {
                return Macro_InvokeInt(olex, mi.name.c_str(), *mi.data, input);
            }
        }
    }

    throw ParseError::Generic(olex, FMT("Macro '" << name << "' was not found") );
}


Position MacroExpander::getPosition() const
{
    DEBUG("olex.getPosition() = " << m_olex.getPosition());
    return Position(FMT("Macro:" << ""), m_offsets[0].first);
}
Token MacroExpander::realGetToken()
{
    // Use m_next_token first
    if( m_next_token.type() != TOK_NULL )
    {
        DEBUG("m_next_token = " << m_next_token);
        return ::std::move(m_next_token);
    }
    // Then try m_ttstream
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
        unsigned int layer = m_offsets.size() - 1;
        const auto& ents = *m_cur_ents;
        
        // Obtain current read position in layer, and increment
        size_t idx = m_offsets.back().first;
        m_offsets.back().first ++;
        
        // Check if limit has been reached
        if( idx < ents.size() )
        {
            // - If not, just handle the next entry
            const auto& ent = ents[idx];
            // Check type of entry
            // - XXX: Hack for $crate special name
            if( ent.name == "*crate" )
            {
                DEBUG("Crate name hack");
                if( m_crate_name != "" )
                {
                    m_next_token = Token(TOK_STRING, m_crate_name);
                    return Token(TOK_DOUBLE_COLON);
                }
            }
            // - Expand to a parameter
            else if( ent.name != "" )
            {
                const TokenTree* tt;
                unsigned int search_layer = layer;
                do {
                    unsigned int parent_iter = (search_layer > 0 ? m_layer_iters.at(search_layer-1) : 0);
                    const size_t iter_idx = m_offsets.at(search_layer).second;
                    tt = m_mappings.get(search_layer, parent_iter, ent.name.c_str(), iter_idx);
                } while( !tt && search_layer-- > 0 );
                if( ! tt )
                {
                    throw ParseError::Generic(*this, FMT("Cannot find '" << ent.name << "' for " << layer));
                }
                else
                {
                    m_ttstream.reset( new TTStream(*tt) );
                    return m_ttstream->getToken();
                }
            }
            // - Descend into a repetition
            else if( ent.subpats.size() != 0 )
            {
                DEBUG("desc: layer = " << layer << ", m_layer_iters = " << m_layer_iters);
                unsigned int layer_iter = m_layer_iters.at(layer);
                unsigned int num_repeats = m_mappings.iter_count(layer+1, layer_iter);
                // New layer
                DEBUG("- NL = " << layer+1 << ", count = " << num_repeats );
                if( num_repeats > 0 )
                {
                    // - Push an offset
                    m_offsets.push_back( ::std::make_pair(0, 0) );
                    // - Save the current layer
                    m_cur_ents = getCurLayer();
                    // - Restart loop for new layer
                }
                else
                {
                    // Layer empty
                    DEBUG("Layer " << layer+1 << " is empty");
                }
                // VVV fall through and continue loop
            }
            // - Emit a raw token
            else
            {
                return ent.tok;
            }
            // Fall through for loop
        }
        else if( layer > 0 )
        {
            // - Otherwise, restart/end loop and fall through
            DEBUG("layer = " << layer << ", m_layer_iters = " << m_layer_iters);
            unsigned int parent_layer_iter = m_layer_iters.at(layer-1);
            unsigned int layer_max = m_mappings.iter_count(layer, parent_layer_iter);
            DEBUG("Layer #" << layer << " Cur: " << m_offsets.back().second << ", Max: " << layer_max);
            if( m_offsets.back().second + 1 < layer_max )
            {
                m_layer_iters.at(layer) ++;
                
                DEBUG("Restart layer");
                m_offsets.back().first = 0;
                m_offsets.back().second ++;
                
                auto& loop_layer = getCurLayerEnt();
                assert(loop_layer.subpats.size());
                if( loop_layer.tok.type() != TOK_NULL ) {
                    DEBUG("- Separator token = " << loop_layer.tok);
                    return loop_layer.tok;
                }
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
        else
        {
            DEBUG("Terminate evaluation");
            m_offsets.pop_back();
            assert( m_offsets.size() == 0 );
        }
    } // while( m_offsets NONEMPTY )
    
    DEBUG("EOF");
    return Token(TOK_EOF);
}

/// Count the number of names at each layer
void MacroExpander::prep_counts()
{
    m_layer_iters.resize(m_mappings.layer_count(), 0);
}
const MacroRuleEnt& MacroExpander::getCurLayerEnt() const
{
    assert( m_offsets.size() > 1 );
    
    const ::std::vector<MacroRuleEnt>* ents = &m_root_contents;
    for( unsigned int i = 0; i < m_offsets.size()-2; i ++ )
    {
        unsigned int ofs = m_offsets[i].first;
        assert( ofs > 0 && ofs <= ents->size() );
        ents = &(*ents)[ofs-1].subpats;
    }
    return (*ents)[m_offsets[m_offsets.size()-2].first-1];
    
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

void Macro_Invoke_Concat_Once(::std::string& s, TokenStream& lex, enum eTokenType exp)
{
    Token   tok;
    GET_TOK(tok, lex);
    if( exp == TOK_STRING )
    {
        if( tok.type() == TOK_MACRO )
        {
            // Special case, expand both concat! and stringify! internally
            // TODO: Invoke
            auto tt = Parse_TT(lex, false);
            auto slex = Macro_Invoke(lex, tok.str(), tt);
            Macro_Invoke_Concat_Once(s, (*slex), exp);
            GET_CHECK_TOK(tok, (*slex), TOK_EOF);
        }
        else if( tok.type() == exp )
        {
            s += tok.str();
        }
        else
        {
            CHECK_TOK(tok, exp);
        }
    }
    else
    {
        CHECK_TOK(tok, exp);
        s += tok.str();
    }
}

::std::unique_ptr<TokenStream> Macro_Invoke_Concat(const TokenTree& input, enum eTokenType exp)
{
    Token   tok;
    TTStream    lex(input);
    ::std::string   val;
    do
    {
        Macro_Invoke_Concat_Once(val, lex, exp);
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
    return Position("MacroToken", 0);
}
Token MacroToken::realGetToken()
{
    Token ret = m_tok;
    m_tok = Token(TOK_EOF);
    return ret;
}
MacroStringify::MacroStringify(const TokenTree& input)
{
    Token   tok;
    TTStream    lex(input);
    
    ::std::string   rv;
    while( GET_TOK(tok, lex) != TOK_EOF )
    {
        rv += tok.to_str();
        rv += " ";
    }
    
    m_tok = Token(TOK_STRING, ::std::move(rv));
}
Position MacroStringify::getPosition() const
{
    return Position("Stringify", 0);
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
    _(PAT_PAT);
    _(PAT_TYPE);
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
    _(PAT_PAT);
    _(PAT_TYPE);
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
