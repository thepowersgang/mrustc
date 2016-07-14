/*
 */
#include <common.hpp>
#include "macro_rules.hpp"
#include <parse/parseerror.hpp>
#include <parse/tokentree.hpp>
#include <parse/common.hpp>
#include <limits.h>
#include "pattern_checks.hpp"
#include <parse/interpolated_fragment.hpp>

extern AST::ExprNodeP Parse_ExprBlockNode(TokenStream& lex);
extern AST::ExprNodeP Parse_Stmt(TokenStream& lex);

class ParameterMappings
{
    TAGGED_UNION_EX(CaptureLayer, (), Vals, (
        (Vals, ::std::vector<InterpolatedFragment>),
        (Nested, ::std::vector<CaptureLayer>)
    ),
    (),
    (),
    (
    public:
        CaptureLayer& next_layer_or_self(unsigned int idx) {
            TU_IFLET(CaptureLayer, (*this), Nested, e,
                return e.at(idx);
            )
            else {
                return *this;
            }
        }
        friend ::std::ostream& operator<<(::std::ostream& os, const CaptureLayer& x) {
            TU_MATCH(CaptureLayer, (x), (e),
            (Vals,
                os << "[" << e << "]";
                ),
            (Nested,
                os << "{" << e << "}";
                )
            )
            return os;
        }
    )
    );
    
    /// Represents the value 
    struct CapturedVar
    {
        CaptureLayer    top_layer;
        
        friend ::std::ostream& operator<<(::std::ostream& os, const CapturedVar& x) {
            os << "CapturedVar { top_layer: " << x.top_layer << " }";
            return os;
        }
        
    };
    
    ::std::vector<CapturedVar>  m_mappings;
    unsigned m_layer_count;
public:
    ParameterMappings():
        m_layer_count(0)
    {
    }
    ParameterMappings(ParameterMappings&&) = default;
    
    const ::std::vector<CapturedVar>& mappings() const { return m_mappings; }
    
    void dump() const {
        DEBUG("m_mappings = {" << m_mappings << "}");
    }
    
    size_t layer_count() const {
        return m_layer_count+1;
    }
    
    void insert(unsigned int name_index, const ::std::vector<unsigned int>& iterations, InterpolatedFragment data) {
        if( name_index >= m_mappings.size() ) {
            m_mappings.resize( name_index + 1 );
        }
        auto* layer = &m_mappings[name_index].top_layer;
        if( iterations.size() > 0 )
        {
            for(unsigned int i = 0; i < iterations.size()-1; i ++ )
            {
                auto iter = iterations[i];
                
                if( layer->is_Vals() ) {
                    assert( layer->as_Vals().size() == 0 );
                    *layer = CaptureLayer::make_Nested({});
                }
                auto& e = layer->as_Nested();
                while( e.size() < iter ) {
                    DEBUG("- Skipped iteration " << e.size());
                    e.push_back( CaptureLayer::make_Nested({}) );
                }
                
                if(e.size() == iter) {
                    e.push_back( CaptureLayer::make_Vals({}) );
                }
                else {
                    if( e.size() > iter ) {
                        DEBUG("ERROR: Iterations ran backwards?");
                    }
                }
                layer = &e[iter];
            }
            assert(layer->as_Vals().size() == iterations.back());
            layer->as_Vals().push_back( mv$(data) );
        }
        else {
            assert(layer->as_Vals().size() == 0);
            layer->as_Vals().push_back( mv$(data) );
        }
    }
    
    InterpolatedFragment* get(const ::std::vector<unsigned int>& iterations, unsigned int name_idx)
    {
        DEBUG("(iterations=[" << iterations << "], name_idx=" << name_idx << ")");
        auto& e = m_mappings.at(name_idx);
        //DEBUG("- e = " << e);
        auto* layer = &e.top_layer;
        
        // - If the top layer is a 1-sized set of values, unconditionally return
        TU_IFLET(CaptureLayer, (*layer), Vals, e,
            if( e.size() == 1 ) {
                return &e[0];
            }
        )
        
        for(const auto iter : iterations)
        {
            TU_MATCH(CaptureLayer, (*layer), (e),
            (Vals,
                return &e[iter];
                ),
            (Nested,
                layer = &e[iter];
                )
            )
        }

        ERROR(Span(), E0000, "Variable #" << name_idx << " is still repeating at this level (" << iterations.size() << ")");
    }
    unsigned int count_in(const ::std::vector<unsigned int>& iterations, unsigned int name_idx)
    {
        DEBUG("(iterations=[" << iterations << "], name_idx=" << name_idx << ")");
        if( name_idx >= m_mappings.size() ) {
            return 0;
        }
        auto& e = m_mappings.at(name_idx);
        auto* layer = &e.top_layer;
        for(const auto iter : iterations)
        {
            layer = &layer->next_layer_or_self(iter);
        }
        return layer->is_Vals() ? layer->as_Vals().size() : 0;
    }
};

class MacroExpander:
    public TokenStream
{
public:

private:
    const RcString  m_macro_filename;

    const ::std::string m_crate_name;
    const ::std::vector<MacroExpansionEnt>&  m_root_contents;
    ParameterMappings m_mappings;
    

    struct t_offset {
        unsigned read_pos;
        unsigned loop_index;
        unsigned max_index;
    };
    /// Layer states : Index and Iteration
    ::std::vector< t_offset >   m_offsets;
    ::std::vector< unsigned int>    m_iterations;
    
    /// Cached pointer to the current layer
    const ::std::vector<MacroExpansionEnt>*  m_cur_ents;  // For faster lookup.

    Token   m_next_token;   // used for inserting a single token into the stream
    ::std::unique_ptr<TTStream>  m_ttstream;
    
public:
    MacroExpander(const MacroExpander& x) = delete;
    
    MacroExpander(const ::std::string& macro_name, const ::std::vector<MacroExpansionEnt>& contents, ParameterMappings mappings, ::std::string crate_name):
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
    const MacroExpansionEnt& getCurLayerEnt() const;
    const ::std::vector<MacroExpansionEnt>* getCurLayer() const;
    void prep_counts();
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
        return LOOK_AHEAD(lex) == TOK_BRACE_OPEN || LOOK_AHEAD(lex) == TOK_INTERPOLATED_BLOCK;
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
        return LOOK_AHEAD(lex) == TOK_IDENT || LOOK_AHEAD(lex) == TOK_INTERPOLATED_META;
    }
    throw ParseError::Todo(lex, FMT("Macro_TryPattern : " << pat));
}

bool Macro_HandlePattern(TTStream& lex, const MacroPatEnt& pat, ::std::vector<unsigned int>& iterations, ParameterMappings& bound_tts)
{
    TRACE_FUNCTION_F("iterations = " << iterations);
    Token   tok;
    
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
        iterations.push_back(0);
        for(;;)
        {
            if( ! Macro_TryPattern(lex, pat.subpats[0]) )
            {
                DEBUG("break");
                break;
            }
            for( unsigned int i = 0; i < pat.subpats.size(); i ++ )
            {
                if( !Macro_HandlePattern(lex, pat.subpats[i], iterations, bound_tts) ) {
                    DEBUG("Ent " << i << " failed");
                    return false;
                }
            }
            match_count += 1;
            iterations.back() += 1;
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
        iterations.pop_back();
        DEBUG("Done (" << match_count << " matches)");
        break; }
    
    case MacroPatEnt::PAT_TT:
        DEBUG("TT");
        if( GET_TOK(tok, lex) == TOK_EOF )
            throw ParseError::Unexpected(lex, TOK_EOF);
        else
            PUTBACK(tok, lex);
        bound_tts.insert( pat.name_index, iterations, InterpolatedFragment( Parse_TT(lex, false) ) );
        break;
    case MacroPatEnt::PAT_PAT:
        bound_tts.insert( pat.name_index, iterations, InterpolatedFragment( Parse_Pattern(lex, true) ) );
        break;
    case MacroPatEnt::PAT_TYPE:
        bound_tts.insert( pat.name_index, iterations, InterpolatedFragment( Parse_Type(lex) ) );
        break;
    case MacroPatEnt::PAT_EXPR:
        bound_tts.insert( pat.name_index, iterations, InterpolatedFragment( InterpolatedFragment::EXPR, Parse_Expr0(lex).release() ) );
        break;
    case MacroPatEnt::PAT_STMT:
        bound_tts.insert( pat.name_index, iterations, InterpolatedFragment( InterpolatedFragment::STMT, Parse_Stmt(lex).release() ) );
        break;
    case MacroPatEnt::PAT_PATH:
        bound_tts.insert( pat.name_index, iterations, InterpolatedFragment( Parse_Path(lex, PATH_GENERIC_TYPE) ) );    // non-expr mode
        break;
    case MacroPatEnt::PAT_BLOCK:
        bound_tts.insert( pat.name_index, iterations, InterpolatedFragment( InterpolatedFragment::BLOCK, Parse_ExprBlockNode(lex).release() ) );
        break;
    case MacroPatEnt::PAT_META:
        bound_tts.insert( pat.name_index, iterations, InterpolatedFragment( Parse_MetaItem(lex) ) );
        break;
    case MacroPatEnt::PAT_IDENT:
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        bound_tts.insert( pat.name_index, iterations, InterpolatedFragment( TokenTree(tok) ) );
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
            ::std::vector<unsigned int> iterations;
            if( !Macro_HandlePattern(lex, pat, iterations, bound_tts) )
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
    
    DEBUG( rule.m_contents.size() << " rule contents with " << bound_tts.mappings().size() << " bound values - " << name );
    for( unsigned int i = 0; i < bound_tts.mappings().size(); i ++ )
    {
        DEBUG(" - " << rule.m_param_names[i] << " = [" << bound_tts.mappings()[i] << "]");
    }
    bound_tts.dump();
    
    DEBUG("TODO: Obtain crate name correctly, using \"\" for now");
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
            TU_MATCH( MacroExpansionEnt, (ent), (e),
            (Token,
                return e;
                ),
            (NamedValue,
                if( e >> 30 ) {
                    switch( e & 0x3FFFFFFF )
                    {
                    // - XXX: Hack for $crate special name
                    case 0:
                        DEBUG("Crate name hack");
                        if( m_crate_name != "" )
                        {
                            m_next_token = Token(TOK_STRING, m_crate_name);
                            return Token(TOK_DOUBLE_COLON);
                        }
                        break;
                    default:
                        BUG(Span(), "Unknown macro metavar");
                    }
                }
                else {
                    auto* frag = m_mappings.get(m_iterations, e);
                    if( !frag )
                    {
                        throw ParseError::Generic(*this, FMT("Cannot find '" << e << "' for " << m_iterations));
                    }
                    else
                    {
                        DEBUG("Insert replacement #" << e << " = " << *frag);
                        if( frag->m_type == InterpolatedFragment::TT )
                        {
                            m_ttstream.reset( new TTStream( frag->as_tt() ) );
                            return m_ttstream->getToken();
                        }
                        else
                        {
                            return Token( *frag );
                        }
                    }
                }
                ),
            (Loop,
                // 1. Get number of times this will repeat (based on the next iteration count)
                unsigned int num_repeats = 0;
                for(const auto idx : e.variables) {
                    unsigned int this_repeats = m_mappings.count_in(m_iterations, idx);
                    if( this_repeats > num_repeats )
                        num_repeats = this_repeats;
                }
                if( num_repeats > 0 )
                {
                    m_offsets.push_back( {0, 0, num_repeats} );
                    m_iterations.push_back( 0 );
                    m_cur_ents = getCurLayer();
                }
                )
            )
            // Fall through for loop
        }
        else if( layer > 0 )
        {
            // - Otherwise, restart/end loop and fall through
            DEBUG("layer = " << layer << ", m_iterations = " << m_iterations);
            auto& cur_ofs = m_offsets.back();
            DEBUG("Layer #" << layer << " Cur: " << cur_ofs.loop_index << ", Max: " << cur_ofs.max_index);
            if( cur_ofs.loop_index + 1 < cur_ofs.max_index )
            {
                m_iterations.back() ++;
                
                DEBUG("Restart layer");
                cur_ofs.read_pos = 0;
                cur_ofs.loop_index ++;
                
                auto& loop_layer = getCurLayerEnt();
                if( loop_layer.as_Loop().joiner.type() != TOK_NULL ) {
                    DEBUG("- Separator token = " << loop_layer.as_Loop().joiner);
                    return loop_layer.as_Loop().joiner;
                }
                // Fall through and restart layer
            }
            else
            {
                DEBUG("Terminate layer");
                // Terminate loop, fall through to lower layers
                m_offsets.pop_back();
                m_iterations.pop_back();
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
}
const MacroExpansionEnt& MacroExpander::getCurLayerEnt() const
{
    assert( m_offsets.size() > 1 );
    
    const auto* ents = &m_root_contents;
    for( unsigned int i = 0; i < m_offsets.size()-2; i ++ )
    {
        unsigned int ofs = m_offsets[i].read_pos;
        assert( ofs > 0 && ofs <= ents->size() );
        ents = &(*ents)[ofs-1].as_Loop().entries;
    }
    return (*ents)[m_offsets[m_offsets.size()-2].read_pos-1];
    
}
const ::std::vector<MacroExpansionEnt>* MacroExpander::getCurLayer() const
{
    assert( m_offsets.size() > 0 );
    const auto* ents = &m_root_contents;
    for( unsigned int i = 0; i < m_offsets.size()-1; i ++ )
    {
        unsigned int ofs = m_offsets[i].read_pos;
        //DEBUG(i << " ofs=" << ofs << " / " << ents->size());
        assert( ofs > 0 && ofs <= ents->size() );
        ents = &(*ents)[ofs-1].as_Loop().entries;
        //DEBUG("ents = " << ents);
    }
    return ents;
}
