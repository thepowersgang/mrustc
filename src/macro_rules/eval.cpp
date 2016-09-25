/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * macro_rules/parse.cpp
 * - macro_rules! evaluation (expansion)
 */
#include <common.hpp>
#include "macro_rules.hpp"
#include <parse/parseerror.hpp>
#include <parse/tokentree.hpp>
#include <parse/common.hpp>
#include <limits.h>
#include "pattern_checks.hpp"
#include <parse/interpolated_fragment.hpp>
#include <ast/expr.hpp>

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
            ASSERT_BUG(Span(), layer->as_Vals().size() == iterations.back(), "Capture count mismatch with iteration index - iterations=[" << iterations << "]");
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
            TU_MATCH(CaptureLayer, (*layer), (e),
            (Vals,
                return 0;
                ),
            (Nested,
                layer = &e[iter];
                )
            )
        }
        TU_MATCH(CaptureLayer, (*layer), (e),
        (Vals,
            return e.size();
            ),
        (Nested,
            return e.size();
            )
        )
        return 0;
    }
};

/// Simple pattern entry for macro_rules! arm patterns
TAGGED_UNION( SimplePatEnt, End,
    // End of the pattern stream
    (End, struct{}),
    // Expect a specific token
    (ExpectTok, Token),
    // Expect a pattern match
    (ExpectPat, struct {
        MacroPatEnt::Type   type;
        unsigned int    idx;
        }),
    // Compare the head of the input stream and poke the pattern stream
    (IfTok, struct {
        bool is_equal;
        Token   tok;
        }),
    // Compare the head of the input stream and poke the pattern stream
    (IfPat, struct {
        bool is_equal;
        MacroPatEnt::Type   type;
        })
    );
class MacroPatternStream
{
    const ::std::vector<MacroPatEnt>*   m_pattern;
    // Position in each nested pattern
    ::std::vector<unsigned int> m_pos;
    // Iteration index of each active loop level
    ::std::vector<unsigned int> m_loop_iterations;
    
    ::std::vector<SimplePatEnt> m_stack;
public:
    MacroPatternStream(const ::std::vector<MacroPatEnt>& pattern):
        m_pattern(&pattern),
        m_pos({0})
    {
    }
    
    /// Get the next pattern entry
    SimplePatEnt next();
    /// Inform the stream that the `if` rule that was just returned succeeded
    void if_succeeded();
    /// Get the current loop iteration count
    const ::std::vector<unsigned int>& get_loop_iters() const {
        return m_loop_iterations;
    }
    
private:
    SimplePatEnt emit_loop_start(const MacroPatEnt& pat);
};

SimplePatEnt MacroPatternStream::next()
{
    TRACE_FUNCTION_F("m_pos=[" << m_pos << "], m_stack.size()=" << m_stack.size());
    assert(m_pos.size() >= 1);
    
    // Pop off the generation stack
    if( ! m_stack.empty() ) {
        auto rv = mv$(m_stack.back());
        m_stack.pop_back();
        return rv;
    }
    
    /*
    if( m_break_if_not && ! m_condition_fired ) {
        // Break out of the current loop then continue downwards.
    }
    */
    
    const MacroPatEnt*  parent_pat = nullptr;
    const auto* ents = m_pattern;
    for(unsigned int i = 0; i < m_pos.size() - 1; i ++)
    {
        auto idx = m_pos[i];
        //DEBUG(i << " idx=" << idx << " ents->size()=" << ents->size());
        assert( idx < ents->size() );
        assert( (*ents)[idx].type == MacroPatEnt::PAT_LOOP );
        parent_pat = &(*ents)[idx];
        ents = &parent_pat->subpats;
    }
    
    DEBUG( (m_pos.size()-1) << " " << m_pos.back() << " / " << ents->size());
    if( m_pos.back() < ents->size() )
    {
        const auto& pat = ents->at( m_pos.back() );
        
        if( pat.type == MacroPatEnt::PAT_LOOP ) {
            DEBUG("Enter " << pat);
            // Increase level, return entry control
            m_pos.push_back( 0 );
            m_loop_iterations.push_back( 0 );
            
            if( pat.name == "*" )
            {
                return emit_loop_start(pat);
            }
            else
            {
                // If the name is "+" then this is should always be entered, so just recurse
                assert( pat.name == "+" );
                return next();
            }
        }
        else if( pat.type == MacroPatEnt::PAT_TOKEN ) {
            m_pos.back() += 1;
            return SimplePatEnt::make_ExpectTok( pat.tok.clone() );
        }
        else {
            m_pos.back() += 1;
            return SimplePatEnt::make_ExpectPat({ pat.type, pat.name_index });
        }
    }
    else
    {
        if( parent_pat )
        {
            // Last entry in a loop - return the breakout control
            // - Reset the loop back to the start
            m_pos.back() = 0;
            m_loop_iterations.back() += 1;
            
            // - Emit break conditions
            if( parent_pat->tok == TOK_NULL ) {
                // Loop separator is TOK_NULL - get the first token of the loop and use it.
                // - This shares the code that controls if a loop is entered
                return emit_loop_start(*parent_pat);
            }
            else {
                // - Yeild `IF NOT sep BREAK` and `EXPECT sep`
                m_stack.push_back( SimplePatEnt::make_ExpectTok( parent_pat->tok.clone() ) );
                return SimplePatEnt::make_IfTok({ false, parent_pat->tok.clone() });
            }
        }
        else
        {
            // End of the input sequence
            return SimplePatEnt::make_End({});
        }
    }
}
/// Returns (and primes m_stack) the rules to control the start of a loop
/// This code emits rules to break out of the loop if the entry conditions are not met
SimplePatEnt MacroPatternStream::emit_loop_start(const MacroPatEnt& pat)
{
    // Find the next non-loop pattern to control if this loop should be entered
    const auto* entry_pat = &pat.subpats.at(0);
    while( entry_pat->type == MacroPatEnt::PAT_LOOP ) {
        entry_pat = &entry_pat->subpats.at(0);
    }
    // - TODO: What if there's multiple tokens that can be used to enter the loop?
    //  > `$( $(#[...])* foo)*` should enter based on `#` and `foo`
    //  > Requires returning multiple controllers and requiring that at least one succeed

    // Emit an if based on it
    if( entry_pat->type == MacroPatEnt::PAT_TOKEN )
        return SimplePatEnt::make_IfTok({ false, entry_pat->tok.clone() });
    else
        return SimplePatEnt::make_IfPat({ false, entry_pat->type });
}

void MacroPatternStream::if_succeeded()
{
    // Break out of an active loop (pop level and increment parent level)
    assert( m_pos.size() >= 1 );
    // - This should never be called when on the top level
    assert( m_pos.size() != 1 );
    
    // HACK: Clear the stack if an if succeeded
    m_stack.clear();
    
    m_pos.pop_back();
    m_pos.back() += 1;
    
    m_loop_iterations.pop_back();
}

// ----------------------------------------------------------------
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

bool Macro_TryPatternCap(/*const*/ TTStream& lex, MacroPatEnt::Type type)
{
    switch(type)
    {
    case MacroPatEnt::PAT_TOKEN:
        BUG(lex.getPosition(), "");
    case MacroPatEnt::PAT_LOOP:
        BUG(lex.getPosition(), "");
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
    case MacroPatEnt::PAT_ITEM:
        return is_token_item( LOOK_AHEAD(lex) );
    }
    BUG(lex.getPosition(), "");
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
    default:
        return Macro_TryPatternCap(lex, pat.type);
    }
}

void Macro_HandlePatternCap(TTStream& lex, unsigned int index, MacroPatEnt::Type type, const ::std::vector<unsigned int>& iterations, ParameterMappings& bound_tts)
{
    Token   tok;
    switch(type)
    {
    case MacroPatEnt::PAT_TOKEN:
        BUG(lex.getPosition(), "");
    case MacroPatEnt::PAT_LOOP:
        BUG(lex.getPosition(), "");
    
    case MacroPatEnt::PAT_TT:
        DEBUG("TT");
        if( GET_TOK(tok, lex) == TOK_EOF )
            throw ParseError::Unexpected(lex, TOK_EOF);
        else
            PUTBACK(tok, lex);
        bound_tts.insert( index, iterations, InterpolatedFragment( Parse_TT(lex, false) ) );
        break;
    case MacroPatEnt::PAT_PAT:
        bound_tts.insert( index, iterations, InterpolatedFragment( Parse_Pattern(lex, true) ) );
        break;
    case MacroPatEnt::PAT_TYPE:
        bound_tts.insert( index, iterations, InterpolatedFragment( Parse_Type(lex) ) );
        break;
    case MacroPatEnt::PAT_EXPR:
        bound_tts.insert( index, iterations, InterpolatedFragment( InterpolatedFragment::EXPR, Parse_Expr0(lex).release() ) );
        break;
    case MacroPatEnt::PAT_STMT:
        bound_tts.insert( index, iterations, InterpolatedFragment( InterpolatedFragment::STMT, Parse_Stmt(lex).release() ) );
        break;
    case MacroPatEnt::PAT_PATH:
        bound_tts.insert( index, iterations, InterpolatedFragment( Parse_Path(lex, PATH_GENERIC_TYPE) ) );    // non-expr mode
        break;
    case MacroPatEnt::PAT_BLOCK:
        bound_tts.insert( index, iterations, InterpolatedFragment( InterpolatedFragment::BLOCK, Parse_ExprBlockNode(lex).release() ) );
        break;
    case MacroPatEnt::PAT_META:
        bound_tts.insert( index, iterations, InterpolatedFragment( Parse_MetaItem(lex) ) );
        break;
    case MacroPatEnt::PAT_ITEM:
        bound_tts.insert( index, iterations, InterpolatedFragment( Parse_Mod_Item_S(lex, false, "!", lex.parse_state().module->path(), false, AST::MetaItems{}) ) );
        break;
    case MacroPatEnt::PAT_IDENT:
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        bound_tts.insert( index, iterations, InterpolatedFragment( TokenTree(tok) ) );
        break;
    }
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
    
    default:
        Macro_HandlePatternCap(lex, pat.name_index, pat.type, iterations, bound_tts);
        break;
    }
    return true;
}

/// Parse the input TokenTree according to the `macro_rules!` patterns and return a token stream of the replacement
::std::unique_ptr<TokenStream> Macro_InvokeRules(const char *name, const MacroRules& rules, const TokenTree& input, AST::Module& mod)
{
    TRACE_FUNCTION;
    Span    sp;// = input
    
    // - List of active rules (rules that haven't yet failed)
    ::std::vector< ::std::pair<unsigned, MacroPatternStream> > active_arms;
    active_arms.reserve( rules.m_rules.size() );
    for(unsigned int i = 0; i < rules.m_rules.size(); i ++)
    {
        active_arms.push_back( ::std::make_pair(i, MacroPatternStream(rules.m_rules[i].m_pattern)) );
    }
    
    ParameterMappings   bound_tts;
    unsigned int    rule_index;
    
    TTStream    lex(input);
    SET_MODULE(lex, mod);
    while(true)
    {
        // 1. Get concrete patterns for all active rules (i.e. no If* patterns)
        ::std::vector<SimplePatEnt> arm_pats;
        for(auto& arm : active_arms)
        {
            SimplePatEnt pat;
            // Consume all If* rules 
            do
            {
                pat = arm.second.next();
                TU_IFLET( SimplePatEnt, pat, IfPat, e,
                    DEBUG("IfPat(" << e.is_equal << " ?" << e.type << ")");
                    if( Macro_TryPatternCap(lex, e.type) == e.is_equal )
                    {
                        DEBUG("- Succeeded");
                        arm.second.if_succeeded();
                    }
                )
                else TU_IFLET( SimplePatEnt, pat, IfTok, e,
                    DEBUG("IfTok(" << e.is_equal << " ?" << e.tok << ")");
                    auto tok = lex.getToken();
                    if( (tok == e.tok) == e.is_equal )
                    {
                        DEBUG("- Succeeded");
                        arm.second.if_succeeded();
                    }
                    lex.putback( mv$(tok) );
                )
                else {
                    break;
                }
            } while( pat.is_IfPat() || pat.is_IfTok() );
            arm_pats.push_back( mv$(pat) );
        }
        assert( arm_pats.size() == active_arms.size() );
        
        // 2. Prune imposible arms
        for(unsigned int i = 0; i < arm_pats.size(); )
        {
            const auto& pat = arm_pats[i];
            bool fail = false;
            
            TU_MATCH( SimplePatEnt, (pat), (e),
            (IfPat, BUG(sp, "IfTok unexpected here");),
            (IfTok, BUG(sp, "IfTok unexpected here");),
            (ExpectTok,
                DEBUG(i << " ExpectTok(" << e << ")");
                auto tok = lex.getToken();
                fail = !(tok == e);
                lex.putback( mv$(tok) );
                ),
            (ExpectPat,
                DEBUG(i << " ExpectPat(" << e.type << " => $" << e.idx << ")");
                fail = !Macro_TryPatternCap(lex, e.type);
                ),
            (End,
                DEBUG(i << " End");
                fail = !(lex.lookahead(0) == TOK_EOF);
                )
            )
            if( fail ) {
                DEBUG("- Failed arm " << active_arms[i].first);
                arm_pats.erase( arm_pats.begin() + i );
                active_arms.erase( active_arms.begin() + i );
            }
            else {
                i ++;
            }
        }
        
        if( arm_pats.size() == 0 ) {
            ERROR(sp, E0000, "No rules expected " << lex.getToken());
        }
        // 3. Check that all remaining arms are the same pattern.
        for(unsigned int i = 1; i < arm_pats.size(); i ++)
        {
            if( arm_pats[0].tag() != arm_pats[i].tag() ) {
                ERROR(lex.getPosition(), E0000, "Incompatible macro arms - " << arm_pats[0].tag_str() << " vs " << arm_pats[i].tag_str());
            }
            TU_MATCH( SimplePatEnt, (arm_pats[0], arm_pats[i]), (e1, e2),
            (IfPat, BUG(sp, "IfPat unexpected here");),
            (IfTok, BUG(sp, "IfTok unexpected here");),
            (ExpectTok,
                // NOTE: This should never fail.
                if( e1 != e2 ) {
                    ERROR(lex.getPosition(), E0000, "Incompatible macro arms - mismatched token expectation " << e1 << " vs " << e2);
                }
                ),
            (ExpectPat,
                // Can fail, as :expr and :stmt overlap in their trigger set
                if( e1.type != e2.type ) {
                    ERROR(lex.getPosition(), E0000, "Incompatible macro arms - mismatched patterns");
                }
                if( e1.idx != e2.idx ) {
                    ERROR(lex.getPosition(), E0000, "Incompatible macro arms - mismatched pattern bindings " << e1.idx << " and " << e2.idx);
                }
                ),
            (End,
                )
            )
        }
        // 4. Apply patterns.

        // - Check for an end rule outside of the match so it can break correctly.
        if( arm_pats[0].is_End() ) {
            auto tok = lex.getToken();
            if( tok.type() != TOK_EOF ) {
                ERROR(lex.getPosition(), E0000, "Unexpected " << tok << ", expected TOK_EOF");
            }
            // NOTE: There can be multiple arms active, take the first.
            rule_index = active_arms[0].first;
            break ;
        }
        TU_MATCH( SimplePatEnt, (arm_pats[0]), (e),
        (IfPat, BUG(sp, "IfPat unexpected here");),
        (IfTok, BUG(sp, "IfTok unexpected here");),
        (ExpectTok,
            auto tok = lex.getToken();
            if( tok != e ) {
                ERROR(lex.getPosition(), E0000, "Unexpected " << tok << ", expected " << e);
            }
            ),
        (ExpectPat,
            // NOTE: This is going to fail somewhere, but need to determine what to do when it does
            for( unsigned int i = 1; i < active_arms.size(); i ++ ) {
                if( active_arms[0].second.get_loop_iters() != active_arms[i].second.get_loop_iters() ) {
                    TODO(sp, "ExpectPat with mismatched loop iterations");
                }
            }
            Macro_HandlePatternCap(lex, e.idx, e.type,  active_arms[0].second.get_loop_iters(),  bound_tts);
            ),
        (End,
            BUG(sp, "SimplePatEnt::End unexpected here");
            )
        )
        
        // Keep looping - breakout is handled by an if above
    }
    
    
    const auto& rule = rules.m_rules.at(rule_index);
    
    DEBUG( rule.m_contents.size() << " rule contents with " << bound_tts.mappings().size() << " bound values - " << name );
    assert( rule.m_param_names.size() >= bound_tts.mappings().size() );
    for( unsigned int i = 0; i < bound_tts.mappings().size(); i ++ )
    {
        DEBUG(" - " << rule.m_param_names.at(i) << " = [" << bound_tts.mappings()[i] << "]");
    }
    //bound_tts.dump();
    
    TokenStream* ret_ptr = new MacroExpander(name, rule.m_contents, mv$(bound_tts), rules.m_source_crate);
    
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
                DEBUG("Looping " << num_repeats << " times");
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
