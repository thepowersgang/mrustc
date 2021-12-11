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
#include <ast/crate.hpp>
#include <hir/hir.hpp>  // HIR::Crate

 // Map of: LoopIndex=>(Path=>Count)
typedef std::map<unsigned, std::map< std::vector<unsigned>, unsigned > >    loop_counts_t;

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
            os << "CapturedVar { " << x.top_layer << " }";
            return os;
        }
    };


    loop_counts_t   m_loop_counts;

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

    void set_loop_counts(loop_counts_t loop_counts) {
        for(const auto& e : loop_counts) {
            DEBUG(e.first << ": {" << e.second << "}");
        }
        m_loop_counts = std::move(loop_counts);
    }
    void insert(unsigned int name_index, const ::std::vector<unsigned int>& iterations, InterpolatedFragment data);

    InterpolatedFragment* get(const ::std::vector<unsigned int>& iterations, unsigned int name_idx);

    unsigned int get_loop_repeats(const ::std::vector<unsigned int>& iterations, unsigned int loop_idx) const;

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

class MacroPatternStream
{
    const ::std::vector<SimplePatEnt>&  m_simple_ents;
    size_t  m_cur_pos;

    bool    m_last_was_cond;
    bool    m_condition_met;
    ::std::vector<bool> m_condition_history;

    const ::std::vector<bool>* m_condition_replay;
    size_t  m_condition_replay_pos;

    // Currently processed loop indexes
    ::std::vector<unsigned int> m_current_loops;
    // Iteration index of each active loop level
    ::std::vector<unsigned int> m_loop_iterations;

    loop_counts_t m_loop_counts;

    bool m_peek_cache_valid = false;
    const SimplePatEnt* m_peek_cache;

public:
    MacroPatternStream(const ::std::vector<SimplePatEnt>& ents, const ::std::vector<bool>* condition_replay=nullptr):
        m_simple_ents(ents),
        m_cur_pos(0),
        m_last_was_cond(false),
        m_condition_replay(condition_replay),
        m_condition_replay_pos(0)
    {
    }

    size_t cur_pos() const { return m_cur_pos; }

    /// Get the next pattern entry
    const SimplePatEnt& next();

    const SimplePatEnt& peek() {
        if( !m_peek_cache_valid ) {
            m_peek_cache = &next();
            m_peek_cache_valid = true;
        }
        return *m_peek_cache;
    }

    /// Inform the stream that the `if` rule that was just returned succeeded
    void if_succeeded();
    /// Get the current loop iteration count
    const ::std::vector<unsigned int>& get_loop_iters() const {
        return m_loop_iterations;
    }

    ::std::vector<bool> take_history() {
        return ::std::move(m_condition_history);
    }
    loop_counts_t take_loop_counts() {
        return ::std::move(m_loop_counts);
    }
};

// === Prototypes ===
unsigned int Macro_InvokeRules_MatchPattern(const Span& sp, const MacroRules& rules, TokenTree input, const AST::Crate& crate, AST::Module& mod,  ParameterMappings& bound_tts);
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
unsigned int ParameterMappings::get_loop_repeats(const ::std::vector<unsigned int>& iterations, unsigned int loop_idx) const
{
    const auto& list = m_loop_counts.at(loop_idx);
    // Iterate the list, find the first prefix match of `iterations`
    // - `iterations` should always be longer or equal in length to every entry in `list`
    //auto ranges = list.equal_range(iterations);
    for(const auto& e : list)
    {
        ASSERT_BUG(Span(), e.first.size() <= iterations.size(), "Loop " << loop_idx << " iteration path [" << e.first << "] larger than query path [" << iterations << "]");
        if( std::equal(e.first.begin(), e.first.end(), iterations.begin()) )
        {
            return e.second;
        }
    }
    BUG(Span(), "Loop " << loop_idx << " cannot find an iteration count for path [" << iterations << "]");
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

const SimplePatEnt& MacroPatternStream::next()
{
    if( m_peek_cache_valid ) {
        m_peek_cache_valid = false;
        return *m_peek_cache;
    }

    for(;;)
    {
        // If not replaying, and the previous entry was a conditional, record the result of that conditional
        if( !m_condition_replay && m_last_was_cond )
        {
            m_condition_history.push_back(m_condition_met);
        }
        m_last_was_cond = false;
        // End of list? return End entry
        if( m_cur_pos == m_simple_ents.size() ) {
            static SimplePatEnt END = SimplePatEnt::make_End({});
            return END;
        }
        const auto& cur_ent = m_simple_ents[m_cur_pos];
        // If replaying, and this is a conditional
        if( m_condition_replay && cur_ent.is_If() )
        {
            // Skip the conditional (following its target or just skipping over)
            if( (*m_condition_replay)[m_condition_replay_pos++] )
                m_cur_pos = cur_ent.as_If().jump_target;
            else
                m_cur_pos += 1;
            continue ;
        }
        m_cur_pos += 1;
        TU_MATCH_HDRA( (cur_ent), {)
        default:
            if( cur_ent.is_If() )
            {
                m_last_was_cond = true;
                m_condition_met = false;
            }
            return cur_ent;
        TU_ARMA(End, _e)
            BUG(Span(), "Unexpected End");
        TU_ARMA(Jump, e)
            m_cur_pos = e.jump_target;
        TU_ARMA(LoopStart, e) {
            m_current_loops.push_back(e.index);
            m_loop_iterations.push_back(0);
            }
        TU_ARMA(LoopNext, _e) {
            m_loop_iterations.back() += 1;
            }
        TU_ARMA(LoopEnd, _e) {
            assert(!m_loop_iterations.empty());
            assert(!m_current_loops.empty());
            auto loop_index = m_current_loops.back();
            auto num_iter = m_loop_iterations.back();
            m_loop_iterations.pop_back();
            m_current_loops.pop_back();

            // Save this iteration count if replaying
            if( m_condition_replay )
            {
                m_loop_counts[loop_index].insert( std::make_pair(m_loop_iterations, num_iter) );
            }
            }
        }
    }
}

void MacroPatternStream::if_succeeded()
{
    assert(m_cur_pos > 0);
    assert(m_cur_pos <= m_simple_ents.size());
    assert(m_last_was_cond);
    const auto& ent = m_simple_ents[m_cur_pos-1];
    ASSERT_BUG(Span(), ent.is_If(), "Expected If when calling `if_succeeded`, got " << ent);
    const auto& e = ent.as_If();
    ASSERT_BUG(Span(), e.jump_target < m_simple_ents.size(), "Jump target " << e.jump_target << " out of range " << m_simple_ents.size());
    m_cur_pos = e.jump_target;
    m_condition_met = true;
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
    // Used to track a specific invocation for debugging
    static unsigned s_next_log_index;
    unsigned m_log_index;

    const RcString  m_macro_filename;

    const RcString  m_crate_name;
    Span m_invocation_span;
    AST::Edition    m_invocation_edition;

    ParameterMappings m_mappings;
    MacroExpandState    m_state;

    Token   m_next_token;   // used for inserting a single token into the stream
    ::std::unique_ptr<TTStreamO> m_ttstream;
    AST::Edition    m_source_edition;
    Ident::Hygiene  m_hygiene;

public:
    MacroExpander(const MacroExpander& x) = delete;

    MacroExpander(
        const ::std::string& macro_name,
        const Span& sp,
        AST::Edition edition,
        const Ident::Hygiene& parent_hygiene,
        const ::std::vector<MacroExpansionEnt>& contents,
        ParameterMappings mappings,
        RcString crate_name,
        AST::Edition source_edition
    ):
        TokenStream(ParseState()),
        m_log_index(s_next_log_index++),
        m_macro_filename( FMT("Macro:" << macro_name) ),
        m_crate_name( mv$(crate_name) ),
        m_invocation_span( sp ),
        m_invocation_edition( edition ),
        m_mappings( mv$(mappings) ),
        m_state( contents, m_mappings ),
        m_source_edition( source_edition ),
        m_hygiene( Ident::Hygiene::new_scope_chained(parent_hygiene) )
    {
    }

    Position getPosition() const override;
    Span outerSpan() const override { return m_invocation_span; }
    Ident::Hygiene realGetHygiene() const override;
    AST::Edition realGetEdition() const override;
    Token realGetToken() override;
};
unsigned MacroExpander::s_next_log_index = 0;

void Macro_InitDefaults()
{
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
        return InterpolatedFragment( Parse_Pattern(lex, AllowOrPattern::No) );
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
        if( Token::type_is_rword(tok.type()) )
            return InterpolatedFragment( TokenTree(lex.get_edition(), lex.get_hygiene(), tok) );
        else {
            CHECK_TOK(tok, TOK_IDENT);
            return InterpolatedFragment( TokenTree(lex.get_edition(), lex.get_hygiene(), tok) );
        }
    case MacroPatEnt::PAT_VIS:
        return InterpolatedFragment( Parse_Publicity(lex, /*allow_restricted=*/true) );
    case MacroPatEnt::PAT_LIFETIME:
        GET_CHECK_TOK(tok, lex, TOK_LIFETIME);
        return InterpolatedFragment( TokenTree(lex.get_edition(), lex.get_hygiene(), tok) );
    case MacroPatEnt::PAT_LITERAL:
        GET_TOK(tok, lex);
        switch(tok.type())
        {
        case TOK_INTEGER:
        case TOK_FLOAT:
        case TOK_STRING:
        case TOK_BYTESTRING:
        case TOK_RWORD_TRUE:
        case TOK_RWORD_FALSE:
            break;
        default:
            throw ParseError::Unexpected(lex, tok, {TOK_INTEGER, TOK_FLOAT, TOK_STRING, TOK_BYTESTRING, TOK_RWORD_TRUE, TOK_RWORD_FALSE});
        }
        return InterpolatedFragment( TokenTree(lex.get_edition(), lex.get_hygiene(), tok) );
    }
    throw "";
}

/// Parse the input TokenTree according to the `macro_rules!` patterns and return a token stream of the replacement
::std::unique_ptr<TokenStream> Macro_InvokeRules(const char *name, const MacroRules& rules, const Span& sp, TokenTree input, const AST::Crate& crate, AST::Module& mod)
{
    TRACE_FUNCTION_F("'" << name << "', " << input);
    DEBUG("rules.m_hygiene = " << rules.m_hygiene);

    ParameterMappings   bound_tts;
    unsigned int    rule_index = Macro_InvokeRules_MatchPattern(sp, rules, mv$(input), crate, mod,  bound_tts);

    const auto& rule = rules.m_rules.at(rule_index);

    DEBUG( "Using macro '" << name << "' #" << rule_index << " - " << rule.m_contents.size() << " rule contents with " << bound_tts.mappings().size() << " bound values");
    for( unsigned int i = 0; i < ::std::min( bound_tts.mappings().size(), rule.m_param_names.size() ); i ++ )
    {
        DEBUG("- #" << i << " " << rule.m_param_names.at(i) << " = [" << bound_tts.mappings()[i] << "]");
    }
    //bound_tts.dump();

    // Run through the expansion counting the number of times each fragment is used
    Macro_InvokeRules_CountSubstUses(bound_tts, rule.m_contents);

    TokenStream* ret_ptr = new MacroExpander(
        name, sp, crate.m_edition, rules.m_hygiene, rule.m_contents, mv$(bound_tts), rules.m_source_crate == "" ? crate.m_crate_name_real : rules.m_source_crate,
        rules.m_source_crate == "" ? crate.m_edition : crate.m_extern_crates.at(rules.m_source_crate).m_hir->m_edition
        );

    return ::std::unique_ptr<TokenStream>( ret_ptr );
}

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
        case TOK_RWORD_CRATE:
            lex.consume();
            // Require `::` after `crate`
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
        case TOK_IDENT:
            if( TARGETVER_LEAST_1_29 && lex.next_tok().ident().name == "dyn" )
                lex.consume();
            if(0)
        case TOK_RWORD_DYN:
            lex.consume();
        case TOK_RWORD_CRATE:
        case TOK_RWORD_SUPER:
        case TOK_RWORD_SELF:
        case TOK_DOUBLE_COLON:
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
            // ... or ..=
            if( lex.consume_if(TOK_TRIPLE_DOT) || lex.consume_if(TOK_DOUBLE_DOT_EQUAL) )
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
            case TOK_INTERPOLATED_PATH:
            case TOK_DOUBLE_COLON:
            case TOK_RWORD_SELF:
            case TOK_RWORD_SUPER:
            case TOK_RWORD_CRATE:
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

            // Possibly a left-open (or full-open) range literal
            case TOK_DOUBLE_DOT:
            case TOK_DOUBLE_DOT_EQUAL:
            case TOK_TRIPLE_DOT:
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
            case TOK_DOUBLE_DOT_EQUAL:
            case TOK_TRIPLE_DOT:
                lex.consume();
                break;
            case TOK_DOUBLE_DOT:
                lex.consume();
                DEBUG("TOK_DOUBLE_DOT => " << lex.next());
                switch(lex.next())
                {
                case TOK_EOF:
                    return true;
                case TOK_COMMA:
                case TOK_SEMICOLON:
                case TOK_BRACE_CLOSE:
                case TOK_PAREN_CLOSE:
                case TOK_SQUARE_CLOSE:
                   cont = false;
                   break;
                default:
                    break;
                }
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
        if( lex.consume_if(TOK_INTERPOLATED_STMT) )
        {
            return true;
        }
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
    bool consume_vis(TokenStreamRO& lex)
    {
        TRACE_FUNCTION;
        if( lex.consume_if(TOK_INTERPOLATED_VIS) || lex.consume_if(TOK_RWORD_CRATE) )
        {
            return true;
        }
        else if( lex.consume_if(TOK_RWORD_PUB) )
        {
            if( lex.next() == TOK_PAREN_OPEN )
            {
                return consume_tt(lex);
            }
            return true;
        }
        else
        {
            // HACK: If the next character is nothing interesting, then force no match?
            // - TODO: Instead, have `:vis` force a deepeer check
            if( lex.next() == TOK_EOF || lex.next() == TOK_PAREN_CLOSE || lex.next() == TOK_BRACE_CLOSE || lex.next() == TOK_SQUARE_CLOSE )
            {
                return false;
            }
            // NOTE: This is kinda true?
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
        if( (lex.next() == TOK_IDENT && lex.next_tok().ident().name != "union")
         || lex.next() == TOK_RWORD_SELF
         || lex.next() == TOK_RWORD_SUPER
         || lex.next() == TOK_DOUBLE_COLON
         )
        {
            if( !consume_path(lex) )
                return false;
            if( !lex.consume_if(TOK_EXCLAM) )
                return false;
            lex.consume_if(TOK_IDENT);
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
        if( !consume_vis(lex) )
            return false;
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
            if( lex.next_tok().ident().name == "union" )
            {
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
                    if( !lex.consume_if(TOK_IDENT) )
                        return false;
                    if( !H::maybe_generics(lex) )
                        return false;
                    if( !H::maybe_where(lex) )
                        return false;
                    if( lex.next() != TOK_BRACE_OPEN )
                        return false;
                    return consume_tt(lex);
                }
            }
            else if( lex.next_tok().ident().name == "auto" )
            {
                lex.consume();
                if( lex.consume_if(TOK_RWORD_TRAIT) )
                {
                    goto trait;
                }
                else
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
            break;
        // const [unsafe] [extern] fn
        // const FOO
        case TOK_RWORD_CONST:
            lex.consume();
            if(lex.next() == TOK_RWORD_UNSAFE)
                lex.consume();
            if(lex.next() == TOK_RWORD_EXTERN)
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
        case TOK_RWORD_TRAIT:
            lex.consume();
        trait:
            if( !lex.consume_if(TOK_IDENT) )
                return false;

            if( !H::maybe_generics(lex) )
                return false;
            if(lex.next() != TOK_BRACE_OPEN)
                return false;
            if( !consume_tt(lex) )
                return false;
            break;
        case TOK_RWORD_EXTERN:
            lex.consume();
            if( lex.consume_if(TOK_RWORD_CRATE) )
            {
                if( !lex.consume_if(TOK_IDENT) )
                    return false;
                if( lex.consume_if(TOK_RWORD_AS) )
                {
                    if( !lex.consume_if(TOK_IDENT) )
                        return false;
                }
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
            BUG(Span(), "Encountered " << type << " in consume_from_frag");;
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
            if( lex.next() == TOK_IDENT || Token::type_is_rword(lex.next()) ) {
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
                    return consume_expr(lex);
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
        case MacroPatEnt::PAT_VIS:
            return consume_vis(lex);
        case MacroPatEnt::PAT_LIFETIME:
            return lex.consume_if(TOK_LIFETIME);
        case MacroPatEnt::PAT_LITERAL:
            switch(lex.next())
            {
            case TOK_INTEGER:
            case TOK_FLOAT:
            case TOK_STRING:
            case TOK_RWORD_TRUE:
            case TOK_RWORD_FALSE:
                lex.consume();
                return true;
            default:
                return false;
            }
        }
        return true;
    }
}

unsigned int Macro_InvokeRules_MatchPattern(const Span& sp, const MacroRules& rules, TokenTree input, const AST::Crate& crate, AST::Module& mod,  ParameterMappings& bound_tts)
{
    TRACE_FUNCTION_F(rules.m_rules.size() << " options");
    ASSERT_BUG(sp, rules.m_rules.size() > 0, "Empty macro_rules set");

    ::std::vector< ::std::pair<size_t, ::std::vector<bool>> >    matches;
    ::std::vector< std::pair<size_t, eTokenType> >  fail_pos;
    for(size_t i = 0; i < rules.m_rules.size(); i ++)
    {
        auto lex = TokenStreamRO(input);
        auto arm_stream = MacroPatternStream(rules.m_rules[i].m_pattern);

        bool fail = false;
        for(;;)
        {
            const auto pos = arm_stream.cur_pos();
            const auto& pat = arm_stream.next();
            // NOTE: The positions seen by this aren't fully sequential, as `next` steps over jumps/loop control ops
            DEBUG("Arm " << i << " @" << pos << " " << pat);
            if(pat.is_End())
            {
                if( lex.next() != TOK_EOF )
                    fail = true;
                break;
            }
            else if( const auto* e = pat.opt_If() )
            {
                auto lc = lex.clone();
                bool rv = true;
                for(const auto& check : e->ents)
                {
                    if( check.ty != MacroPatEnt::PAT_TOKEN ) {
                        if( !consume_from_frag(lc, check.ty)  )
                        {
                            rv = false;
                            break;
                        }
                    }
                    else
                    {
                        if( lc.next_tok() != check.tok )
                        {
                            rv = false;
                            break;
                        }
                        if( lc.next_tok() != TOK_EOF )
                            lc.consume();
                    }
                }
                if( rv == e->is_equal )
                {
                    DEBUG("- Succeeded");
                    arm_stream.if_succeeded();
                }
            }
            else if( const auto* e = pat.opt_ExpectTok() )
            {
                const auto& tok = lex.next_tok();
                DEBUG("Arm " << i << " @" << pos << " ExpectTok(" << *e << ") == " << tok);
                if( tok != *e )
                {
                    fail = true;
                    break;
                }
                lex.consume();
            }
            else if( const auto* e = pat.opt_ExpectPat() )
            {
                DEBUG("Arm " << i << " @" << pos << " ExpectPat(" << e->type << " => $" << e->idx << ")");
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
            matches.push_back( ::std::make_pair(i, arm_stream.take_history()) );
            DEBUG(i << " MATCHED");
        }
        else
        {
            DEBUG(i << " FAILED");
            fail_pos.push_back( std::make_pair(lex.position(), lex.next()) );
        }
    }

    if( matches.size() == 0 )
    {
        // ERROR!
        // TODO: Keep track of where each arm failed.
        TODO(sp, "No arm matched - " << fail_pos);
    }
    else
    {
        // yay!

        // NOTE: There can be multiple arms active, take the first.
        auto i = matches[0].first;
        const auto& history = matches[0].second;
        DEBUG("Evalulating arm " << i);

        auto lex = TTStreamO(sp, ParseState(), mv$(input));
        lex.parse_state().crate = &crate;
        SET_MODULE(lex, mod);
        auto arm_stream = MacroPatternStream(rules.m_rules[i].m_pattern, &history);

        struct Capture {
            unsigned int    binding_idx;
            ::std::vector<unsigned int> iterations;
            unsigned int    cap_idx;
        };
        ::std::vector<InterpolatedFragment> captures;
        ::std::vector<Capture>  capture_info;

        for(;;)
        {
            const auto& pat = arm_stream.next();
            DEBUG(i << " " << pat);
            if(pat.is_End())
            {
                break;
            }
            else if( pat.is_If() )
            {
                BUG(sp, "Unexpected If pattern during final matching - " << pat);
            }
            else if( const auto* e = pat.opt_ExpectTok() )
            {
                auto tok = lex.getToken();
                DEBUG(i << " ExpectTok(" << *e << ") == " << tok);
                if( tok != *e )
                {
                    ERROR(sp, E0000, "Expected token " << *e << " in match arm, got " << tok);
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
        bound_tts.set_loop_counts(arm_stream.take_loop_counts());
        return i;
    }
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
    // TODO: Return the attached position of the last fetched token
    return Position(m_macro_filename, 0, m_state.top_pos());
}
AST::Edition MacroExpander::realGetEdition() const
{
    if( m_ttstream )
    {
        return m_ttstream->get_edition();
    }
    else
    {
        return m_source_edition;
    }
}
Ident::Hygiene MacroExpander::realGetHygiene() const
{
    if( m_ttstream )
    {
        return m_ttstream->get_hygiene();
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
        DEBUG("[" << m_log_index << "] m_next_token = " << m_next_token);
        return mv$(m_next_token);
    }
    // Then try m_ttstream
    if( m_ttstream.get() )
    {
        Token rv = m_ttstream->getToken();
        DEBUG("[" << m_log_index << "] TTStream present: " << rv);
        if( rv.type() != TOK_EOF )
            return rv;
        m_ttstream.reset();
    }

    // Loop to handle case where $crate expands to nothing
    while( const auto* next_ent_ptr = m_state.next_ent() )
    {
        const auto& ent = *next_ent_ptr;
        TU_MATCH_HDRA( (ent), {)
        TU_ARMA(Token, e) {
            switch(e.type())
            {
            case TOK_IDENT:
            case TOK_LIFETIME: {
                // Rewrite the hygiene of an ident such that idents in the macro explicitly are unique for each expansion
                // - Appears to be a valid option.
                auto ident = e.ident();
                if( ident.hygiene == m_hygiene.get_parent() )
                {
                    ident.hygiene = m_hygiene;
                }
                auto rv = Token(e.type(), std::move(ident));
                DEBUG("[" << m_log_index << "] Updated hygine: " << rv);
                return rv;
                break; }
            default:
                DEBUG("[" << m_log_index << "] Raw token: " << e);
                return e.clone();
            }
            }
        TU_ARMA(NamedValue, e) {
            if( e >> 30 ) {
                switch( e & 0x3FFFFFFF )
                {
                // - XXX: Hack for $crate special name
                case 0:
                    DEBUG("[" << m_log_index << "] Crate name hack");
                    if( m_crate_name == "" )
                    {
                        if( this->edition_after(AST::Edition::Rust2018) )
                        {
                            return Token(TOK_RWORD_CRATE);
                        }
                    }
                    else
                    {
                        m_next_token = Token(TOK_STRING, ::std::string(m_crate_name.c_str()));
                        return Token(TOK_DOUBLE_COLON);
                    }
                    break;
                default:
                    BUG(Span(), "Unknown macro metavar");
                }
            }
            else {
                auto* frag = m_mappings.get(m_state.iterations(), e);
                ASSERT_BUG(this->point_span(), frag, "Cannot find '" << e << "' for " << m_state.iterations());

                bool can_steal = ( m_mappings.dec_count(m_state.iterations(), e) == false );
                DEBUG("[" << m_log_index << "] Insert replacement #" << e << " = " << *frag);
                if( frag->m_type == InterpolatedFragment::TT )
                {
                    auto res_tt = can_steal ? mv$(frag->as_tt()) : frag->as_tt().clone();
                    m_ttstream.reset( new TTStreamO(this->outerSpan(), ParseState(), mv$(res_tt)) );
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
        TU_ARMA(Loop, e) {
            //assert( e.joiner.tok() != TOK_NULL );
            DEBUG("[" << m_log_index << "] Loop joiner " << e.joiner);
            return e.joiner;
            }
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
            TU_MATCH_HDRA( (ent), {)
            TU_ARMA(Token, e) {
                return &ent;
                }
            TU_ARMA(NamedValue, e) {
                return &ent;
                }
            TU_ARMA(Loop, e) {
                assert( !e.controlling_input_loops.empty() );
                unsigned int num_repeats = m_mappings.get_loop_repeats(m_iterations, *e.controlling_input_loops.begin());
                for(auto loop_ident : e.controlling_input_loops)
                {
                    if( loop_ident == *e.controlling_input_loops.begin() )
                        continue ;

                    unsigned int this_repeats = m_mappings.get_loop_repeats(m_iterations, loop_ident);
                    if( this_repeats != num_repeats ) {
                        // TODO: Get the variables involved, or the pattern+output spans
                        ERROR(Span(), E0000, "Mismatch in loop iterations: " << this_repeats << " != " << num_repeats);
                    }
                }
                DEBUG("Looping " << num_repeats << " times based on {" << e.controlling_input_loops << "}");
                // 2. If it's going to repeat, start the loop
                if( num_repeats > 0 )
                {
                    m_offsets.push_back( {0, 0, num_repeats} );
                    m_iterations.push_back( 0 );
                    m_cur_ents = getCurLayer();
                }
                }
            }
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
