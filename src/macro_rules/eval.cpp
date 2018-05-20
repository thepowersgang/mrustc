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
#include <parse/ttstream.hpp>
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

    SimplePatEnt    m_peek_cache;
    bool m_peek_cache_valid = false;

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

    const SimplePatEnt& peek() {
        if( !m_peek_cache_valid ) {
            m_peek_cache = next();
            m_peek_cache_valid = true;
        }
        return m_peek_cache;
    }

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
unsigned int Macro_InvokeRules_MatchPattern(const Span& sp, const MacroRules& rules, TokenTree input, AST::Module& mod,  ParameterMappings& bound_tts);
void Macro_InvokeRules_CountSubstUses(ParameterMappings& bound_tts, const ::std::vector<MacroExpansionEnt>& contents);

// ------------------------------------
// ParameterMappings
// ------------------------------------

void ParameterMappings::insert(unsigned int name_index, const ::std::vector<unsigned int>& iterations, InterpolatedFragment data)
{
    DEBUG("index="<<name_index << ", iterations=[" << iterations << "], data="<<data);
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
        if( e.size() == 0 ) {
            BUG(Span(), "Attempting to get binding for empty capture - #" << name_idx);
        }
    )

    for(const auto iter : iterations)
    {
        TU_MATCH(CaptureLayer, (*layer), (e),
        (Vals,
            ASSERT_BUG(Span(), iter < e.size(), "Iteration index " << iter << " outside of range " << e.size() << " (values)");
            return e.at(iter);
            ),
        (Nested,
            ASSERT_BUG(Span(), iter < e.size(), "Iteration index " << iter << " outside of range " << e.size() << " (nest)");
            layer = &e.at(iter);
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
    if( e.top_layer.is_Vals() && e.top_layer.as_Vals().size() == 0 ) {
        DEBUG("- Not populated");
        return 0;
    }
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

    if( m_peek_cache_valid ) {
        m_peek_cache_valid = false;
        return mv$(m_peek_cache);
    }

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
                // If the next token is the same as the separator emit: Expect(separator), ShouldEnter

                auto i = m_pos[ m_pos.size() - 2 ] + 1;
                if( i < parent_ents->size() )
                {
                    DEBUG("sep = " << parent_pat->tok << ", next = " << parent_ents->at(i) << ", start = " << ents->at(0));
                    if( parent_ents->at(i).type == MacroPatEnt::PAT_TOKEN && parent_pat->tok == parent_ents->at(i).tok )
                    {
                        DEBUG("MAGIC: Reverse conditions for case where sep==next");
                        //  > Mark to skip the next token after the end of the loop
                        m_skip_count = 1;
                        // - Yeild `EXPECT sep` then the entry condition of this loop
                        auto pat = emit_loop_start(*parent_pat);
                        m_stack.push_back( mv$(pat) );
                        return SimplePatEnt::make_ExpectTok( parent_pat->tok.clone() );
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

namespace {
    void get_loop_entry_pats(const MacroPatEnt& pat,  ::std::vector<const MacroPatEnt*>& entry_pats)
    {
        assert( pat.type == MacroPatEnt::PAT_LOOP );

        // If this pattern is a loop, get the entry concrete patterns for it
        // - Otherwise, just
        unsigned int i = 0;
        while( i < pat.subpats.size() && pat.subpats[i].type == MacroPatEnt::PAT_LOOP )
        {
            const auto& cur_pat = pat.subpats[i];
            bool is_optional = (cur_pat.name == "*");

            get_loop_entry_pats(cur_pat, entry_pats);

            if( !is_optional )
            {
                // Non-optional loop, MUST be entered, so return after recursing
                return ;
            }
            // Optional, so continue the loop.
            i ++;
        }

        // First non-loop pattern
        if( i < pat.subpats.size() )
        {
            entry_pats.push_back( &pat.subpats[i] );
        }
    }
} // namespace

/// Returns (and primes m_stack) the rules to control the start of a loop
/// This code emits rules to break out of the loop if the entry conditions are not met
SimplePatEnt MacroPatternStream::emit_loop_start(const MacroPatEnt& pat)
{
    // Find the next non-loop pattern to control if this loop should be entered
    ::std::vector<const MacroPatEnt*> m_entry_pats;

    get_loop_entry_pats(pat, m_entry_pats);
    DEBUG("m_entry_pats = [" << FMT_CB(ss, for(const auto* p : m_entry_pats) { ss << *p << ","; }) << "]");

    struct H {
        static SimplePatEnt get_if(bool flag, const MacroPatEnt& mpe) {
            if( mpe.type == MacroPatEnt::PAT_TOKEN )
                return SimplePatEnt::make_IfTok({ flag, mpe.tok.clone() });
            else
                return SimplePatEnt::make_IfPat({ flag, mpe.type });
        }
    };

    const auto* entry_pat = m_entry_pats.back();
    m_entry_pats.pop_back();
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
    unsigned int top_pos() const { if(m_offsets.empty()) return 0; return m_offsets[0].read_pos; }

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
    ::std::shared_ptr<Span> m_invocation_span;

    ParameterMappings m_mappings;
    MacroExpandState    m_state;

    Token   m_next_token;   // used for inserting a single token into the stream
    ::std::unique_ptr<TTStreamO> m_ttstream;
    Ident::Hygiene  m_hygiene;

public:
    MacroExpander(const MacroExpander& x) = delete;

    MacroExpander(const ::std::string& macro_name, const Span& sp, const Ident::Hygiene& parent_hygiene, const ::std::vector<MacroExpansionEnt>& contents, ParameterMappings mappings, ::std::string crate_name):
        m_macro_filename( FMT("Macro:" << macro_name) ),
        m_crate_name( mv$(crate_name) ),
        m_invocation_span( new Span(sp) ),
        m_mappings( mv$(mappings) ),
        m_state( contents, m_mappings ),
        m_hygiene( Ident::Hygiene::new_scope_chained(parent_hygiene) )
    {
    }

    Position getPosition() const override;
    ::std::shared_ptr<Span> outerSpan() const override;
    Ident::Hygiene realGetHygiene() const override;
    Token realGetToken() override;
};

void Macro_InitDefaults()
{
}

namespace {
    bool is_reserved_word(eTokenType tok)
    {
        return tok >= TOK_RWORD_PUB;
    }
}

// TODO: This shouldn't exist, and can false-positives
// - Ideally, this would use consume_from_frag (which takes a clone-able input)
bool Macro_TryPatternCap(TokenStream& lex, MacroPatEnt::Type type)
{
    switch(type)
    {
    case MacroPatEnt::PAT_TOKEN:
        BUG(lex.point_span(), "");
    case MacroPatEnt::PAT_LOOP:
        BUG(lex.point_span(), "");
    case MacroPatEnt::PAT_BLOCK:
        return LOOK_AHEAD(lex) == TOK_BRACE_OPEN || LOOK_AHEAD(lex) == TOK_INTERPOLATED_BLOCK;
    case MacroPatEnt::PAT_IDENT:
        return LOOK_AHEAD(lex) == TOK_IDENT || is_reserved_word(LOOK_AHEAD(lex));
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
        return is_token_item( LOOK_AHEAD(lex) ) || LOOK_AHEAD(lex) == TOK_IDENT;
    }
    BUG(lex.point_span(), "Fell through");
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

InterpolatedFragment Macro_HandlePatternCap(TokenStream& lex, MacroPatEnt::Type type)
{
    Token   tok;
    switch(type)
    {
    case MacroPatEnt::PAT_TOKEN:
        BUG(lex.point_span(), "Encountered PAT_TOKEN when handling capture");
    case MacroPatEnt::PAT_LOOP:
        BUG(lex.point_span(), "Encountered PAT_LOOP when handling capture");

    case MacroPatEnt::PAT_TT:
        if( GET_TOK(tok, lex) == TOK_EOF )
            throw ParseError::Unexpected(lex, TOK_EOF);
        else
            PUTBACK(tok, lex);
        return InterpolatedFragment( Parse_TT(lex, false) );
    case MacroPatEnt::PAT_PAT:
        return InterpolatedFragment( Parse_Pattern(lex, true) );
    case MacroPatEnt::PAT_TYPE:
        return InterpolatedFragment( Parse_Type(lex) );
    case MacroPatEnt::PAT_EXPR:
        return InterpolatedFragment( InterpolatedFragment::EXPR, Parse_Expr0(lex).release() );
    case MacroPatEnt::PAT_STMT:
        return InterpolatedFragment( InterpolatedFragment::STMT, Parse_Stmt(lex).release() );
    case MacroPatEnt::PAT_PATH:
        return InterpolatedFragment( Parse_Path(lex, PATH_GENERIC_TYPE) );    // non-expr mode
    case MacroPatEnt::PAT_BLOCK:
        return InterpolatedFragment( InterpolatedFragment::BLOCK, Parse_ExprBlockNode(lex).release() );
    case MacroPatEnt::PAT_META:
        return InterpolatedFragment( Parse_MetaItem(lex) );
    case MacroPatEnt::PAT_ITEM: {
        assert( lex.parse_state().module );
        const auto& cur_mod = *lex.parse_state().module;
        return InterpolatedFragment( Parse_Mod_Item_S(lex, cur_mod.m_file_info, cur_mod.path(), AST::AttributeList{}) );
        } break;
    case MacroPatEnt::PAT_IDENT:
        // NOTE: Any reserved word is also valid as an ident
        GET_TOK(tok, lex);
        if( tok.type() == TOK_IDENT || is_reserved_word(tok.type()) )
            ;
        else
            CHECK_TOK(tok, TOK_IDENT);
        // TODO: TOK_INTERPOLATED_IDENT
        return InterpolatedFragment( TokenTree(lex.getHygiene(), tok) );
    }
    throw "";
}

/// Parse the input TokenTree according to the `macro_rules!` patterns and return a token stream of the replacement
::std::unique_ptr<TokenStream> Macro_InvokeRules(const char *name, const MacroRules& rules, const Span& sp, TokenTree input, AST::Module& mod)
{
    TRACE_FUNCTION_F("'" << name << "', " << input);

    ParameterMappings   bound_tts;
    unsigned int    rule_index = Macro_InvokeRules_MatchPattern(sp, rules, mv$(input), mod,  bound_tts);

    const auto& rule = rules.m_rules.at(rule_index);

    DEBUG( rule.m_contents.size() << " rule contents with " << bound_tts.mappings().size() << " bound values - " << name );
    for( unsigned int i = 0; i < ::std::min( bound_tts.mappings().size(), rule.m_param_names.size() ); i ++ )
    {
        DEBUG("- #" << i << " " << rule.m_param_names.at(i) << " = [" << bound_tts.mappings()[i] << "]");
    }
    //bound_tts.dump();

    // Run through the expansion counting the number of times each fragment is used
    Macro_InvokeRules_CountSubstUses(bound_tts, rule.m_contents);

    TokenStream* ret_ptr = new MacroExpander(name, sp, rules.m_hygiene, rule.m_contents, mv$(bound_tts), rules.m_source_crate);

    return ::std::unique_ptr<TokenStream>( ret_ptr );
}

#if 1
// Collection of functions that consume a specific fragment type from a token stream
// - Does very loose consuming
namespace
{
    // Class that provides read-only iteration over a TokenTree
    class TokenStreamRO
    {
        const TokenTree& m_tt;
        ::std::vector<size_t>   m_offsets;
        size_t  m_active_offset;

        Token m_faked_next;
        size_t  m_consume_count;
    public:
        TokenStreamRO(const TokenTree& tt):
            m_tt(tt),
            m_active_offset(0),
            m_consume_count(0)
        {
            assert( ! m_tt.is_token() );
            if( m_tt.size() == 0 )
            {
                m_active_offset = 0;
                DEBUG("TOK_EOF");
            }
            else
            {
                const auto* cur_tree = &m_tt;
                while( !cur_tree->is_token() )
                {
                    cur_tree = &(*cur_tree)[0];
                    m_offsets.push_back(0);
                }
                assert(m_offsets.size() > 0);
                m_offsets.pop_back();
                m_active_offset = 0;
                DEBUG(next_tok());
            }
        }
        TokenStreamRO clone() const {
            return TokenStreamRO(*this);
        }

        enum eTokenType next() const {
            return next_tok().type();
        }
        const Token& next_tok() const {
            static Token    eof_token = TOK_EOF;

            if( m_faked_next.type() != TOK_NULL )
            {
                return m_faked_next;
            }

            if( m_offsets.empty() && m_active_offset == m_tt.size() )
            {
                //DEBUG(m_consume_count << " " << eof_token << "(EOF)");
                return eof_token;
            }
            else
            {
                const auto* cur_tree = &m_tt;
                for(auto idx : m_offsets)
                    cur_tree = &(*cur_tree)[idx];
                const auto& rv = (*cur_tree)[m_active_offset].tok();
                //DEBUG(m_consume_count << " " << rv);
                return rv;
            }
        }
        void consume()
        {
            if( m_faked_next.type() != TOK_NULL )
            {
                m_faked_next = Token(TOK_NULL);
                return ;
            }

            if( m_offsets.empty() && m_active_offset == m_tt.size() )
                throw ::std::runtime_error("Attempting to consume EOS");
            DEBUG(m_consume_count << " " << next_tok());
            m_consume_count ++;
            for(;;)
            {
                const auto* cur_tree = &m_tt;
                for(auto idx : m_offsets)
                    cur_tree = &(*cur_tree)[idx];

                m_active_offset ++;
                // If reached the end of a tree...
                if(m_active_offset == cur_tree->size())
                {
                    // If the end of the root is reached, return (leaving the state indicating EOS)
                    if( m_offsets.empty() )
                        return ;
                    // Pop and continue
                    m_active_offset = m_offsets.back();
                    m_offsets.pop_back();
                }
                else
                {
                    // Dig into nested trees
                    while( !(*cur_tree)[m_active_offset].is_token() )
                    {
                        cur_tree = &(*cur_tree)[m_active_offset];
                        m_offsets.push_back(m_active_offset);
                        m_active_offset = 0;
                    }
                    DEBUG("-> " << next_tok());
                    return ;
                }
            }
        }
        void consume_and_push(eTokenType ty)
        {
            consume();
            m_faked_next = Token(ty);
        }

        // Consumes if the current token is `ty`, otherwise doesn't and returns false
        bool consume_if(eTokenType ty)
        {
            if(next() == ty) {
                consume();
                return true;
            }
            else {
                return false;
            }
        }

        /// Returns the position in the stream (number of tokens that have been consumed)
        size_t position() const {
            return m_consume_count;
        }
    };

    // Consume an entire TT
    bool consume_tt(TokenStreamRO& lex)
    {
        TRACE_FUNCTION;
        switch(lex.next())
        {
        case TOK_EOF:
        case TOK_PAREN_CLOSE:
        case TOK_BRACE_CLOSE:
        case TOK_SQUARE_CLOSE:
            return false;
        case TOK_PAREN_OPEN:
            lex.consume();
            while(lex.next() != TOK_PAREN_CLOSE)
                consume_tt(lex);
            lex.consume();
            break;
        case TOK_SQUARE_OPEN:
            lex.consume();
            while(lex.next() != TOK_SQUARE_CLOSE)
                consume_tt(lex);
            lex.consume();
            break;
        case TOK_BRACE_OPEN:
            lex.consume();
            while(lex.next() != TOK_BRACE_CLOSE)
                consume_tt(lex);
            lex.consume();
            break;
        default:
            lex.consume();
            break;
        }
        return true;
    }
    bool consume_tt_angle(TokenStreamRO& lex)
    {
        TRACE_FUNCTION;
        unsigned int level = (lex.next() == TOK_DOUBLE_LT ? 2 : 1);
        // Seek until enouh matching '>'s are seen
        // TODO: Can expressions show up on this context?
        lex.consume();
        for(;;)
        {
            if( lex.next() == TOK_LT || lex.next() == TOK_DOUBLE_LT )
            {
                level += (lex.next() == TOK_DOUBLE_LT ? 2 : 1);
            }
            else if( lex.next() == TOK_GT || lex.next() == TOK_DOUBLE_GT )
            {
                assert(level > 0);
                if( lex.next() == TOK_DOUBLE_GT )
                {
                    if( level == 1 )
                    {
                        lex.consume_and_push(TOK_GT);
                        return true;
                    }
                    level -= 2;
                }
                else
                {
                    level -= 1;
                }
                if( level == 0 )
                    break;
            }
            else if( lex.next() == TOK_EOF )
            {
                return false;
            }
            else
            {
            }

            // Consume TTs separately
            if( lex.next() == TOK_PAREN_OPEN )
            {
                consume_tt(lex);
            }
            else
            {
                lex.consume();
            }
        }
        // Consume closing token
        lex.consume();
        return true;
    }
    // Consume a path
    bool consume_path(TokenStreamRO& lex, bool type_mode=false)
    {
        TRACE_FUNCTION;
        switch(lex.next())
        {
        case TOK_INTERPOLATED_PATH:
            lex.consume();
            return true;
        case TOK_RWORD_SELF:
            lex.consume();
            // Allow a lone `self` (it's referring to the current object)
            if( lex.next() != TOK_DOUBLE_COLON )
                return true;
            break;
        case TOK_RWORD_SUPER:
            lex.consume();
            if( lex.next() != TOK_DOUBLE_COLON )
                return false;
            break;
        case TOK_DOUBLE_COLON:
            break;
        case TOK_IDENT:
            lex.consume();
            if( type_mode && (lex.next() == TOK_LT || lex.next() == TOK_DOUBLE_LT) )
                ;
            // Allow a lone ident
            else if( lex.next() != TOK_DOUBLE_COLON )
                return true;
            else
                ;
            break;
        case TOK_LT:
        case TOK_DOUBLE_LT:
            if( !consume_tt_angle(lex) )
                return false;
            if( lex.next() != TOK_DOUBLE_COLON )
                return false;
            break;
        default:
            return false;
        }

        if( type_mode && (lex.next() == TOK_LT || lex.next() == TOK_DOUBLE_LT) )
        {
            if( !consume_tt_angle(lex) )
                return false;
        }

        while(lex.next() == TOK_DOUBLE_COLON)
        {
            lex.consume();
            if( lex.next() == TOK_STRING )
            {
                lex.consume();
            }
            else if( !type_mode && (lex.next() == TOK_LT || lex.next() == TOK_DOUBLE_LT) )
            {
                if( !consume_tt_angle(lex) )
                    return false;
            }
            else if( lex.next() == TOK_IDENT )
            {
                lex.consume();
                if( type_mode && (lex.next() == TOK_LT || lex.next() == TOK_DOUBLE_LT) )
                {
                    if( !consume_tt_angle(lex) )
                        return false;
                }
            }
            else
            {
                return false;
            }
        }
        return true;
    }
    bool consume_type(TokenStreamRO& lex)
    {
        TRACE_FUNCTION;
        switch(lex.next())
        {
        case TOK_UNDERSCORE:
            lex.consume();
            return true;
        case TOK_INTERPOLATED_TYPE:
            lex.consume();
            return true;
        case TOK_PAREN_OPEN:
        case TOK_SQUARE_OPEN:
            return consume_tt(lex);
        case TOK_RWORD_SUPER:
        case TOK_RWORD_SELF:
        case TOK_IDENT:
        case TOK_DOUBLE_COLON:
        case TOK_INTERPOLATED_IDENT:
        case TOK_INTERPOLATED_PATH:
            if( !consume_path(lex, true) )
                return false;
            if( lex.consume_if(TOK_EXCLAM) )
            {
                if( lex.next() != TOK_PAREN_OPEN && lex.next() != TOK_SQUARE_OPEN && lex.next() != TOK_BRACE_OPEN )
                    return false;
                if( !consume_tt(lex) )
                    return false;
            }
            return true;
        case TOK_AMP:
        case TOK_DOUBLE_AMP:
            lex.consume();
            lex.consume_if(TOK_LIFETIME);
            lex.consume_if(TOK_RWORD_MUT);
            return consume_type(lex);
        case TOK_STAR:
            lex.consume();
            if( lex.consume_if(TOK_RWORD_MUT) )
                ;
            else if( lex.consume_if(TOK_RWORD_CONST) )
                ;
            else
                return false;
            return consume_type(lex);
        case TOK_EXCLAM:
            lex.consume();
            return true;

        case TOK_RWORD_UNSAFE:
            lex.consume();
            if(lex.next() == TOK_RWORD_EXTERN) {
        case TOK_RWORD_EXTERN:
            lex.consume();
            lex.consume_if(TOK_STRING);
            }
            if( lex.next() != TOK_RWORD_FN )
                return false;
        case TOK_RWORD_FN:
            lex.consume();
            if( lex.next() != TOK_PAREN_OPEN )
                return false;
            if( !consume_tt(lex) )
                return false;
            if( lex.consume_if(TOK_THINARROW) )
            {
                consume_type(lex);
            }
            return true;
        default:
            return false;
        }
    }
    bool consume_pat(TokenStreamRO& lex)
    {
        TRACE_FUNCTION;

        if(lex.next() == TOK_RWORD_REF || lex.next() == TOK_RWORD_MUT )
        {
            lex.consume_if(TOK_RWORD_REF);
            lex.consume_if(TOK_RWORD_MUT);
            if( !lex.consume_if(TOK_IDENT) )
                return false;
            if( !lex.consume_if(TOK_AT) )
                return true;
        }

        if( lex.consume_if(TOK_INTERPOLATED_PATTERN) )
            return true;

        for(;;)
        {
            if( lex.consume_if(TOK_UNDERSCORE) )
                return true;
            switch(lex.next())
            {
            case TOK_IDENT:
            case TOK_RWORD_SUPER:
            case TOK_RWORD_SELF:
            case TOK_DOUBLE_COLON:
            case TOK_INTERPOLATED_PATH:
                consume_path(lex);
                if( lex.next() == TOK_BRACE_OPEN ) {
                    return consume_tt(lex);
                }
                else if( lex.next() == TOK_PAREN_OPEN ) {
                    return consume_tt(lex);
                }
                else if( lex.next() == TOK_EXCLAM ) {
                    lex.consume();
                    return consume_tt(lex);
                }
                break;
            case TOK_RWORD_BOX:
                lex.consume();
                return consume_pat(lex);
            case TOK_AMP:
            case TOK_DOUBLE_AMP:
                lex.consume();
                lex.consume_if(TOK_RWORD_MUT);
                return consume_pat(lex);
            case TOK_PAREN_OPEN:
            case TOK_SQUARE_OPEN:
                return consume_tt(lex);
            case TOK_STRING:
            case TOK_INTEGER:
            case TOK_FLOAT:
                lex.consume();
                break;
            default:
                return false;
            }
            if(lex.consume_if(TOK_AT))
                continue;
            if( lex.consume_if(TOK_TRIPLE_DOT) )
            {
                switch(lex.next())
                {
                case TOK_IDENT:
                case TOK_RWORD_SUPER:
                case TOK_RWORD_SELF:
                case TOK_DOUBLE_COLON:
                case TOK_INTERPOLATED_PATH:
                    consume_path(lex);
                    break;
                case TOK_STRING:
                case TOK_INTEGER:
                case TOK_FLOAT:
                    lex.consume();
                    break;
                default:
                    return false;
                }
            }
            return true;
        }
    }
    // Consume an expression
    bool consume_expr(TokenStreamRO& lex, bool no_struct_lit=false)
    {
        TRACE_FUNCTION;
        bool cont;

        // Closures
        if( lex.next() == TOK_RWORD_MOVE || lex.next() == TOK_PIPE || lex.next() == TOK_DOUBLE_PIPE )
        {
            lex.consume_if(TOK_RWORD_MOVE);
            if( lex.consume_if(TOK_PIPE) )
            {
                do
                {
                    if( lex.next() == TOK_PIPE )
                        break;
                    consume_pat(lex);
                    if(lex.consume_if(TOK_COLON))
                    {
                        consume_type(lex);
                    }
                } while(lex.consume_if(TOK_COMMA));
                if( !lex.consume_if(TOK_PIPE) )
                    return false;
            }
            else
            {
                lex.consume();
            }
            if(lex.consume_if(TOK_THINARROW))
            {
                if( !consume_type(lex) )
                    return false;
            }
            return consume_expr(lex);
        }

        do {
            bool inner_cont;
            do
            {
                inner_cont = true;
                switch(lex.next())
                {
                case TOK_STAR:  // Deref
                case TOK_DASH:  // Negate
                case TOK_EXCLAM: // Invert
                case TOK_RWORD_BOX: // Box
                    lex.consume();
                    break;
                case TOK_AMP:
                    lex.consume();
                    lex.consume_if(TOK_RWORD_MUT);
                    break;
                default:
                    inner_cont = false;
                    break;
                }
            } while(inner_cont);

            // :: -> path
            // ident -> path
            // '<' -> path
            // '(' -> tt
            // '[' -> tt
            switch(lex.next())
            {
            case TOK_RWORD_CONTINUE:
            case TOK_RWORD_BREAK:
                lex.consume();
                lex.consume_if(TOK_LIFETIME);
                if(0)
            case TOK_RWORD_RETURN:
                lex.consume();
                switch(lex.next())
                {
                case TOK_EOF:
                case TOK_SEMICOLON:
                case TOK_COMMA:
                case TOK_PAREN_CLOSE:
                case TOK_BRACE_CLOSE:
                case TOK_SQUARE_CLOSE:
                    break;
                default:
                    if( !consume_expr(lex) )
                        return false;
                    break;
                }
                break;
            case TOK_IDENT:
            case TOK_INTERPOLATED_IDENT:
            case TOK_INTERPOLATED_PATH:
            case TOK_DOUBLE_COLON:
            case TOK_RWORD_SELF:
            case TOK_RWORD_SUPER:
            case TOK_LT:
            case TOK_DOUBLE_LT:
                if( !consume_path(lex) )
                    return false;
                if( lex.next() == TOK_BRACE_OPEN && !no_struct_lit )
                    consume_tt(lex);
                else if( lex.consume_if(TOK_EXCLAM) )
                {
                    if( lex.consume_if(TOK_IDENT) )
                    {
                        // yay?
                    }
                    consume_tt(lex);
                }
                break;

            case TOK_INTERPOLATED_EXPR:
                lex.consume();
                break;
            case TOK_INTEGER:
            case TOK_FLOAT:
            case TOK_STRING:
            case TOK_BYTESTRING:
            case TOK_RWORD_TRUE:
            case TOK_RWORD_FALSE:
                lex.consume();
                break;


            case TOK_RWORD_UNSAFE:
                lex.consume();
                if(lex.next() != TOK_BRACE_OPEN )
                    return false;
            case TOK_PAREN_OPEN:
            case TOK_SQUARE_OPEN:
            case TOK_BRACE_OPEN:
                consume_tt(lex);
                break;

            // TODO: Do these count for "expr"?
            case TOK_RWORD_FOR:
                lex.consume();
                if( !consume_pat(lex) )
                    return false;
                if( !lex.consume_if(TOK_RWORD_IN) )
                    return false;
                if( !consume_expr(lex, true) )
                    return false;
                if( lex.next() != TOK_BRACE_OPEN )
                    return false;
                if( !consume_tt(lex) )
                    return false;
                break;
            case TOK_RWORD_MATCH:
                lex.consume();
                // TODO: Parse _without_ consuming a struct literal
                if( !consume_expr(lex, true) )
                    return false;
                if( lex.next() != TOK_BRACE_OPEN )
                    return false;
                if( !consume_tt(lex) )
                    return false;
                break;
            case TOK_RWORD_WHILE:
                lex.consume();
                if( !consume_expr(lex, true) )
                    return false;
                if( lex.next() != TOK_BRACE_OPEN )
                    return false;
                if( !consume_tt(lex) )
                    return false;
                break;
            case TOK_RWORD_LOOP:
                lex.consume();
                if( lex.next() != TOK_BRACE_OPEN )
                    return false;
                consume_tt(lex);
                break;
            case TOK_RWORD_IF:
                while(1)
                {
                    assert(lex.next() == TOK_RWORD_IF);
                    lex.consume();
                    if(lex.next() == TOK_RWORD_LET)
                    {
                        lex.consume();
                        if( !consume_pat(lex) )
                            return false;
                        if( lex.next() != TOK_EQUAL )
                            return false;
                        lex.consume();
                    }
                    if( !consume_expr(lex, true) )
                        return false;
                    if( lex.next() != TOK_BRACE_OPEN )
                        return false;
                    consume_tt(lex);
                    if( lex.next() != TOK_RWORD_ELSE )
                        break;
                    lex.consume();

                    if( lex.next() != TOK_RWORD_IF )
                    {
                        if( lex.next() != TOK_BRACE_OPEN )
                            return false;
                        consume_tt(lex);
                        break;
                    }
                }
                break;
            default:
                return false;
            }

            do
            {
                inner_cont = true;
                // '.' ident/int
                switch( lex.next() )
                {
                case TOK_QMARK:
                    lex.consume();
                    break;
                case TOK_DOT:
                    lex.consume();
                    if( lex.consume_if(TOK_IDENT) )
                    {
                        if( lex.consume_if(TOK_DOUBLE_COLON) )
                        {
                            if( !(lex.next() == TOK_LT || lex.next() == TOK_DOUBLE_LT) )
                                return false;
                            if( !consume_tt_angle(lex) )
                                return false;
                        }
                    }
                    else if( lex.consume_if(TOK_INTEGER) )
                        ;
                    else
                        return false;
                    break;
                // '[' -> tt
                case TOK_SQUARE_OPEN:
                // '(' -> tt
                case TOK_PAREN_OPEN:
                    consume_tt(lex);
                    break;
                default:
                    inner_cont = false;
                    break ;
                }
            } while(inner_cont);

            if( lex.consume_if(TOK_COLON) )
            {
                consume_type(lex);
            }

            while( lex.consume_if(TOK_RWORD_AS) )
            {
                consume_type(lex);
            }

            cont = true;
            switch(lex.next())
            {
            case TOK_PLUS:
            case TOK_DASH:
            case TOK_SLASH:
            case TOK_STAR:
            case TOK_PERCENT:
            case TOK_DOUBLE_LT:
            case TOK_DOUBLE_GT:
            case TOK_PIPE:
            case TOK_AMP:
            case TOK_CARET:
            case TOK_LT:
            case TOK_GT:
            case TOK_LTE:
            case TOK_GTE:
            case TOK_DOUBLE_EQUAL:
            case TOK_EXCLAM_EQUAL:
            case TOK_DOUBLE_AMP:
            case TOK_DOUBLE_PIPE:
            case TOK_DOUBLE_DOT:
            case TOK_TRIPLE_DOT:
                lex.consume();
                break;
            case TOK_EQUAL:
            case TOK_PLUS_EQUAL:
            case TOK_DASH_EQUAL:
            case TOK_SLASH_EQUAL:
            case TOK_STAR_EQUAL:
            case TOK_PERCENT_EQUAL:
            case TOK_AMP_EQUAL:
            case TOK_PIPE_EQUAL:
                lex.consume();
                break;
            default:
                cont = false;
                break;
            }
        } while(cont);
        return true;
    }
    bool consume_stmt(TokenStreamRO& lex)
    {
        TRACE_FUNCTION;
        if( lex.consume_if(TOK_RWORD_LET) )
        {
            if( !consume_pat(lex) )
                return false;
            if( lex.consume_if(TOK_COLON) )
            {
                if( !consume_type(lex) )
                    return false;
            }
            if( lex.consume_if(TOK_EQUAL) )
            {
                if( !consume_expr(lex) )
                    return false;
            }
            return true;
        }
        else
        {
            if( !consume_expr(lex) )
                return false;
            return true;
        }
    }
    bool consume_item(TokenStreamRO& lex)
    {
        TRACE_FUNCTION;

        struct H {
            static bool maybe_generics(TokenStreamRO& lex) {
                if(lex.next() == TOK_LT)
                {
                    if( !consume_tt_angle(lex) )
                        return false;
                }
                return true;
            }
            static bool maybe_where(TokenStreamRO& lex) {
                if(lex.next() == TOK_RWORD_WHERE)
                {
                    TODO(Span(), "where in macro eval");
                }
                return true;
            }
        };

        while( lex.next() == TOK_HASH )
        {
            lex.consume();
            lex.consume_if(TOK_EXCLAM);
            consume_tt(lex);
        }
        // Interpolated items
        if( lex.consume_if(TOK_INTERPOLATED_ITEM) )
            return true;
        // Macro invocation
        // TODO: What about `union!` as a macro? Needs to be handled below
        if( (lex.next() == TOK_IDENT && lex.next_tok().str() != "union")
         || lex.next() == TOK_RWORD_SELF
         || lex.next() == TOK_RWORD_SUPER
         || lex.next() == TOK_DOUBLE_COLON
         )
        {
            if( !consume_path(lex) )
                return false;
            if( !lex.consume_if(TOK_EXCLAM) )
                return false;
            bool need_semicolon = (lex.next() != TOK_BRACE_OPEN);
            consume_tt(lex);
            if( need_semicolon )
            {
                if( !lex.consume_if(TOK_SEMICOLON) )
                    return false;
            }
            return true;
        }
        // Normal items
        if(lex.next() == TOK_RWORD_PUB)
            lex.consume();
        if(lex.next() == TOK_RWORD_UNSAFE)
            lex.consume();
        DEBUG("Check item: " << lex.next_tok());
        switch(lex.next())
        {
        case TOK_RWORD_USE:
            // Lazy mode
            while( lex.next() != TOK_SEMICOLON )
                lex.consume();
            lex.consume();
            break;
        case TOK_RWORD_MOD:
            lex.consume();
            if( !lex.consume_if(TOK_IDENT) )
                return false;
            if( lex.consume_if(TOK_SEMICOLON) )
                ;
            else if( lex.next() == TOK_BRACE_OPEN )
            {
                if( !consume_tt(lex) )
                    return false;
            }
            else
            {
                return false;
            }
            break;
        // impl [Foo for] Bar { ... }
        case TOK_RWORD_IMPL:
            lex.consume();
            if( !H::maybe_generics(lex) )
                return false;
            if( !consume_type(lex) )
                return false;
            if( lex.consume_if(TOK_RWORD_FOR) )
            {
                if( !consume_type(lex) )
                    return false;
            }
            if( !H::maybe_where(lex) )
                return false;
            if( lex.next() != TOK_BRACE_OPEN )
                return false;
            return consume_tt(lex);
        // type Foo
        case TOK_RWORD_TYPE:
            lex.consume();
            if( !lex.consume_if(TOK_IDENT) )
                return false;
            if( !H::maybe_generics(lex) )
                return false;

            if( !lex.consume_if(TOK_EQUAL) )
                return false;
            if( !consume_type(lex) )
                return false;
            if( !lex.consume_if(TOK_SEMICOLON) )
                return false;

            break;
        // static FOO
        case TOK_RWORD_STATIC:
            lex.consume();
            if( !lex.consume_if(TOK_IDENT) )
                return false;
            if( !lex.consume_if(TOK_COLON) )
                return false;
            if( !consume_type(lex) )
                return false;
            if( !lex.consume_if(TOK_EQUAL) )
                return false;
            if( !consume_expr(lex) )
                return false;
            if( !lex.consume_if(TOK_SEMICOLON) )
                return false;
            break;
        case TOK_RWORD_STRUCT:
            lex.consume();
            if( !lex.consume_if(TOK_IDENT) )
                return false;
            if( !H::maybe_generics(lex) )
                return false;
            if( !H::maybe_where(lex) )
                return false;
            if( lex.consume_if(TOK_SEMICOLON) )
                ;
            else if( lex.next() == TOK_PAREN_OPEN )
            {
                if( !consume_tt(lex) )
                    return false;
                if( !lex.consume_if(TOK_SEMICOLON) )
                    return false;
            }
            else if( lex.next() == TOK_BRACE_OPEN )
            {
                if( !consume_tt(lex) )
                    return false;
            }
            else
                return false;
            break;
        case TOK_RWORD_ENUM:
            lex.consume();
            if( !lex.consume_if(TOK_IDENT) )
                return false;
            if( !H::maybe_generics(lex) )
                return false;
            if( !H::maybe_where(lex) )
                return false;
            if( lex.next() != TOK_BRACE_OPEN )
                return false;
            return consume_tt(lex);
        case TOK_IDENT:
            if( lex.next_tok().str() != "union" )
                return false;
            lex.consume();
            if( lex.next() == TOK_EXCLAM )
            {
                bool need_semicolon = (lex.next() != TOK_BRACE_OPEN);
                consume_tt(lex);
                if( need_semicolon )
                {
                    if( !lex.consume_if(TOK_SEMICOLON) )
                        return false;
                }
                return true;
            }
            else
            {
                if( !H::maybe_generics(lex) )
                    return false;
                if( !H::maybe_where(lex) )
                    return false;
                if( lex.next() != TOK_BRACE_OPEN )
                    return false;
                return consume_tt(lex);
            }
            break;
        // const fn
        // const FOO
        case TOK_RWORD_CONST:
            lex.consume();
            if(lex.next() == TOK_RWORD_UNSAFE)
                lex.consume();
            if( lex.consume_if(TOK_RWORD_FN) )
            {
                goto fn;
            }
            else
            {
                if( !lex.consume_if(TOK_IDENT) )
                    return false;
                if( !lex.consume_if(TOK_COLON) )
                    return false;
                consume_type(lex);
                if( !lex.consume_if(TOK_EQUAL) )
                    return false;
                consume_expr(lex);
                if( !lex.consume_if(TOK_SEMICOLON) )
                    return false;
            }
            break;
        case TOK_RWORD_EXTERN:
            lex.consume();
            if( lex.consume_if(TOK_RWORD_CRATE) )
            {
                if( !lex.consume_if(TOK_IDENT) )
                    return false;
                if( !lex.consume_if(TOK_SEMICOLON) )
                    return false;
                break;
            }

            lex.consume_if(TOK_STRING);
            if( lex.next() == TOK_BRACE_OPEN )
            {
                return consume_tt(lex);
            }
            if( ! lex.consume_if(TOK_RWORD_FN) )
                return false;
            goto fn;
        case TOK_RWORD_FN:
            lex.consume();
        fn:
            if( !lex.consume_if(TOK_IDENT) )
                return false;

            if( !H::maybe_generics(lex) )
                return false;
            if(lex.next() != TOK_PAREN_OPEN)
                return false;
            if( !consume_tt(lex) )
                return false;

            if( lex.consume_if(TOK_THINARROW) )
            {
                if( !consume_type(lex) )
                    return false;
            }

            if( !H::maybe_where(lex) )
                return false;

            if( lex.consume_if(TOK_SEMICOLON) )
            {
                // TODO: Is this actually valid?
                break;
            }
            else if( lex.next() == TOK_BRACE_OPEN )
            {
                if( !consume_tt(lex) )
                    return false;
            }
            else
            {
                return false;
            }
            break;
        default:
            return false;
        }
        return true;
    }

    bool consume_from_frag(TokenStreamRO& lex, MacroPatEnt::Type type)
    {
        TRACE_FUNCTION_F(type);
        switch(type)
        {
        case MacroPatEnt::PAT_TOKEN:
        case MacroPatEnt::PAT_LOOP:
            throw "";
        case MacroPatEnt::PAT_BLOCK:
            if( lex.next() == TOK_BRACE_OPEN ) {
                return consume_tt(lex);
            }
            else if( lex.next() == TOK_INTERPOLATED_BLOCK ) {
                lex.consume();
            }
            else {
                return false;
            }
            break;
        case MacroPatEnt::PAT_IDENT:
            if( lex.next() == TOK_IDENT || is_reserved_word(lex.next()) ) {
                lex.consume();
            }
            else {
                return false;
            }
            break;
        case MacroPatEnt::PAT_TT:
            return consume_tt(lex);
        case MacroPatEnt::PAT_PATH:
            return consume_path(lex, true);
        case MacroPatEnt::PAT_TYPE:
            return consume_type(lex);
        case MacroPatEnt::PAT_EXPR:
            return consume_expr(lex);
        case MacroPatEnt::PAT_STMT:
            return consume_stmt(lex);
        case MacroPatEnt::PAT_PAT:
            return consume_pat(lex);
        case MacroPatEnt::PAT_META:
            if( lex.next() == TOK_INTERPOLATED_META ) {
                lex.consume();
            }
            else if( lex.next() == TOK_IDENT )
            {
                lex.consume();
                switch(lex.next())
                {
                case TOK_PAREN_OPEN:
                    return consume_tt(lex);
                case TOK_EQUAL:
                    lex.consume();
                    switch(lex.next())
                    {
                    case TOK_INTEGER:
                    case TOK_FLOAT:
                    case TOK_STRING:
                        lex.consume();
                        break;
                    default:
                        return false;
                    }
                    break;
                default:
                    break;
                }
            }
            else {
                return false;
            }
            break;
        case MacroPatEnt::PAT_ITEM:
            return consume_item(lex);
        }
        return true;
    }
}

unsigned int Macro_InvokeRules_MatchPattern(const Span& sp, const MacroRules& rules, TokenTree input, AST::Module& mod,  ParameterMappings& bound_tts)
{
    TRACE_FUNCTION;

    struct ActiveArm {
        unsigned int    index;
        MacroPatternStream  pat_stream;
        TokenStreamRO   in_stream;
    };

    ::std::vector<size_t>   matches;
    for(size_t i = 0; i < rules.m_rules.size(); i ++)
    {
        auto lex = TokenStreamRO(input);
        auto arm_stream = MacroPatternStream(rules.m_rules[i].m_pattern);

        bool fail = false;
        for(;;)
        {
            auto pat = arm_stream.next();
            if(pat.is_End())
            {
                DEBUG(i << " End");
                if( lex.next() != TOK_EOF )
                    fail = true;
                break;
            }
            else if( const auto* e = pat.opt_IfPat() )
            {
                DEBUG(i << " IfPat(" << (e->is_equal ? "==" : "!=") << " ?" << e->type << ")");
                auto lc = lex.clone();
                if( consume_from_frag(lc, e->type) == e->is_equal )
                {
                    DEBUG("- Succeeded");
                    arm_stream.if_succeeded();
                }
            }
            else if( const auto* e = pat.opt_IfTok() )
            {
                DEBUG(i << " IfTok(" << (e->is_equal ? "==" : "!=") << " ?" << e->tok << ")");
                const auto& tok = lex.next_tok();
                if( (tok == e->tok) == e->is_equal )
                {
                    DEBUG("- Succeeded");
                    arm_stream.if_succeeded();
                }
            }
            else if( const auto* e = pat.opt_ExpectTok() )
            {
                const auto& tok = lex.next_tok();
                DEBUG(i << " ExpectTok(" << *e << ") == " << tok);
                if( tok != *e )
                {
                    fail = true;
                    break;
                }
                lex.consume();
            }
            else if( const auto* e = pat.opt_ExpectPat() )
            {
                DEBUG(i << " ExpectPat(" << e->type << " => $" << e->idx << ")");
                if( !consume_from_frag(lex, e->type) )
                {
                    fail = true;
                    break;
                }
            }
            else
            {
                // Unreachable.
            }
        }


        if( ! fail )
        {
            matches.push_back(i);
            DEBUG(i << " MATCHED");
        }
        else
        {
            DEBUG(i << " FAILED");
        }
    }

    if( matches.size() == 0 )
    {
        // ERROR!
        TODO(sp, "No arm matched");
    }
    else
    {
        // yay!

        // NOTE: There can be multiple arms active, take the first.
        auto i = matches[0];
        DEBUG("Evalulating arm " << i);

        auto lex = TTStreamO(sp, mv$(input));
        SET_MODULE(lex, mod);
        auto arm_stream = MacroPatternStream(rules.m_rules[i].m_pattern);

        struct Capture {
            unsigned int    binding_idx;
            ::std::vector<unsigned int> iterations;
            unsigned int    cap_idx;
        };
        ::std::vector<InterpolatedFragment> captures;
        ::std::vector<Capture>  capture_info;

        for(;;)
        {
            auto pat = arm_stream.next();
            if(pat.is_End())
            {
                break;
            }
            else if( const auto* e = pat.opt_IfPat() )
            {
                DEBUG(i << " IfPat(" << (e->is_equal ? "==" : "!=") << " ?" << e->type << ")");
                if( Macro_TryPatternCap(lex, e->type) == e->is_equal )
                {
                    DEBUG("- Succeeded");
                    arm_stream.if_succeeded();
                }
            }
            else if( const auto* e = pat.opt_IfTok() )
            {
                DEBUG(i << " IfTok(" << (e->is_equal ? "==" : "!=") << " ?" << e->tok << ")");
                auto tok = lex.getToken();
                if( (tok == e->tok) == e->is_equal )
                {
                    DEBUG("- Succeeded");
                    arm_stream.if_succeeded();
                }
                lex.putback( mv$(tok) );
            }
            else if( const auto* e = pat.opt_ExpectTok() )
            {
                auto tok = lex.getToken();
                DEBUG(i << " ExpectTok(" << *e << ") == " << tok);
                if( tok != *e )
                {
                    ERROR(sp, E0000, "Expected token in match arm");
                    break;
                }
            }
            else if( const auto* e = pat.opt_ExpectPat() )
            {
                DEBUG(i << " ExpectPat(" << e->type << " => $" << e->idx << ")");

                auto cap = Macro_HandlePatternCap(lex, e->type);

                unsigned int cap_idx = captures.size();
                captures.push_back( mv$(cap) );
                capture_info.push_back( Capture { e->idx, arm_stream.get_loop_iters(), cap_idx } );
            }
            else
            {
                // Unreachable.
            }
        }

        for(const auto& cap : capture_info)
        {
            bound_tts.insert( cap.binding_idx, cap.iterations, mv$(captures[cap.cap_idx]) );
        }
        return i;
    }
}
#else
unsigned int Macro_InvokeRules_MatchPattern(const Span& sp, const MacroRules& rules, TokenTree input, AST::Module& mod,  ParameterMappings& bound_tts)
{
    TRACE_FUNCTION;
    Span    sp;// = input.span();

    struct Capture {
        unsigned int    binding_idx;
        ::std::vector<unsigned int> iterations;
        unsigned int    cap_idx;
    };
    struct ActiveArm {
        unsigned int    index;
        ::std::vector<Capture>  captures;
        MacroPatternStream  stream;
    };
    // - List of active rules (rules that haven't yet failed)
    ::std::vector< ActiveArm > active_arms;
    active_arms.reserve( rules.m_rules.size() );
    for(unsigned int i = 0; i < rules.m_rules.size(); i ++)
    {
        active_arms.push_back( ActiveArm { i, {}, MacroPatternStream(rules.m_rules[i].m_pattern) } );
    }

    // - List of captured values
    ::std::vector<InterpolatedFragment> captures;

    TTStreamO   lex(sp, mv$(input) );
    SET_MODULE(lex, mod);
    while(true)
    {
        DEBUG("--- ---");
        // 1. Get concrete patterns for all active rules (i.e. no If* patterns)
        ::std::vector<SimplePatEnt> arm_pats;
        for(auto& arm : active_arms)
        {
            auto idx = arm.index;
            SimplePatEnt pat;
            // Consume all If* rules
            do
            {
                pat = arm.stream.next();
                TU_IFLET( SimplePatEnt, pat, IfPat, e,
                    DEBUG(idx << " IfPat(" << (e.is_equal ? "==" : "!=") << " ?" << e.type << ")");
                    if( Macro_TryPatternCap(lex, e.type) == e.is_equal )
                    {
                        DEBUG("- Succeeded");
                        arm.stream.if_succeeded();
                    }
                )
                else TU_IFLET( SimplePatEnt, pat, IfTok, e,
                    DEBUG(idx << " IfTok(" << (e.is_equal ? "==" : "!=") << " ?" << e.tok << ")");
                    auto tok = lex.getToken();
                    if( (tok == e.tok) == e.is_equal )
                    {
                        DEBUG("- Succeeded");
                        arm.stream.if_succeeded();
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
            auto idx = active_arms[i].index;
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
                DEBUG("- Failed arm " << active_arms[i].index);
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

        // 3. If there is a token pattern in the list, take that arm (and any other token arms)
        const SimplePatEnt* tok_pat = nullptr;
        unsigned int ident_pat_idx = arm_pats.size();
        bool has_non_ident_pat = false;
        for( unsigned int i = 0; i < arm_pats.size(); i ++ )
        {
            const auto& pat = arm_pats[i];
            TU_IFLET(SimplePatEnt, pat, ExpectTok, e,
                if( tok_pat ) {
                    if( e != tok_pat->as_ExpectTok() )
                        ERROR(lex.getPosition(), E0000, "Incompatible macro arms - " << tok_pat->as_ExpectTok() << " vs " << e);
                }
                else {
                    tok_pat = &pat;
                }
            )
            else TU_IFLET(SimplePatEnt, pat, ExpectPat, e,
                if( e.type == MacroPatEnt::PAT_IDENT ) {
                    ident_pat_idx = i;
                }
                else {
                    has_non_ident_pat = true;
                }
            )
        }

        if( tok_pat )
        {
            auto tok = lex.getToken();
            const auto& e = tok_pat->as_ExpectTok();
            // NOTE: This should never fail.
            if( tok != e ) {
                ERROR(lex.getPosition(), E0000, "Unexpected " << tok << ", expected " << e);
            }
        }
        else
        {
            if( has_non_ident_pat && ident_pat_idx < arm_pats.size() )
            {
                // For all :ident patterns present, check the next rule.
                // - If this rule would fail, remove the arm.
                bool ident_rule_kept = false;
                for( unsigned int i = 0; i < arm_pats.size(); )
                {
                    bool discard = false;
                    const auto& pat = arm_pats[i];
                    const auto& e = pat.as_ExpectPat();
                    if( e.type == MacroPatEnt::PAT_IDENT )
                    {
                        const auto& next = active_arms[i].stream.peek();
                        TU_MATCHA( (next), (ne),
                        (IfPat, TODO(sp, "Handle IfPat following a conflicting :ident");),
                        (IfTok, TODO(sp, "IfTok following a conflicting :ident");),
                        (ExpectTok,
                            if( ne.type() != lex.lookahead(1) ) {
                                DEBUG("Discard active arm " << i << " due to next token mismatch");
                                discard = true;
                            }
                            else {
                                ident_rule_kept = true;
                            }
                            ),
                        (ExpectPat,
                            TODO(sp, "Handle ExpectPat following a conflicting :ident");
                            ),
                        (End, TODO(sp, "Handle End following a conflicting :ident"); )
                        )
                    }

                    if( discard ) {
                        arm_pats.erase( arm_pats.begin() + i );
                        active_arms.erase( active_arms.begin() + i );
                    }
                    else {
                        ++ i;
                    }
                }

                // If there are any remaining ident rules, erase the non-ident rules.
                if( ident_rule_kept ) {
                    // If no rules were discarded, remove the non-ident rules
                    for( unsigned int i = 0; i < arm_pats.size(); )
                    {
                        if( arm_pats[i].as_ExpectPat().type != MacroPatEnt::PAT_IDENT ) {
                            arm_pats.erase( arm_pats.begin() + i );
                            active_arms.erase( active_arms.begin() + i );
                        }
                        else {
                            ++ i;
                        }
                    }
                }
                assert(arm_pats.size() > 0);
                assert(arm_pats.size() == active_arms.size());
            }

            // 3. Check that all remaining arms are the same pattern.
            const auto& active_pat = arm_pats[0];
            for(unsigned int i = 1; i < arm_pats.size(); i ++)
            {
                if( active_pat.tag() != arm_pats[i].tag() ) {
                    ERROR(lex.getPosition(), E0000, "Incompatible macro arms "
                        << "- " << active_arms[0].index << " SimplePatEnt::" << active_pat.tag_str()
                        << " vs " << active_arms[i].index<<  " SimplePatEnt::" << arm_pats[i].tag_str()
                        );
                }
                TU_MATCH( SimplePatEnt, (active_pat, arm_pats[i]), (e1, e2),
                (IfPat, BUG(sp, "IfPat unexpected here");),
                (IfTok, BUG(sp, "IfTok unexpected here");),
                (ExpectTok,
                    BUG(sp, "ExpectTok unexpected here");
                    ),
                (ExpectPat,
                    // Can fail, as :expr and :stmt overlap in their trigger set
                    if( e1.type != e2.type ) {
                        ERROR(lex.getPosition(), E0000, "Incompatible macro arms - mismatched patterns " << e1.type << " and " << e2.type);
                    }
                    ),
                (End,
                    )
                )
            }

            // 4. Apply patterns.
            TU_MATCH( SimplePatEnt, (arm_pats[0]), (e),
            (End,
                auto tok = lex.getToken();
                if( tok.type() != TOK_EOF ) {
                    ERROR(lex.getPosition(), E0000, "Unexpected " << tok << ", expected TOK_EOF");
                }
                // NOTE: There can be multiple arms active, take the first.
                for(const auto& cap : active_arms[0].captures)
                {
                    bound_tts.insert( cap.binding_idx, cap.iterations, mv$(captures[cap.cap_idx]) );
                }
                return active_arms[0].index;
                ),
            (IfPat, BUG(sp, "IfPat unexpected here");),
            (IfTok, BUG(sp, "IfTok unexpected here");),
            (ExpectTok,
                BUG(sp, "ExpectTok should have been handled already");
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

                auto cap = Macro_HandlePatternCap(lex, e.type);

                unsigned int cap_idx = captures.size();
                captures.push_back( mv$(cap) );
                for(unsigned int i = 0; i < active_arms.size(); i ++)
                {
                    auto& arm = active_arms[i];
                    const auto& pat_e = arm_pats[i].as_ExpectPat();
                    arm.captures.push_back( Capture { pat_e.idx, arm.stream.get_loop_iters(), cap_idx } );
                }
                )
            )
        }

        // Keep looping - breakout is handled in 'End' above
    }
}
#endif

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
    // TODO: Return the attached position of the last fetched token
    return Position(m_macro_filename, 0, m_state.top_pos());
}
::std::shared_ptr<Span> MacroExpander::outerSpan() const
{
    return m_invocation_span;
}
Ident::Hygiene MacroExpander::realGetHygiene() const
{
    if( m_ttstream )
    {
        return m_ttstream->getHygiene();
    }
    else
    {
        return m_hygiene;
    }
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
        TU_IFLET(MacroExpansionEnt, ent, Token, e,
            return e.clone();
        )
        else if( ent.is_NamedValue() ) {
            const auto& e = ent.as_NamedValue();
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
                        m_ttstream.reset( new TTStreamO(*this->outerSpan(), mv$(frag->as_tt()) ) );
                    }
                    else
                    {
                        m_ttstream.reset( new TTStreamO(*this->outerSpan(), frag->as_tt().clone() ) );
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
        }
        else TU_IFLET(MacroExpansionEnt, ent, Loop, e,
            //assert( e.joiner.tok() != TOK_NULL );
            return e.joiner;
        )
        else {
            throw "";
        }
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
