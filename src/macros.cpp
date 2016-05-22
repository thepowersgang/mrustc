/*
 */
#include "common.hpp"
#include "macros.hpp"
#include "parse/parseerror.hpp"
#include "parse/tokentree.hpp"
#include "parse/common.hpp"
#include "ast/ast.hpp"
#include <limits.h>

typedef ::std::map< ::std::string, MacroRules>  t_macro_regs;

t_macro_regs g_macro_registrations;

class ParameterMappings
{
    // MultiMap (layer, name) -> TokenTree
    // - Multiple values are only allowed for layer>0
    typedef ::std::pair<unsigned, unsigned> t_mapping_block;
    struct Mapping
    {
        t_mapping_block block;
        ::std::vector<TokenTree>    entries;
        friend ::std::ostream& operator<<(::std::ostream& os, const Mapping& x) {
            os << "(" << x.block.first << ", " << x.block.second << "): '" << x.entries << "')";
            return os;
        }
    };
    struct less_cstr {
        bool operator()(const char *a, const char *b) const { return ::std::strcmp(a,b) < 0; }
    };
    
    typedef ::std::map<const char *, Mapping, less_cstr>    t_inner_map;
    
    t_inner_map m_inner;
    unsigned m_layer_count;
public:
    ParameterMappings():
        m_inner(),
        m_layer_count(0)
    {
    }
    ParameterMappings(ParameterMappings&&) = default;
    
    const t_inner_map& inner_() const {
        return m_inner;
    }
    
    size_t layer_count() const {
        return m_layer_count+1;
    }
    
    void insert(unsigned int layer, const char *name, TokenTree data) {
        if(layer > m_layer_count)
            m_layer_count = layer;
        auto v = m_inner.insert( ::std::make_pair( name, Mapping { {layer, 0}, {} } ) );
        if(v.first->second.block.first != layer) {
            throw ParseError::Generic(FMT("matching '"<<name<<"' at multiple layers"));
        }
        v.first->second.entries.push_back( mv$(data) );
    }
    
    const TokenTree* get(unsigned int layer, unsigned int iteration, const char *name, unsigned int idx) const
    {
        const auto it = m_inner.find( name );
        if( it == m_inner.end() ) {
            DEBUG("m_mappings = " << m_inner);
            return nullptr;
        }
        const auto& e = it->second;
        if( e.block.first < layer ) {
            DEBUG(name<<" higher layer (" << e.block.first << ")");
            return nullptr;
        }
        else if( e.block.first > layer ) {
            throw ParseError::Generic( FMT("'" << name << "' is still repeating at this layer") );
        }
        else if( idx >= e.entries.size() ) {
            DEBUG("Not enough mappings for name " << name << " at layer " << layer << " PI " << iteration);
            return nullptr;
        }
        else {
            DEBUG(name << " #" << idx << ": " << e.entries[idx]);
            return &e.entries[idx];
        }
    }
    unsigned int count(unsigned int layer, unsigned int iteration, const char *name) const
    {
        const auto it = m_inner.find( name );
        if( it == m_inner.end() ) {
            DEBUG("("<<layer<<","<<iteration<<",'"<<name<<"') not found m_mappings = " << m_inner);
            return 0;
        }
        const auto& e = it->second;
        
        if( e.block.first < layer ) {
            DEBUG(name<<" higher layer (" << e.block.first << ")");
            return UINT_MAX;
        }
        else if( e.block.first > layer ) {
            //throw ParseError::Generic( FMT("'" << name << "' is still repeating at this layer") );
            // ignore mismatch
            return UINT_MAX;
        }
        else {
            return it->second.entries.size();
        }
    }
};

class MacroExpander:
    public TokenStream
{
public:

private:
    const RcString  m_macro_filename;

    const ::std::string m_crate_name;
    const ::std::vector<MacroRuleEnt>&  m_root_contents;
    const ParameterMappings m_mappings;
    

    struct t_offset {
        unsigned read_pos;
        unsigned loop_index;
        unsigned max_index;
    };
    /// Layer states : Index and Iteration
    ::std::vector< t_offset >   m_offsets;
    ::std::vector< size_t > m_layer_iters;
    
    /// Cached pointer to the current layer
    const ::std::vector<MacroRuleEnt>*  m_cur_ents;  // For faster lookup.

    Token   m_next_token;   // used for inserting a single token into the stream
    ::std::unique_ptr<TTStream>   m_ttstream;
    
public:
    MacroExpander(const MacroExpander& x) = delete;
    //MacroExpander(const MacroExpander& x):
    //    m_macro_name( x.m_macro_name ),
    //    m_crate_name(x.m_crate_name),
    //    m_root_contents(x.m_root_contents),
    //    m_mappings(x.m_mappings),
    //    m_offsets({ {0,0,0} }),
    //    m_cur_ents(&m_root_contents)
    //{
    //    prep_counts();
    //}
    MacroExpander(const ::std::string& macro_name, const ::std::vector<MacroRuleEnt>& contents, ParameterMappings mappings, ::std::string crate_name):
        m_macro_filename( FMT("Macro:" << macro_name) ),
        m_crate_name( mv$(crate_name) ),
        m_root_contents(contents),
        m_mappings( mv$(mappings) ),
        m_offsets({ {0,0,0} }),
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
    unsigned int count_repeats(const ::std::vector<MacroRuleEnt>& ents, unsigned layer, unsigned iter);
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
            PUTBACK(tok, lex);
            return false;
        }
        else {
            PUTBACK(tok, lex);
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
    case MacroPatEnt::PAT_META:
        return LOOK_AHEAD(lex) == TOK_IDENT;
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
            match_count += 1;
            DEBUG("succ");
            if( pat.tok.type() != TOK_NULL )
            {
                if( GET_TOK(tok, lex) != pat.tok.type() )
                {
                    lex.putback( mv$(tok) );
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
            PUTBACK(tok, lex);
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
    case MacroPatEnt::PAT_META:
        val = Parse_TT_Meta(lex);
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

::std::unique_ptr<TokenStream> Macro_InvokeRules(const char *name, const MacroRules& rules, const TokenTree& input)
{
    TRACE_FUNCTION;
    
    // TODO: Use types in `parse/macro_rules.cpp` to evaluate
    
    // 2. Check input token tree against possible variants
    // 3. Bind names
    // 4. Return expander
    int i = 0;
    for(const auto& rule : rules.m_rules)
    {
        Token   tok;
        // Create token stream for input tree
        TTStream    lex(input);
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
            
            DEBUG("TODO: Obtain crate name correctly");
            TokenStream* ret_ptr = new MacroExpander(name, rule.m_contents, mv$(bound_tts), "");
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
    throw ParseError::Todo(/*source_span, */"Error when macro fails to match");
}


Position MacroExpander::getPosition() const
{
    return Position(m_macro_filename, 0, m_offsets[0].read_pos);
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
        size_t idx = m_offsets.back().read_pos++;
        
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
                DEBUG("lookup '" << ent.name << "'");
                const TokenTree* tt;
                unsigned int search_layer = layer;
                do {
                    unsigned int parent_iter = (search_layer > 0 ? m_layer_iters.at(search_layer-1) : 0);
                    const size_t iter_idx = m_offsets.at(search_layer).loop_index;
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
                unsigned int num_repeats = this->count_repeats(ent.subpats, layer+1, layer_iter);
                if(num_repeats == UINT_MAX)
                    num_repeats = 0;
                // New layer
                DEBUG("- NL = " << layer+1 << ", count = " << num_repeats );
                if( num_repeats > 0 )
                {
                    // - Push an offset
                    m_offsets.push_back( {0, 0, num_repeats} );
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
            auto& cur_ofs = m_offsets.back();
            DEBUG("Layer #" << layer << " Cur: " << cur_ofs.loop_index << ", Max: " << cur_ofs.max_index);
            if( cur_ofs.loop_index + 1 < cur_ofs.max_index )
            {
                m_layer_iters.at(layer) ++;
                
                DEBUG("Restart layer");
                cur_ofs.read_pos = 0;
                cur_ofs.loop_index ++;
                
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
        unsigned int ofs = m_offsets[i].read_pos;
        assert( ofs > 0 && ofs <= ents->size() );
        ents = &(*ents)[ofs-1].subpats;
    }
    return (*ents)[m_offsets[m_offsets.size()-2].read_pos-1];
    
}
const ::std::vector<MacroRuleEnt>* MacroExpander::getCurLayer() const
{
    assert( m_offsets.size() > 0 );
    const ::std::vector<MacroRuleEnt>* ents = &m_root_contents;
    for( unsigned int i = 0; i < m_offsets.size()-1; i ++ )
    {
        unsigned int ofs = m_offsets[i].read_pos;
        //DEBUG(i << " ofs=" << ofs << " / " << ents->size());
        assert( ofs > 0 && ofs <= ents->size() );
        ents = &(*ents)[ofs-1].subpats;
        //DEBUG("ents = " << ents);
    }
    return ents;
}
unsigned int MacroExpander::count_repeats(const ::std::vector<MacroRuleEnt>& ents, unsigned layer, unsigned iter)
{
    bool    valid = false;
    unsigned int count = 0;
    for(const auto& ent : ents)
    {
        if( ent.name != "" )
        {
            if( ent.name[0] == '*' ) {
                // Ignore meta-vars, they don't repeat
            }
            else {
                auto c = m_mappings.count(layer, iter, ent.name.c_str());
                DEBUG(c << " mappings for " << ent.name << " at (" << layer << ", " << iter << ")");
                if( c == UINT_MAX ) {
                    // Ignore
                }
                else if(!valid || count == c) {
                    count = c;
                    valid = true;
                }
                else {
                    // Mismatch!
                    throw ParseError::Generic("count_repeats - iteration count mismatch");
                }
            }
        }
        else if( ent.subpats.size() > 0 )
        {
            auto c = this->count_repeats(ent.subpats, layer, iter);
            if( c == UINT_MAX ) {
            }
            else if(!valid || count == c) {
                count = c;
                valid = true;
            }
            else {
                // Mismatch!
                throw ParseError::Generic("count_repeats - iteration count mismatch (subpat)");
            }
        }
        else
        {
            // Don't care
        }
    }
    
    if(valid) {
        return count;
    }
    else {
        return UINT_MAX;
    }
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
            //auto tt = Parse_TT(lex, false);
            //auto slex = Macro_Invoke(lex, tok.str(), tt);
            //Macro_Invoke_Concat_Once(s, (*slex), exp);
            //GET_CHECK_TOK(tok, (*slex), TOK_EOF);
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
    return Position("MacroToken", 0, 0);
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
    return Position("Stringify", 0,0);
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
    _(PAT_META);
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
    _(PAT_META);
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
::std::ostream& operator<<(::std::ostream& os, const MacroPatEnt& x)
{
    switch(x.type)
    {
    case MacroPatEnt::PAT_TOKEN: os << "=" << x.tok; break;
    case MacroPatEnt::PAT_LOOP:  os << "loop w/ "  << x.tok << " [" << x.subpats << "]";  break;
    default:
        os << "$" << x.name << ":";
        switch(x.type)
        {
        case MacroPatEnt::PAT_TOKEN: throw "";
        case MacroPatEnt::PAT_LOOP:  throw "";
        case MacroPatEnt::PAT_TT:    os << "tt";    break;
        case MacroPatEnt::PAT_PAT:   os << "pat";   break;
        case MacroPatEnt::PAT_IDENT: os << "ident"; break;
        case MacroPatEnt::PAT_PATH:  os << "path";  break;
        case MacroPatEnt::PAT_TYPE:  os << "type";  break;
        case MacroPatEnt::PAT_EXPR:  os << "expr";  break;
        case MacroPatEnt::PAT_STMT:  os << "stmt";  break;
        case MacroPatEnt::PAT_BLOCK: os << "block"; break;
        case MacroPatEnt::PAT_META:  os << "meta"; break;
        }
        break;
    }
    return os;
}

SERIALISE_TYPE_S(MacroRuleEnt, {
    s.item(name);
    s.item(tok);
    s.item(subpats);
});

::std::ostream& operator<<(::std::ostream& os, const MacroRuleEnt& x)
{
    if(x.name.size())
        os << "$"<<x.name;
    else if( x.subpats.size() )
        os << "expand w/ " << x.tok << " [" << x.subpats << "]";
    else
        os << "=" << x.tok;
    return os;
}

SERIALISE_TYPE_S(MacroRules, {
    s.item( m_exported );
    s.item( m_rules );
});

