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
    /// A particular captured fragment
    struct CapturedVal
    {
        unsigned int    num_uses;   // Number of times this var will be used
        unsigned int    num_used;   // Number of times it has been used
        InterpolatedFragment    frag;
    };
    
    /// A single layer of the capture set
    TAGGED_UNION(CaptureLayer, Vals,
        (Vals, ::std::vector<CapturedVal>),
        (Nested, ::std::vector<CaptureLayer>)
        );
    
    /// Represents the fragments captured for a name
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
    
    void insert(unsigned int name_index, const ::std::vector<unsigned int>& iterations, InterpolatedFragment data);
    
    InterpolatedFragment* get(const ::std::vector<unsigned int>& iterations, unsigned int name_idx);
    unsigned int count_in(const ::std::vector<unsigned int>& iterations, unsigned int name_idx) const;
    
    /// Increment the number of times a particular fragment will be used
    void inc_count(const ::std::vector<unsigned int>& iterations, unsigned int name_idx);
    /// Decrement the number of times a particular fragment is used (returns true if there are still usages remaining)
    bool dec_count(const ::std::vector<unsigned int>& iterations, unsigned int name_idx);
    
    
    friend ::std::ostream& operator<<(::std::ostream& os, const CapturedVal& x) {
        os << x.frag;
        return os;
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
    
private:
    CapturedVal& get_cap(const ::std::vector<unsigned int>& iterations, unsigned int name_idx);
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
    unsigned int    m_skip_count;

    bool m_break_if_not = false;
    bool m_condition_fired = false;
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
    
    SimplePatEnt emit_seq(SimplePatEnt v1, SimplePatEnt v2) {
        assert( m_stack.empty() );
        m_stack.push_back( mv$(v2) );
        return v1;
    }
    
    void break_loop();
};

// === Prototypes ===
unsigned int Macro_InvokeRules_MatchPattern(const MacroRules& rules, TokenTree input, AST::Module& mod,  ParameterMappings& bound_tts);
void Macro_InvokeRules_CountSubstUses(ParameterMappings& bound_tts, const ::std::vector<MacroExpansionEnt>& contents);

// ------------------------------------
// ParameterMappings
// ------------------------------------

void ParameterMappings::insert(unsigned int name_index, const ::std::vector<unsigned int>& iterations, InterpolatedFragment data)
{
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
                    DEBUG("ERROR: Iterations ran backwards? - " << e.size() << " > " << iter);
                }
            }
            layer = &e[iter];
        }
        ASSERT_BUG(Span(), layer->as_Vals().size() == iterations.back(), "Capture count mismatch with iteration index - iterations=[" << iterations << "]");
    }
    else {
        assert(layer->as_Vals().size() == 0);
    }
    layer->as_Vals().push_back( CapturedVal { 0,0, mv$(data) } );
}

ParameterMappings::CapturedVal& ParameterMappings::get_cap(const ::std::vector<unsigned int>& iterations, unsigned int name_idx)
{
    DEBUG("(iterations=[" << iterations << "], name_idx=" << name_idx << ")");
    auto& e = m_mappings.at(name_idx);
    //DEBUG("- e = " << e);
    auto* layer = &e.top_layer;
    
    // - If the top layer is a 1-sized set of values, unconditionally return it
    TU_IFLET(CaptureLayer, (*layer), Vals, e,
        if( e.size() == 1 ) {
            return e[0];
        }
    )
    
    for(const auto iter : iterations)
    {
        TU_MATCH(CaptureLayer, (*layer), (e),
        (Vals,
            return e[iter];
            ),
        (Nested,
            layer = &e[iter];
            )
        )
    }

    ERROR(Span(), E0000, "Variable #" << name_idx << " is still repeating at this level (" << iterations.size() << ")");
}

InterpolatedFragment* ParameterMappings::get(const ::std::vector<unsigned int>& iterations, unsigned int name_idx)
{
    return &get_cap(iterations, name_idx).frag;
}
unsigned int ParameterMappings::count_in(const ::std::vector<unsigned int>& iterations, unsigned int name_idx) const
{
    DEBUG("(iterations=[" << iterations << "], name_idx=" << name_idx << ")");
    if( name_idx >= m_mappings.size() ) {
        DEBUG("- Missing");
        return 0;
    }
    auto& e = m_mappings.at(name_idx);
    auto* layer = &e.top_layer;
    for(const auto iter : iterations)
    {
        TU_MATCH(CaptureLayer, (*layer), (e),
        (Vals,
            // TODO: Returning zero here isn't correct, maybe 1 will be?
            return 1;
            ),
        (Nested,
            if( iter >= e.size() ) {
                DEBUG("Counting value for an iteration index it doesn't have - " << iter << " >= " << e.size());
                return 0;
            }
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
void ParameterMappings::inc_count(const ::std::vector<unsigned int>& iterations, unsigned int name_idx)
{
    auto& cap = get_cap(iterations, name_idx);
    assert(cap.num_used == 0);
    cap.num_uses += 1;
}
bool ParameterMappings::dec_count(const ::std::vector<unsigned int>& iterations, unsigned int name_idx)
{
    auto& cap = get_cap(iterations, name_idx);
    assert(cap.num_used < cap.num_uses);
    cap.num_used += 1;
    return (cap.num_used < cap.num_uses);
}

// ------------------------------------
// MacroPatternStream
// ------------------------------------

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
    
    if( m_break_if_not && ! m_condition_fired ) {
        // Break out of the current loop then continue downwards.
        break_loop();
    }
    
    m_skip_count = 0;
    m_break_if_not = false;
    m_condition_fired = false;
    
    const MacroPatEnt*  parent_pat = nullptr;
    decltype(m_pattern) parent_ents = nullptr;
    const auto* ents = m_pattern;
    for(unsigned int i = 0; i < m_pos.size() - 1; i ++)
    {
        auto idx = m_pos[i];
        //DEBUG(i << " idx=" << idx << " ents->size()=" << ents->size());
        assert( idx < ents->size() );
        assert( (*ents)[idx].type == MacroPatEnt::PAT_LOOP );
        parent_pat = &(*ents)[idx];
        parent_ents = ents;
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
                DEBUG("No separator");
                return emit_loop_start(*parent_pat);
            }
            else {
                // TODO: What if the next token after the end of the loop is the separator?
                // - Need to consume a bunch before deciding to commit to a path
                auto i = m_pos[ m_pos.size() - 2 ] + 1;
                if( i < parent_ents->size() )
                {
                    DEBUG("sep = " << parent_pat->tok << ", next = " << parent_ents->at(i) << ", start = " << ents->at(0));
                    if( parent_ents->at(i).type == MacroPatEnt::PAT_TOKEN && parent_pat->tok == parent_ents->at(i).tok && ents->at(0).type == MacroPatEnt::PAT_TOKEN )
                    {
                        DEBUG("MAGIC: Reverse conditions for case where sep==next");
                        //  > Mark to skip the next token after the end of the loop
                        m_skip_count = 1;
                        // - Yeild `EXPECT sep` then `IF NOT start BREAK`
                        return emit_seq(
                            SimplePatEnt::make_ExpectTok( parent_pat->tok.clone() ),
                            SimplePatEnt::make_IfTok({ false, ents->at(0).tok.clone() })
                            );
                    }
                }
                // - Yeild `IF NOT sep BREAK` and `EXPECT sep`
                DEBUG("Separator = " << parent_pat->tok);
                return emit_seq(
                    SimplePatEnt::make_IfTok({ false, parent_pat->tok.clone() }),
                    SimplePatEnt::make_ExpectTok( parent_pat->tok.clone() )
                    );
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
    ::std::vector<const MacroPatEnt*> m_entry_pats;
    
    const auto* parent_subpats = &pat.subpats;
    const auto* entry_pat = &pat.subpats.at(0);
    while( entry_pat->type == MacroPatEnt::PAT_LOOP ) {
        if( entry_pat->name == "*" ) {
            if( parent_subpats->size() > 1 ) {
                if( parent_subpats->at(1).type == MacroPatEnt::PAT_LOOP ) {
                    // TODO: Recurse here
                    // This case is either: Direct recurse, or recurse+next pattern (index 2 instead of 1)
                    DEBUG("TODO TODO: Handle case where an optional loop is folled by another loop.");
                }
                else {
                    m_entry_pats.push_back( &parent_subpats->at(1) );
                }
            }
            else {
                // Only entry, no second condtiion present
            }
        }
        else {
            // Always entered, so entry condition is the inner
        }
        parent_subpats = &entry_pat->subpats;
        entry_pat = &entry_pat->subpats.at(0);
    }
    // - TODO: What if there's multiple tokens that can be used to enter the loop?
    //  > `$( $(#[...])* foo)*` should enter based on `#` and `foo`
    //  > Requires returning multiple controllers and requiring that at least one succeed
    
    struct H {
        static SimplePatEnt get_if(bool flag, const MacroPatEnt& mpe) {
            if( mpe.type == MacroPatEnt::PAT_TOKEN )
                return SimplePatEnt::make_IfTok({ flag, mpe.tok.clone() });
            else
                return SimplePatEnt::make_IfPat({ flag, mpe.type });
        }
    };
    
    if( m_entry_pats.size() > 0 )
    {
        DEBUG("Multiple entry possibilities, reversing condition");
        m_break_if_not = true;
        for(auto pat_ptr : m_entry_pats)
        {
            m_stack.push_back( H::get_if(true, *pat_ptr) );
        }
        return H::get_if(true, *entry_pat);
    }
    else
    {
        // Emit an if based on it
        return H::get_if(false, *entry_pat);
    }
}

void MacroPatternStream::if_succeeded()
{
    if( m_break_if_not )
    {
        m_condition_fired = true;
    }
    else
    {
        break_loop();
    }
}
void MacroPatternStream::break_loop()
{
    DEBUG("- Break out of loop, m_skip_count = " << m_skip_count);
    // Break out of an active loop (pop level and increment parent level)
    assert( m_pos.size() >= 1 );
    // - This should never be called when on the top level
    assert( m_pos.size() != 1 );
    
    // HACK: Clear the stack if an if succeeded
    m_stack.clear();
    
    m_pos.pop_back();
    m_pos.back() += 1 + m_skip_count;
    
    m_loop_iterations.pop_back();
}

// ----------------------------------------------------------------
/// State for MacroExpander and Macro_InvokeRules_CountSubstUses
class MacroExpandState
{
    const ::std::vector<MacroExpansionEnt>&  m_root_contents;
    const ParameterMappings& m_mappings;

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
    
public:
    MacroExpandState(const ::std::vector<MacroExpansionEnt>& contents, const ParameterMappings& mappings):
        m_root_contents(contents),
        m_mappings(mappings),
        m_offsets({ {0,0,0} }),
        m_cur_ents(&m_root_contents)
    {
    }
    
    // Returns a pointer to the next entry to expand, or nullptr if the end is reached
    // - NOTE: When a Loop entry is returned, the separator token should be emitted
    const MacroExpansionEnt* next_ent();
    
    const ::std::vector<unsigned int>   iterations() const { return m_iterations; }
    unsigned int top_pos() const { return m_offsets[0].read_pos; }

private:
    const MacroExpansionEnt& getCurLayerEnt() const;
    const ::std::vector<MacroExpansionEnt>* getCurLayer() const;
};
// ----------------------------------------------------------------
class MacroExpander:
    public TokenStream
{
    const RcString  m_macro_filename;

    const ::std::string m_crate_name;
    
    ParameterMappings m_mappings;
    MacroExpandState    m_state;
    
    Token   m_next_token;   // used for inserting a single token into the stream
    ::std::unique_ptr<TTStreamO> m_ttstream;
    
public:
    MacroExpander(const MacroExpander& x) = delete;
    
    MacroExpander(const ::std::string& macro_name, const ::std::vector<MacroExpansionEnt>& contents, ParameterMappings mappings, ::std::string crate_name):
        m_macro_filename( FMT("Macro:" << macro_name) ),
        m_crate_name( mv$(crate_name) ),
        m_mappings( mv$(mappings) ),
        m_state( contents, m_mappings )
    {
    }

    virtual Position getPosition() const override;
    virtual Token realGetToken() override;
};

void Macro_InitDefaults()
{
}

bool Macro_TryPatternCap(TokenStream& lex, MacroPatEnt::Type type)
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
        switch(LOOK_AHEAD(lex))
        {
        case TOK_EOF:
        case TOK_PAREN_CLOSE:
        case TOK_BRACE_CLOSE:
        case TOK_SQUARE_CLOSE:
            return false;
        default:
            return true;
        }
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
bool Macro_TryPattern(TokenStream& lex, const MacroPatEnt& pat)
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

void Macro_HandlePatternCap(TokenStream& lex, unsigned int index, MacroPatEnt::Type type, const ::std::vector<unsigned int>& iterations, ParameterMappings& bound_tts)
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
    case MacroPatEnt::PAT_ITEM: {
        assert( lex.parse_state().module );
        const auto& cur_mod = *lex.parse_state().module;
        bound_tts.insert( index, iterations, InterpolatedFragment( Parse_Mod_Item_S(lex, cur_mod.m_file_info, cur_mod.path(), AST::MetaItems{}) ) );
        } break;
    case MacroPatEnt::PAT_IDENT:
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        bound_tts.insert( index, iterations, InterpolatedFragment( TokenTree(tok) ) );
        break;
    }
}
bool Macro_HandlePattern(TokenStream& lex, const MacroPatEnt& pat, ::std::vector<unsigned int>& iterations, ParameterMappings& bound_tts)
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
::std::unique_ptr<TokenStream> Macro_InvokeRules(const char *name, const MacroRules& rules, TokenTree input, AST::Module& mod)
{
    TRACE_FUNCTION_F("'" << name << "', " << input);
    
    ParameterMappings   bound_tts;
    unsigned int    rule_index = Macro_InvokeRules_MatchPattern(rules, mv$(input), mod,  bound_tts);
    
    const auto& rule = rules.m_rules.at(rule_index);
    
    DEBUG( rule.m_contents.size() << " rule contents with " << bound_tts.mappings().size() << " bound values - " << name );
    assert( rule.m_param_names.size() >= bound_tts.mappings().size() );
    for( unsigned int i = 0; i < bound_tts.mappings().size(); i ++ )
    {
        DEBUG("- #" << i << " " << rule.m_param_names.at(i) << " = [" << bound_tts.mappings()[i] << "]");
    }
    //bound_tts.dump();
    
    // Run through the expansion counting the number of times each fragment is used
    Macro_InvokeRules_CountSubstUses(bound_tts, rule.m_contents);
    
    TokenStream* ret_ptr = new MacroExpander(name, rule.m_contents, mv$(bound_tts), rules.m_source_crate);
    
    return ::std::unique_ptr<TokenStream>( ret_ptr );
}

unsigned int Macro_InvokeRules_MatchPattern(const MacroRules& rules, TokenTree input, AST::Module& mod,  ParameterMappings& bound_tts)
{
    TRACE_FUNCTION;
    Span    sp;// = input.span();
    
    unsigned int    rule_index;
    // - List of active rules (rules that haven't yet failed)
    ::std::vector< ::std::pair<unsigned, MacroPatternStream> > active_arms;
    active_arms.reserve( rules.m_rules.size() );
    for(unsigned int i = 0; i < rules.m_rules.size(); i ++)
    {
        active_arms.push_back( ::std::make_pair(i, MacroPatternStream(rules.m_rules[i].m_pattern)) );
    }
    
    TTStreamO   lex( mv$(input) );
    SET_MODULE(lex, mod);
    while(true)
    {
        DEBUG("--- ---");
        // 1. Get concrete patterns for all active rules (i.e. no If* patterns)
        ::std::vector<SimplePatEnt> arm_pats;
        for(auto& arm : active_arms)
        {
            auto idx = arm.first;
            SimplePatEnt pat;
            // Consume all If* rules 
            do
            {
                pat = arm.second.next();
                TU_IFLET( SimplePatEnt, pat, IfPat, e,
                    DEBUG(idx << " IfPat(" << (e.is_equal ? "==" : "!=") << " ?" << e.type << ")");
                    if( Macro_TryPatternCap(lex, e.type) == e.is_equal )
                    {
                        DEBUG("- Succeeded");
                        arm.second.if_succeeded();
                    }
                )
                else TU_IFLET( SimplePatEnt, pat, IfTok, e,
                    DEBUG(idx << " IfTok(" << (e.is_equal ? "==" : "!=") << " ?" << e.tok << ")");
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
            
            TU_MATCH( SimplePatEnt, (pat), (e),
            (IfPat, BUG(sp, "IfTok unexpected here");),
            (IfTok, BUG(sp, "IfTok unexpected here");),
            (ExpectTok,
                DEBUG(idx << " ExpectTok(" << e << ")");
                ),
            (ExpectPat,
                DEBUG(idx << " ExpectPat(" << e.type << " => $" << e.idx << ")");
                ),
            (End,
                DEBUG(idx << " End");
                )
            )
            arm_pats.push_back( mv$(pat) );
        }
        assert( arm_pats.size() == active_arms.size() );
        
        // 2. Prune imposible arms
        for(unsigned int i = 0, j = 0; i < arm_pats.size(); )
        {
            auto idx = active_arms[i].first;
            const auto& pat = arm_pats[i];
            bool fail = false;
            
            TU_MATCH( SimplePatEnt, (pat), (e),
            (IfPat, BUG(sp, "IfTok unexpected here");),
            (IfTok, BUG(sp, "IfTok unexpected here");),
            (ExpectTok,
                auto tok = lex.getToken();
                DEBUG(j<<"="<<idx << " ExpectTok(" << e << ") == " << tok);
                fail = !(tok == e);
                lex.putback( mv$(tok) );
                ),
            (ExpectPat,
                DEBUG(j<<"="<<idx << " ExpectPat(" << e.type << " => $" << e.idx << ")");
                fail = !Macro_TryPatternCap(lex, e.type);
                ),
            (End,
                DEBUG(j<<"="<<idx << " End");
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
            j ++;
        }
        
        if( arm_pats.size() == 0 ) {
            auto tok = lex.getToken();
            ERROR(tok.get_pos(), E0000, "No rules expected " << tok);
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
            struct H {
                static bool is_prefix(const ::std::vector<unsigned>& needle, const ::std::vector<unsigned>& haystack) {
                    if( needle.size() > haystack.size() ) {
                        return false;
                    }
                    else {
                        for(unsigned int i = 0; i < needle.size(); i ++) {
                            if(needle[i] != haystack[i])
                                return false;
                        }
                        return true;
                    }
                }
            };
            
            // Use the shortest (and ensure that it's a prefix to the others) and let the capture code move caps around when needed
            const auto* longest = &active_arms[0].second.get_loop_iters();
            const auto* shortest = longest;
            for( unsigned int i = 1; i < active_arms.size(); i ++ ) {
                const auto& iters2 = active_arms[i].second.get_loop_iters();
                // If this arm has a deeper tree,
                if( iters2.size() > longest->size() ) {
                    // The existing longest must be a prefix to this
                    if( !H::is_prefix(*longest, iters2) ) {
                        TODO(sp, "Handle ExpectPat where iteration counts aren't prefixes - [" << *longest << "] vs [" << iters2 << "]");
                    }
                    longest = &iters2;
                }
                else {
                    // Keep track of the shortest
                    if( iters2.size() < shortest->size() ) {
                        shortest = &iters2;
                    }
                    
                    // This must be a prefix to the longest
                    if( !H::is_prefix(iters2, *longest) ) {
                        TODO(sp, "Handle ExpectPat where iteration counts aren't prefixes - [" << *longest << "] vs [" << iters2 << "]");
                    }
                }
            }
            
            // Use the shallowest iteration state
            // TODO: All other should be on the first iteration.
            Macro_HandlePatternCap(lex, e.idx, e.type,  *shortest,  bound_tts);
            ),
        (End,
            BUG(sp, "SimplePatEnt::End unexpected here");
            )
        )
        
        // Keep looping - breakout is handled by an if above
    }
    
    return rule_index;
}

void Macro_InvokeRules_CountSubstUses(ParameterMappings& bound_tts, const ::std::vector<MacroExpansionEnt>& contents)
{
    TRACE_FUNCTION;
    MacroExpandState    state(contents, bound_tts);
    
    while(const auto* ent_ptr = state.next_ent())
    {
        DEBUG(*ent_ptr);
        TU_IFLET(MacroExpansionEnt, (*ent_ptr), NamedValue, e,
            if( e >> 30 ) {
            }
            else {
                // Increment a counter in `bound_tts`
                bound_tts.inc_count(state.iterations(), e);
            }
        )
    }
}

Position MacroExpander::getPosition() const
{
    // TODO: Return a far better span - invocaion location?
    return Position(m_macro_filename, 0, m_state.top_pos());
}
Token MacroExpander::realGetToken()
{
    // Use m_next_token first
    if( m_next_token.type() != TOK_NULL )
    {
        DEBUG("m_next_token = " << m_next_token);
        return mv$(m_next_token);
    }
    // Then try m_ttstream
    if( m_ttstream.get() )
    {
        DEBUG("TTStream present");
        Token rv = m_ttstream->getToken();
        if( rv.type() != TOK_EOF )
            return rv;
        m_ttstream.reset();
    }
    
    // Loop to handle case where $crate expands to nothing
    while( const auto* next_ent_ptr = m_state.next_ent() )
    {
        const auto& ent = *next_ent_ptr;
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
                auto* frag = m_mappings.get(m_state.iterations(), e);
                ASSERT_BUG(this->getPosition(), frag, "Cannot find '" << e << "' for " << m_state.iterations());
                
                bool can_steal = ( m_mappings.dec_count(m_state.iterations(), e) == false );
                DEBUG("Insert replacement #" << e << " = " << *frag);
                if( frag->m_type == InterpolatedFragment::TT )
                {
                    if( can_steal )
                    {
                        m_ttstream.reset( new TTStreamO( mv$(frag->as_tt()) ) );
                    }
                    else
                    {
                        m_ttstream.reset( new TTStreamO( frag->as_tt().clone() ) );
                    }
                    return m_ttstream->getToken();
                }
                else
                {
                    if( can_steal )
                    {
                        return Token(Token::TagTakeIP(), mv$(*frag) );
                    }
                    else
                    {
                        // Clones
                        return Token( *frag );
                    }
                }
            }
            ),
        (Loop,
            //assert( e.joiner.tok() != TOK_NULL );
            return e.joiner;
            )
        )
    }
    
    DEBUG("EOF");
    return Token(TOK_EOF);
}

const MacroExpansionEnt* MacroExpandState::next_ent()
{
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
                return &ent;
                ),
            (NamedValue,
                return &ent;
                ),
            (Loop,
                // 1. Get number of times this will repeat (based on the next iteration count)
                unsigned int num_repeats = 0;
                for(const auto& var : e.variables)
                {
                    unsigned int this_repeats = m_mappings.count_in(m_iterations, var.first);
                    DEBUG("= " << this_repeats);
                    // If a variable doesn't have data and it's a required controller, don't loop
                    if( this_repeats == 0 && var.second ) {
                        num_repeats = 0;
                        break;
                    }
                    // TODO: Ideally, all variables would have the same repeat count.
                    // Options: 0 (optional), 1 (higher), N (all equal)
                    if( this_repeats > num_repeats )
                        num_repeats = this_repeats;
                }
                DEBUG("Looping " << num_repeats << " times based on {" << e.variables << "}");
                // 2. If it's going to repeat, start the loop
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
                    return &loop_layer;
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
    
    return nullptr;
}

const MacroExpansionEnt& MacroExpandState::getCurLayerEnt() const
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
const ::std::vector<MacroExpansionEnt>* MacroExpandState::getCurLayer() const
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
