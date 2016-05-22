/*
 */
#include <common.hpp>
#include "macro_rules.hpp"
#include <parse/parseerror.hpp>
#include <parse/tokentree.hpp>
#include <parse/common.hpp>
#include <limits.h>
#include "pattern_checks.hpp"

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
    
    
    ::std::vector<Mapping>  m_mappings;
    unsigned m_layer_count;
    
    //::std::map<const char*, Mapping*, less_cstr>    m_map;
    ::std::map<::std::string, Mapping*>    m_map;
public:
    ParameterMappings():
        m_layer_count(0)
    {
    }
    ParameterMappings(ParameterMappings&&) = default;
    
    const ::std::vector<Mapping>& mappings() const { return m_mappings; }
    
    void dump() const {
        DEBUG("m_mappings = {" << m_mappings << "}");
        DEBUG("m_map = {" << m_map << "}");
    }
    
    size_t layer_count() const {
        return m_layer_count+1;
    }
    
    void insert(unsigned int layer, unsigned int name_index, TokenTree data) {
        if(layer > m_layer_count)
            m_layer_count = layer;
        
        if( name_index >= m_mappings.size() ) {
            m_mappings.resize( name_index + 1 );
        }
        auto& mapping = m_mappings[name_index];
        
        if( mapping.entries.size() == 0 ) {
            mapping.block.first = layer;
        }
        else if( mapping.block.first != layer) {
            throw ParseError::Generic(FMT("matching argument #'"<<name_index<<"' at multiple layers"));
        }
        else {
        }
        mapping.entries.push_back( mv$(data) );
    }
    
    void reindex(const ::std::vector< ::std::string>& new_names) {
        m_map.clear();
        
        if( new_names.size() < m_mappings.size() ) {
            BUG(Span(), "Macro parameter name mismatch - len["<<new_names<<"] != len["<<m_mappings<<"]");
        }
        for( unsigned int i = 0; i < m_mappings.size(); i ++ )
        {
            m_map.insert( ::std::make_pair(new_names[i].c_str(), &m_mappings[i]) );
        }
    }    

    const TokenTree* get(unsigned int layer, unsigned int iteration, const char *name, unsigned int idx) const
    {
        const auto it = m_map.find( name );
        if( it == m_map.end() ) {
            DEBUG("m_map = " << m_map);
            return nullptr;
        }
        const auto& e = *it->second;
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
        const auto it = m_map.find( name );
        if( it == m_map.end() ) {
            DEBUG("("<<layer<<","<<iteration<<",'"<<name<<"') not found - m_map={" << m_map << "}");
            return 0;
        }
        const auto& e = *it->second;
        
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
            return e.entries.size();
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

void Macro_InitDefaults()
{
}

bool Macro_TryPattern(TTStream& lex, const MacroPatEnt& pat)
{
    DEBUG("pat = " << pat);
    Token   tok;
    switch(pat.type)
    {
    case MacroPatEnt::PAT_TOKEN: {
        GET_TOK(tok, lex);
        bool rv = (tok == pat.tok);
        PUTBACK(tok, lex);
        return rv;
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
        return is_token_path( LOOK_AHEAD(lex) );
    case MacroPatEnt::PAT_TYPE:
        return is_token_type( LOOK_AHEAD(lex) );
    case MacroPatEnt::PAT_EXPR:
        return is_token_expr( LOOK_AHEAD(lex) );
    case MacroPatEnt::PAT_STMT:
        return is_token_stmt( LOOK_AHEAD(lex) );
    case MacroPatEnt::PAT_PAT:
        return is_token_pat( LOOK_AHEAD(lex) );
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
        bound_tts.insert( layer, pat.name_index, ::std::move(val) );
        break;
    
    //default:
    //    throw ParseError::Todo("full macro pattern matching");
    }
    return true;
}

::std::unique_ptr<TokenStream> Macro_InvokeRules(const char *name, const MacroRules& rules, const TokenTree& input)
{
    TRACE_FUNCTION;
    
    const auto* cur_frag = &rules.m_pattern;
    unsigned int    cur_frag_ofs = 0;
    
    ParameterMappings   bound_tts;
    unsigned int    rule_index;
    
    TTStream    lex(input);
    while(true)
    {
        // If not at the end of the fragment, handle that pattern
        if( cur_frag_ofs < cur_frag->m_pats_ents.size() )
        {
            const auto& pat = cur_frag->m_pats_ents[cur_frag_ofs];
            DEBUG("- try " << pat);
            if( !Macro_HandlePattern(lex, pat, 0, bound_tts) )
                throw ParseError::Generic(lex, "Macro pattern failed");
            // Keep going
            cur_frag_ofs ++;
        }
        else
        {
            // The stream has ended
            if( LOOK_AHEAD(lex) == TOK_EOF ) {
                // Check if an end is expected here
                if( cur_frag->m_pattern_end == ~0u ) {
                    Token   tok = lex.getToken();
                    ERROR(tok.get_pos(), E0000, "Unexpected end of macro invocation - " << cur_frag_ofs << " != len [" << cur_frag->m_pats_ents << "]");
                }
                // We've found the rule!
                rule_index = cur_frag->m_pattern_end;
                break;
            }
            
            // Search for which path to take
            for(const auto& next : cur_frag->m_next_frags) {
                assert(next.m_pats_ents.size() > 0);
                if( Macro_TryPattern(lex, next.m_pats_ents.front()) ) {
                    cur_frag = &next;
                    cur_frag_ofs = 0;
                    goto continue_;
                }
            }
            
            // No paths matched - error out
            {
                ::std::stringstream expected;
                for(const auto& next : cur_frag->m_next_frags) {
                    expected << next.m_pats_ents.front() << ", ";
                }
                
                Token   tok = lex.getToken();
                ERROR(tok.get_pos(), E0000, "Unexpected token in macro invocation - " << tok << " - expected " << expected.str());
            }
            
        continue_:
            (void)0;
        }
    }
    
    const auto& rule = rules.m_rules[rule_index];
    
    DEBUG( rule.m_contents.size() << " rule contents bound to " << bound_tts.mappings().size() << " values - " << name );
    bound_tts.reindex( rule.m_param_names );
    for( unsigned int i = 0; i < bound_tts.mappings().size(); i ++ )
    {
        DEBUG(" - " << rule.m_param_names[i] << " = [" << bound_tts.mappings()[i] << "]");
    }
    bound_tts.dump();
    
    DEBUG("TODO: Obtain crate name correctly");
    TokenStream* ret_ptr = new MacroExpander(name, rule.m_contents, mv$(bound_tts), "");
    // HACK! Disable nested macro expansion
    //ret_ptr->parse_state().no_expand_macros = true;
    
    return ::std::unique_ptr<TokenStream>( ret_ptr );
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

