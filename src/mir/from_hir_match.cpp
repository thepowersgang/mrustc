/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/from_hir_match.cpp
 * - Conversion of `match` blocks into MIR
 */
#include "from_hir.hpp"
#include <hir_typeck/common.hpp>   // monomorphise_type
#include <algorithm>
#include <numeric>

void MIR_LowerHIR_Match( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val );

#define FIELD_DEREF 255
#define FIELD_INDEX_MAX 128

struct field_path_t
{
    ::std::vector<uint8_t>  data;

    size_t size() const { return data.size(); }
    void push_back(uint8_t v) { data.push_back(v); }
    void pop_back() { data.pop_back(); }
    uint8_t& back() { return data.back(); }

    bool operator==(const field_path_t& x) const { return data == x.data; }

    friend ::std::ostream& operator<<(::std::ostream& os, const field_path_t& x) {
        for(const auto idx : x.data)
            os << "." << static_cast<unsigned int>(idx);
        return os;
    }
};

TAGGED_UNION_EX(PatternRule, (), Any,(
    // Enum variant
    (Variant, struct { unsigned int idx; ::std::vector<PatternRule> sub_rules; }),
    // Slice (includes desired length)
    (Slice, struct { unsigned int len; ::std::vector<PatternRule> sub_rules; }),
    // SplitSlice
    // TODO: How can the negative offsets in the `trailing` be handled correctly? (both here and in the destructure)
    (SplitSlice, struct { unsigned int min_len; unsigned int trailing_len; ::std::vector<PatternRule> leading, trailing; }),
    // Boolean (different to Constant because of how restricted it is)
    (Bool, bool),
    // General value
    (Value, ::MIR::Constant),
    (ValueRange, struct { ::MIR::Constant first, last; }),
    // _ pattern
    (Any, struct {})
    ),
    ( , field_path(mv$(x.field_path)) ), (field_path = mv$(x.field_path);),
    (
        field_path_t    field_path;

        bool operator<(const PatternRule& x) const {
            return this->ord(x) == OrdLess;
        }
        bool operator==(const PatternRule& x) const {
            return this->ord(x) == OrdEqual;
        }
        bool operator!=(const PatternRule& x) const {
            return this->ord(x) != OrdEqual;
        }
        Ordering ord(const PatternRule& x) const;
    )
    );
::std::ostream& operator<<(::std::ostream& os, const PatternRule& x);
/// Constructed set of rules from a pattern
struct PatternRuleset
{
    unsigned int arm_idx;
    unsigned int pat_idx;

    ::std::vector<PatternRule>  m_rules;

    static ::Ordering rule_is_before(const PatternRule& l, const PatternRule& r);

    bool is_before(const PatternRuleset& other) const;
};
/// Generated code for an arm
struct ArmCode {
    ::MIR::BasicBlockId   code = 0;
    bool has_condition = false;
    ::MIR::BasicBlockId   cond_start;
    ::MIR::BasicBlockId   cond_false;
    ::std::vector< ::MIR::BasicBlockId> destructures;   // NOTE: Incomplete

    mutable ::MIR::BasicBlockId cond_fail_tgt = 0;
};

typedef ::std::vector<PatternRuleset>  t_arm_rules;

void MIR_LowerHIR_Match_Simple( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arm_code, ::MIR::BasicBlockId first_cmp_block);
void MIR_LowerHIR_Match_Grouped( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arms_code, ::MIR::BasicBlockId first_cmp_block );
void MIR_LowerHIR_Match_DecisionTree( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arm_code , ::MIR::BasicBlockId first_cmp_block);
/// Helper to construct rules from a passed pattern
struct PatternRulesetBuilder
{
    const StaticTraitResolve&   m_resolve;
    const ::HIR::SimplePath*    m_lang_Box = nullptr;
    bool m_is_impossible;
    ::std::vector<PatternRule>  m_rules;
    field_path_t   m_field_path;

    PatternRulesetBuilder(const StaticTraitResolve& resolve):
        m_resolve(resolve),
        m_is_impossible(false)
    {
        if( resolve.m_crate.m_lang_items.count("owned_box") > 0 ) {
            m_lang_Box = &resolve.m_crate.m_lang_items.at("owned_box");
        }
    }

    void append_from_lit(const Span& sp, const ::HIR::Literal& lit, const ::HIR::TypeRef& ty);
    void append_from(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::TypeRef& ty);
    void push_rule(PatternRule r);
};

class RulesetRef
{
    ::std::vector<PatternRuleset>*  m_rules_vec = nullptr;
    RulesetRef* m_parent = nullptr;
    size_t  m_parent_ofs=0; // If len == 0, this is the innner index, else it's the base
    size_t  m_parent_len=0;
public:
    RulesetRef(::std::vector<PatternRuleset>& rules):
        m_rules_vec(&rules)
    {
    }
    RulesetRef(RulesetRef& parent, size_t start, size_t n):
        m_parent(&parent),
        m_parent_ofs(start),
        m_parent_len(n)
    {
    }
    RulesetRef(RulesetRef& parent, size_t idx):
        m_parent(&parent),
        m_parent_ofs(idx)
    {
    }

    size_t size() const {
        if( m_rules_vec ) {
            return m_rules_vec->size();
        }
        else if( m_parent_len ) {
            return m_parent_len;
        }
        else {
            return m_parent->size();
        }
    }
    RulesetRef slice(size_t s, size_t n) {
        return RulesetRef(*this, s, n);
    }

    const ::std::vector<PatternRule>& operator[](size_t i) const {
        if( m_rules_vec ) {
            return (*m_rules_vec)[i].m_rules;
        }
        else if( m_parent_len ) {
            return (*m_parent)[m_parent_ofs + i];
        }
        else {
            // Fun part - Indexes into inner patterns
            const auto& parent_rule = (*m_parent)[i][m_parent_ofs];
            if(const auto* re = parent_rule.opt_Variant()) {
                return re->sub_rules;
            }
            else {
                throw "TODO";
            }
        }
    }
    void swap(size_t a, size_t b) {
        TRACE_FUNCTION_F(a << ", " << b);
        if( m_rules_vec ) {
            ::std::swap( (*m_rules_vec)[a], (*m_rules_vec)[b] );
        }
        else {
            assert(m_parent);
            if( m_parent_len ) {
                m_parent->swap(m_parent_ofs + a, m_parent_ofs + b);
            }
            else {
                m_parent->swap(a, b);
            }
        }
    }
};

void sort_rulesets(RulesetRef rulesets, size_t idx=0);
void sort_rulesets_inner(RulesetRef rulesets, size_t idx);

// --------------------------------------------------------------------
// CODE
// --------------------------------------------------------------------

// Handles lowering non-trivial matches to MIR
// - Non-trivial means that there's more than one pattern
void MIR_LowerHIR_Match( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val )
{
    // TODO: If any arm moves a non-Copy value, then mark `match_val` as moved
    TRACE_FUNCTION;

    bool fall_back_on_simple = false;

    auto result_val = builder.new_temporary( node.m_res_type );
    auto next_block = builder.new_bb_unlinked();

    // 1. Stop the current block so we can generate code
    auto first_cmp_block = builder.pause_cur_block();


    struct H {
        static bool is_pattern_move(const Span& sp, const MirBuilder& builder, const ::HIR::Pattern& pat) {
            if( pat.m_binding.is_valid() )
            {
                if( pat.m_binding.m_type != ::HIR::PatternBinding::Type::Move)
                    return false;
                return !builder.lvalue_is_copy( sp, builder.get_variable(sp, pat.m_binding.m_slot) );
            }
            TU_MATCHA( (pat.m_data), (e),
            (Any,
                ),
            (Box,
                return is_pattern_move(sp, builder, *e.sub);
                ),
            (Ref,
                return is_pattern_move(sp, builder, *e.sub);
                ),
            (Tuple,
                for(const auto& sub : e.sub_patterns)
                {
                    if( is_pattern_move(sp, builder, sub) )
                        return true;
                }
                ),
            (SplitTuple,
                for(const auto& sub : e.leading)
                {
                    if( is_pattern_move(sp, builder, sub) )
                        return true;
                }
                for(const auto& sub : e.trailing)
                {
                    if( is_pattern_move(sp, builder, sub) )
                        return true;
                }
                ),
            (StructValue,
                // Nothing.
                ),
            (StructTuple,
                for(const auto& sub : e.sub_patterns)
                {
                    if( is_pattern_move(sp, builder, sub) )
                        return true;
                }
                ),
            (Struct,
                for(const auto& fld_pat : e.sub_patterns)
                {
                    if( is_pattern_move(sp, builder, fld_pat.second) )
                        return true;
                }
                ),
            (Value,
                ),
            (Range,
                ),
            (EnumValue,
                ),
            (EnumTuple,
                for(const auto& sub : e.sub_patterns)
                {
                    if( is_pattern_move(sp, builder, sub) )
                        return true;
                }
                ),
            (EnumStruct,
                for(const auto& fld_pat : e.sub_patterns)
                {
                    if( is_pattern_move(sp, builder, fld_pat.second) )
                        return true;
                }
                ),
            (Slice,
                for(const auto& sub : e.sub_patterns)
                {
                    if( is_pattern_move(sp, builder, sub) )
                        return true;
                }
                ),
            (SplitSlice,
                for(const auto& sub : e.leading)
                {
                    if( is_pattern_move(sp, builder, sub) )
                        return true;
                }
                // TODO: Middle binding?
                for(const auto& sub : e.trailing)
                {
                    if( is_pattern_move(sp, builder, sub) )
                        return true;
                }
                )
            )
            return false;
        }
    };

    auto match_scope = builder.new_scope_split(node.span());

    // Map of arm index to ruleset
    ::std::vector< ArmCode> arm_code;
    t_arm_rules arm_rules;
    for(unsigned int arm_idx = 0; arm_idx < node.m_arms.size(); arm_idx ++)
    {
        TRACE_FUNCTION_FR("ARM " << arm_idx, "ARM" << arm_idx);
        /*const*/ auto& arm = node.m_arms[arm_idx];
        const Span& sp = arm.m_code->span();
        ArmCode ac;

        // Register introduced bindings to be dropped on return/diverge within this scope
        auto drop_scope = builder.new_scope_var( arm.m_code->span() );
        // - Define variables from the first pattern
        conv.define_vars_from(node.span(), arm.m_patterns.front());

        auto pat_scope = builder.new_scope_split(node.span());
        for( unsigned int pat_idx = 0; pat_idx < arm.m_patterns.size(); pat_idx ++ )
        {
            const auto& pat = arm.m_patterns[pat_idx];
            // - Convert HIR pattern into ruleset
            auto pat_builder = PatternRulesetBuilder { builder.resolve() };
            pat_builder.append_from(node.span(), pat, node.m_value->m_res_type);
            if( pat_builder.m_is_impossible )
            {
                DEBUG("ARM PAT (" << arm_idx << "," << pat_idx << ") " << pat << " ==> IMPOSSIBLE [" << pat_builder.m_rules << "]");
            }
            else
            {
                DEBUG("ARM PAT (" << arm_idx << "," << pat_idx << ") " << pat << " ==> [" << pat_builder.m_rules << "]");
                arm_rules.push_back( PatternRuleset { arm_idx, pat_idx, mv$(pat_builder.m_rules) } );
            }
            ac.destructures.push_back( builder.new_bb_unlinked() );

            // - Emit code to destructure the matched pattern
            builder.set_cur_block( ac.destructures.back() );
            conv.destructure_from( arm.m_code->span(), pat, match_val.clone(), true );
            // TODO: Previous versions had reachable=false here (causing a use-after-free), would having `true` lead to leaks?
            builder.end_split_arm( arm.m_code->span(), pat_scope, /*reachable=*/true );
            builder.pause_cur_block();
            // NOTE: Paused block resumed upon successful match
        }
        builder.terminate_scope( sp, mv$(pat_scope) );

        ac.code = builder.new_bb_unlinked();

        // Condition
        // NOTE: Lack of drop due to early exit from this arm isn't an issue. All captures must be Copy
        // - The above is rustc E0008 "cannot bind by-move into a pattern guard"
        // TODO: Create a special wrapping scope for the conditions that forces any moves to use a drop flag
        if(arm.m_cond)
        {
            if( H::is_pattern_move(sp, builder, arm.m_patterns[0]) )
                ERROR(sp, E0000, "cannot bind by-move into a pattern guard");
            ac.has_condition = true;
            ac.cond_start = builder.new_bb_unlinked();

            DEBUG("-- Condition Code");
            ac.has_condition = true;
            ac.cond_start = builder.new_bb_unlinked();
            builder.set_cur_block( ac.cond_start );

            auto freeze_scope = builder.new_scope_freeze(arm.m_cond->span());
            auto tmp_scope = builder.new_scope_temp(arm.m_cond->span());
            conv.visit_node_ptr( arm.m_cond );
            auto cond_lval = builder.get_result_in_if_cond(arm.m_cond->span());
            builder.terminate_scope( arm.m_code->span(), mv$(tmp_scope) );
            ac.cond_false = builder.new_bb_unlinked();
            builder.end_block(::MIR::Terminator::make_If({ mv$(cond_lval), ac.code, ac.cond_false }));

            builder.set_cur_block(ac.cond_false);
            builder.end_split_arm(arm.m_cond->span(), match_scope, true, true);
            builder.pause_cur_block();
            builder.terminate_scope( arm.m_code->span(), mv$(freeze_scope) );

            // NOTE: Paused so that later code (which knows what the false branch will be) can end it correctly

            // TODO: What to do with contidionals in the fast model?
            // > Could split the match on each conditional - separating such that if a conditional fails it can fall into the other compatible branches.
            fall_back_on_simple = true;
        }
        else
        {
            ac.has_condition = false;
            ac.cond_start = ~0u;
            ac.cond_false = ~0u;
        }

        // Code
        DEBUG("-- Body Code");

        auto tmp_scope = builder.new_scope_temp(arm.m_code->span());
        builder.set_cur_block( ac.code );
        conv.visit_node_ptr( arm.m_code );

        if( !builder.block_active() && !builder.has_result() ) {
            DEBUG("Arm diverged");
            // Nothing need be done, as the block diverged.
            // - Drops were handled by the diverging block (if not, the below will panic)
            builder.terminate_scope( arm.m_code->span(), mv$(tmp_scope), false );
            builder.terminate_scope( arm.m_code->span(), mv$(drop_scope), false );
            builder.end_split_arm( arm.m_code->span(), match_scope, false );
        }
        else {
            DEBUG("Arm result");
            // - Set result
            auto res = builder.get_result(arm.m_code->span());
            builder.push_stmt_assign( arm.m_code->span(), result_val.clone(), mv$(res) );
            // - Drop all non-moved values from this scope
            builder.terminate_scope( arm.m_code->span(), mv$(tmp_scope) );
            builder.terminate_scope( arm.m_code->span(), mv$(drop_scope) );
            // - Split end match scope
            builder.end_split_arm( arm.m_code->span(), match_scope, true );
            // - Go to the next block
            builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
        }

        arm_code.push_back( mv$(ac) );
    }

    // Sort columns of `arm_rules` to maximise effectiveness
    if( arm_rules[0].m_rules.size() > 1 )
    {
        // TODO: Should columns be sorted within equal sub-arms too?
        ::std::vector<unsigned> column_weights( arm_rules[0].m_rules.size() );
        for(const auto& arm_rule : arm_rules)
        {
            assert( column_weights.size() == arm_rule.m_rules.size() );
            for(unsigned int i = 0; i < arm_rule.m_rules.size(); i++)
            {
                if( !arm_rule.m_rules[i].is_Any() ) {
                    column_weights.at(i) += 1;
                }
            }
        }

        DEBUG("- Column weights = [" << column_weights << "]");
        // - Sort columns such that the largest (most specific) comes first
        ::std::vector<unsigned> columns_sorted(column_weights.size());
        ::std::iota( columns_sorted.begin(), columns_sorted.end(), 0 );
        ::std::sort( columns_sorted.begin(), columns_sorted.end(), [&](auto a, auto b){ return column_weights[a] > column_weights[b]; } );
        DEBUG("- Sorted to = [" << columns_sorted << "]");
        for( auto& arm_rule : arm_rules )
        {
            assert( columns_sorted.size() == arm_rule.m_rules.size() );
            ::std::vector<PatternRule>  sorted;
            sorted.reserve(columns_sorted.size());
            for(auto idx : columns_sorted)
                sorted.push_back( mv$(arm_rule.m_rules[idx]) );
            arm_rule.m_rules = mv$(sorted);
        }
    }

    for(const auto& arm_rule : arm_rules)
    {
        DEBUG("> (" << arm_rule.arm_idx << ", " << arm_rule.pat_idx << ") - " << arm_rule.m_rules
                << (arm_code[arm_rule.arm_idx].has_condition ? " (cond)" : ""));
    }

    // TODO: Remove columns that are all `_`?
    // - Ideally, only accessible structures would be fully destructured like this, making this check redundant

    // Sort rules using the following restrictions:
    // - A rule cannot be reordered across an item that has an overlapping match set
    //  > e.g. nothing can cross _
    //  > equal rules cannot be reordered
    //  > Values cannot cross ranges that contain the value
    //  > This will have to be a bubble sort to ensure that it's correctly stable.
    sort_rulesets(arm_rules);
    DEBUG("Post-sort");
    for(const auto& arm_rule : arm_rules)
    {
        DEBUG("> (" << arm_rule.arm_idx << ", " << arm_rule.pat_idx << ") - " << arm_rule.m_rules
                << (arm_code[arm_rule.arm_idx].has_condition ? " (cond)" : ""));
    }
    // De-duplicate arms (emitting a warning when it happens)
    // - This allows later code to assume that duplicate arms are a codegen bug.
    if( ! arm_rules.empty() )
    {
        for(auto it = arm_rules.begin()+1; it != arm_rules.end(); )
        {
            // If duplicate rule, (and neither is conditional)
            if( (it-1)->m_rules == it->m_rules && !arm_code[it->arm_idx].has_condition && !arm_code[(it-1)->arm_idx].has_condition )
            {
                // Remove
                it = arm_rules.erase(it);
                WARNING(node.m_arms[it->arm_idx].m_code->span(), W0000, "Duplicate match pattern, unreachable code");
            }
            else
            {
                ++ it;
            }
        }
    }

    // TODO: SplitSlice is buggy, make it fall back to simple?

    // TODO: Don't generate inner code until decisions are generated (keeps MIR flow nice)
    // - Challenging, as the decision code needs somewhere to jump to.
    // - Allocating a BB and then rewriting references to it is a possibility.

    if( fall_back_on_simple ) {
        MIR_LowerHIR_Match_Simple( builder, conv, node, mv$(match_val), mv$(arm_rules), mv$(arm_code), first_cmp_block );
    }
    else {
        MIR_LowerHIR_Match_Grouped( builder, conv, node, mv$(match_val), mv$(arm_rules), mv$(arm_code), first_cmp_block );
    }

    builder.set_cur_block( next_block );
    builder.set_result( node.span(), mv$(result_val) );
    builder.terminate_scope( node.span(), mv$(match_scope) );
}

// --------------------------------------------------------------------
// Common Code - Pattern Rules
// --------------------------------------------------------------------
::std::ostream& operator<<(::std::ostream& os, const PatternRule& x)
{
    os << "{" << x.field_path << "}=";
    TU_MATCHA( (x), (e),
    (Any,
        os << "_";
        ),
    // Enum variant
    (Variant,
        os << e.idx << " [" << e.sub_rules << "]";
        ),
    // Slice pattern
    (Slice,
        os << "len=" << e.len << " [" << e.sub_rules << "]";
        ),
    // SplitSlice
    (SplitSlice,
        os << "len>=" << e.min_len << " [" << e.leading << ", ..., " << e.trailing << "]";
        ),
    // Boolean (different to Constant because of how restricted it is)
    (Bool,
        os << (e ? "true" : "false");
        ),
    // General value
    (Value,
        os << e;
        ),
    (ValueRange,
        os << e.first << " ... " << e.last;
        )
    )
    return os;
}

::Ordering PatternRule::ord(const PatternRule& x) const
{
    if(tag() != x.tag())
    {
        return tag() < x.tag() ? ::OrdLess : ::OrdGreater;
    }
    TU_MATCHA( (*this, x), (te, xe),
    (Any, return OrdEqual;),
    (Variant,
        if(te.idx != xe.idx)    return ::ord(te.idx, xe.idx);
        assert( te.sub_rules.size() == xe.sub_rules.size() );
        for(unsigned int i = 0; i < te.sub_rules.size(); i ++)
        {
            auto cmp = te.sub_rules[i].ord( xe.sub_rules[i] );
            if( cmp != ::OrdEqual )
                return cmp;
        }
        return ::OrdEqual;
        ),
    (Slice,
        if(te.len != xe.len)    return ::ord(te.len, xe.len);
        // Wait? Why would the rule count be the same?
        assert( te.sub_rules.size() == xe.sub_rules.size() );
        for(unsigned int i = 0; i < te.sub_rules.size(); i ++)
        {
            auto cmp = te.sub_rules[i].ord( xe.sub_rules[i] );
            if( cmp != ::OrdEqual )
                return cmp;
        }
        return ::OrdEqual;
        ),
    (SplitSlice,
        auto rv = ::ord( te.leading, xe.leading );
        if(rv != OrdEqual)  return rv;
        return ::ord(te.trailing, xe.trailing);
        ),
    (Bool,
        return ::ord( te, xe );
        ),
    (Value,
        return ::ord( te, xe );
        ),
    (ValueRange,
        if( te.first != xe.first )
            return ::ord(te.first, xe.first);
        return ::ord(te.last, xe.last);
        )
    )
    throw "";
}
::Ordering PatternRuleset::rule_is_before(const PatternRule& l, const PatternRule& r)
{
    if( l.tag() != r.tag() ) {
        // Any comes last, don't care about rest
        if( l.tag() < r.tag() )
            return ::OrdGreater;
        else
            return ::OrdLess;
    }

    TU_MATCHA( (l,r), (le,re),
    (Any,
        return ::OrdEqual;
        ),
    (Variant,
        if( le.idx != re.idx )
            return ::ord(le.idx, re.idx);
        assert( le.sub_rules.size() == re.sub_rules.size() );
        for(unsigned int i = 0; i < le.sub_rules.size(); i ++)
        {
            auto cmp = rule_is_before(le.sub_rules[i], re.sub_rules[i]);
            if( cmp != ::OrdEqual )
                return cmp;
        }
        return ::OrdEqual;
        ),
    (Slice,
        if( le.len != re.len )
            return ::ord(le.len, re.len);
        // Wait? Why would the rule count be the same?
        assert( le.sub_rules.size() == re.sub_rules.size() );
        for(unsigned int i = 0; i < le.sub_rules.size(); i ++)
        {
            auto cmp = rule_is_before(le.sub_rules[i], re.sub_rules[i]);
            if( cmp != ::OrdEqual )
                return cmp;
        }
        return ::OrdEqual;
        ),
    (SplitSlice,
        TODO(Span(), "Order PatternRule::SplitSlice");
        ),
    (Bool,
        return ::ord( le, re );
        ),
    (Value,
        TODO(Span(), "Order PatternRule::Value");
        ),
    (ValueRange,
        TODO(Span(), "Order PatternRule::ValueRange");
        )
    )
    throw "";
}

bool PatternRuleset::is_before(const PatternRuleset& other) const
{
    assert( m_rules.size() == other.m_rules.size() );
    for(unsigned int i = 0; i < m_rules.size(); i ++)
    {
        const auto& l = m_rules[i];
        const auto& r = other.m_rules[i];
        auto cmp = rule_is_before(l, r);
        if( cmp != ::OrdEqual )
            return cmp == ::OrdLess;
    }
    return false;
}


void PatternRulesetBuilder::push_rule(PatternRule r)
{
    m_rules.push_back( mv$(r) );
    m_rules.back().field_path = m_field_path;
}

void PatternRulesetBuilder::append_from_lit(const Span& sp, const ::HIR::Literal& lit, const ::HIR::TypeRef& ty)
{
    TRACE_FUNCTION_F("lit="<<lit<<", ty="<<ty<<",   m_field_path=[" << m_field_path << "]");

    TU_MATCHA( (ty.m_data), (e),
    (Infer,   BUG(sp, "Ivar for in match type"); ),
    (Diverge, BUG(sp, "Diverge in match type");  ),
    (Primitive,
        switch(e)
        {
        case ::HIR::CoreType::F32:
        case ::HIR::CoreType::F64: {
            // Yes, this is valid.
            ASSERT_BUG(sp, lit.is_Float(), "Matching floating point type with non-float literal - " << lit);
            double val = lit.as_Float();
            this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Float({ val, e }) ) );
            } break;
        case ::HIR::CoreType::U8:
        case ::HIR::CoreType::U16:
        case ::HIR::CoreType::U32:
        case ::HIR::CoreType::U64:
        case ::HIR::CoreType::U128:
        case ::HIR::CoreType::Usize: {
            ASSERT_BUG(sp, lit.is_Integer(), "Matching integer type with non-integer literal - " << lit);
            uint64_t val = lit.as_Integer();
            this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Uint({val, e}) ) );
            } break;
        case ::HIR::CoreType::I8:
        case ::HIR::CoreType::I16:
        case ::HIR::CoreType::I32:
        case ::HIR::CoreType::I64:
        case ::HIR::CoreType::I128:
        case ::HIR::CoreType::Isize: {
            ASSERT_BUG(sp, lit.is_Integer(), "Matching integer type with non-integer literal - " << lit);
            int64_t val = static_cast<int64_t>( lit.as_Integer() );
            this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Int({ val, e }) ) );
            } break;
        case ::HIR::CoreType::Bool:
            ASSERT_BUG(sp, lit.is_Integer(), "Matching boolean with non-integer literal - " << lit);
            this->push_rule( PatternRule::make_Bool( lit.as_Integer() != 0 ) );
            break;
        case ::HIR::CoreType::Char: {
            // Char is just another name for 'u32'... but with a restricted range
            ASSERT_BUG(sp, lit.is_Integer(), "Matching char with non-integer literal - " << lit);
            uint64_t val = lit.as_Integer();
            this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Uint({ val, e }) ) );
            } break;
        case ::HIR::CoreType::Str:
            BUG(sp, "Hit match over `str` - must be `&str`");
            break;
        }
        ),
    (Tuple,
        m_field_path.push_back(0);
        ASSERT_BUG(sp, lit.is_List(), "Matching tuple with non-list literal - " << lit);
        const auto& list = lit.as_List();
        ASSERT_BUG(sp, e.size() == list.size(), "Matching tuple with mismatched literal size - " << e.size() << " != " << list.size());
        for(unsigned int i = 0; i < e.size(); i ++) {
            this->append_from_lit(sp, list[i], e[i]);
            m_field_path.back() ++;
        }
        m_field_path.pop_back();
        ),
    (Path,
        // This is either a struct destructure or an enum
        TU_MATCHA( (e.binding), (pbe),
        (Unbound,
            BUG(sp, "Encounterd unbound path - " << e.path);
            ),
        (Opaque,
            TODO(sp, "Can an opaque path type be matched with a literal?");
            //ASSERT_BUG(sp, lit.as_List().size() == 0 , "Matching unit struct with non-empty list - " << lit);
            this->push_rule( PatternRule::make_Any({}) );
            ),
        (Struct,
            ASSERT_BUG(sp, lit.is_List(), "Matching struct non-list literal - " << ty << " with " << lit);
            const auto& list = lit.as_List();

            auto monomorph = [&](const auto& ty) {
                auto rv = monomorphise_type(sp, pbe->m_params, e.path.m_data.as_Generic().m_params, ty);
                this->m_resolve.expand_associated_types(sp, rv);
                return rv;
                };
            const auto& str_data = pbe->m_data;
            TU_MATCHA( (str_data), (sd),
            (Unit,
                ASSERT_BUG(sp, list.size() == 0 , "Matching unit struct with non-empty list - " << lit);
                // No rule
                ),
            (Tuple,
                ASSERT_BUG(sp, sd.size() == list.size(), "");
                m_field_path.push_back(0);
                for(unsigned int i = 0; i < sd.size(); i ++ )
                {
                    ::HIR::TypeRef  tmp;
                    const auto& sty_mono = (monomorphise_type_needed(sd[i].ent) ? tmp = monomorph(sd[i].ent) : sd[i].ent);

                    this->append_from_lit(sp, list[i], sty_mono);
                    m_field_path.back() ++;
                }
                m_field_path.pop_back();
                ),
            (Named,
                ASSERT_BUG(sp, sd.size() == list.size(), "");
                m_field_path.push_back(0);
                for( unsigned int i = 0; i < sd.size(); i ++ )
                {
                    const auto& fld_ty = sd[i].second.ent;
                    ::HIR::TypeRef  tmp;
                    const auto& sty_mono = (monomorphise_type_needed(fld_ty) ? tmp = monomorph(fld_ty) : fld_ty);

                    this->append_from_lit(sp, list[i], sty_mono);
                    m_field_path.back() ++;
                }
                )
            )
            ),
        (ExternType,
            TODO(sp, "Match extern type");
            ),
        (Union,
            TODO(sp, "Match union");
            ),
        (Enum,
            ASSERT_BUG(sp, lit.is_Variant(), "Matching enum non-variant literal - " << lit);
            auto var_idx = lit.as_Variant().idx;
            const auto& subval = *lit.as_Variant().val;
            auto monomorph = [&](const auto& ty) {
                auto rv = monomorphise_type(sp, pbe->m_params, e.path.m_data.as_Generic().m_params, ty);
                this->m_resolve.expand_associated_types(sp, rv);
                return rv;
                };

            ASSERT_BUG(sp, var_idx < pbe->num_variants(), "Literal refers to a variant out of range");
            PatternRulesetBuilder   sub_builder { this->m_resolve };
            if( const auto* e = pbe->m_data.opt_Data() )
            {
                const auto& var_def = e->at(var_idx);

                sub_builder.m_field_path = m_field_path;
                sub_builder.m_field_path.push_back(var_idx);

                sub_builder.append_from_lit(sp, subval, monomorph(var_def.type));
            }

            this->push_rule( PatternRule::make_Variant({ var_idx, mv$(sub_builder.m_rules) }) );
            )
        )
        ),
    (Generic,
        // Generics don't destructure, so the only valid pattern is `_`
        TODO(sp, "Match generic with literal?");
        this->push_rule( PatternRule::make_Any({}) );
        ),
    (TraitObject,
        TODO(sp, "Match trait object with literal?");
        ),
    (ErasedType,
        TODO(sp, "Match erased type with literal?");
        ),
    (Array,
        ASSERT_BUG(sp, lit.is_List(), "Matching array with non-list literal - " << lit);
        const auto& list = lit.as_List();
        ASSERT_BUG(sp, e.size_val == list.size(), "Matching array with mismatched literal size - " << e.size_val << " != " << list.size());

        // Sequential match just like tuples.
        m_field_path.push_back(0);
        for(unsigned int i = 0; i < e.size_val; i ++) {
            this->append_from_lit(sp, list[i], *e.inner);
            m_field_path.back() ++;
        }
        m_field_path.pop_back();
        ),
    (Slice,
        ASSERT_BUG(sp, lit.is_List(), "Matching array with non-list literal - " << lit);
        const auto& list = lit.as_List();

        PatternRulesetBuilder   sub_builder { this->m_resolve };
        sub_builder.m_field_path = m_field_path;
        sub_builder.m_field_path.push_back(0);
        for(const auto& val : list)
        {
            sub_builder.append_from_lit( sp, val, *e.inner );
            sub_builder.m_field_path.back() ++;
        }
        // Encodes length check and sub-pattern rules
        this->push_rule( PatternRule::make_Slice({ static_cast<unsigned int>(list.size()), mv$(sub_builder.m_rules) }) );
        ),
    (Borrow,
        m_field_path.push_back( FIELD_DEREF );
        TODO(sp, "Match literal Borrow");
        m_field_path.pop_back();
        ),
    (Pointer,
        TODO(sp, "Match literal with pointer?");
        ),
    (Function,
        ERROR(sp, E0000, "Attempting to match over a functon pointer");
        ),
    (Closure,
        ERROR(sp, E0000, "Attempting to match over a closure");
        )
    )
}
void PatternRulesetBuilder::append_from(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::TypeRef& top_ty)
{
    static ::HIR::Pattern   empty_pattern;
    TRACE_FUNCTION_F("pat="<<pat<<", ty="<<top_ty<<",   m_field_path=[" << m_field_path << "]");
    struct H {
        static uint64_t get_pattern_value_int(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::Pattern::Value& val) {
            TU_MATCH_DEF( ::HIR::Pattern::Value, (val), (e),
            (
                BUG(sp, "Invalid Value type in " << pat);
                ),
            (Integer,
                return e.value;
                ),
            (Named,
                assert(e.binding);
                return e.binding->m_value_res.as_Integer();
                )
            )
            throw "";
        }
        static double get_pattern_value_float(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::Pattern::Value& val) {
            TU_MATCH_DEF( ::HIR::Pattern::Value, (val), (e),
            (
                BUG(sp, "Invalid Value type in " << pat);
                ),
            (Float,
                return e.value;
                ),
            (Named,
                assert(e.binding);
                return e.binding->m_value_res.as_Float();
                )
            )
            throw "";
        }
    };

    const auto* ty_p = &top_ty;
    for(size_t i = 0; i < pat.m_implicit_deref_count; i ++)
    {
        if( !ty_p->m_data.is_Borrow() )
            BUG(sp, "Deref step " << i << "/" << pat.m_implicit_deref_count << " hit a non-borrow " << *ty_p << " from " << top_ty);
        ty_p = &*ty_p->m_data.as_Borrow().inner;
        m_field_path.push_back( FIELD_DEREF );
    }
    const auto& ty = *ty_p;

    // TODO: Outer handling for Value::Named patterns
    // - Convert them into either a pattern, or just a variant of this function that operates on ::HIR::Literal
    //  > It does need a way of handling unknown-value constants (e.g. <GenericT as Foo>::CONST)
    //  > Those should lead to a simple match? Or just a custom rule type that indicates that they're checked early
    TU_IFLET( ::HIR::Pattern::Data, pat.m_data, Value, pe,
        TU_IFLET( ::HIR::Pattern::Value, pe.val, Named, pve,
            if( pve.binding )
            {
                this->append_from_lit(sp, pve.binding->m_value_res, ty);
                for(size_t i = 0; i < pat.m_implicit_deref_count; i ++)
                {
                    m_field_path.pop_back();
                }
                return ;
            }
            else
            {
                TODO(sp, "Match with an unbound constant - " << pve.path);
            }
        )
    )

    TU_MATCH_HDR( (ty.m_data), {)
    TU_ARM(ty.m_data, Infer, e) {
        BUG(sp, "Ivar for in match type");
        }
    TU_ARM(ty.m_data, Diverge, e) {
        // Since ! can never exist, mark this arm as impossible.
        // TODO: Marking as impossible (and not emitting) leads to exhuaustiveness failure.
        //this->m_is_impossible = true;
        }
    TU_ARM(ty.m_data, Primitive, e) {
        TU_MATCH_HDR( (pat.m_data), {)
        default:
            BUG(sp, "Matching primitive with invalid pattern - " << pat);
        TU_ARM(pat.m_data, Any, pe) {
            this->push_rule( PatternRule::make_Any({}) );
            }
        TU_ARM(pat.m_data, Range, pe) {
            switch(e)
            {
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64: {
                double start = H::get_pattern_value_float(sp, pat, pe.start);
                double end   = H::get_pattern_value_float(sp, pat, pe.end  );
                this->push_rule( PatternRule::make_ValueRange( {::MIR::Constant::make_Float({ start, e }), ::MIR::Constant::make_Float({ end, e })} ) );
                } break;
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::U128:
            case ::HIR::CoreType::Usize: {
                uint64_t start = H::get_pattern_value_int(sp, pat, pe.start);
                uint64_t end   = H::get_pattern_value_int(sp, pat, pe.end  );
                this->push_rule( PatternRule::make_ValueRange( {::MIR::Constant::make_Uint({ start, e }), ::MIR::Constant::make_Uint({ end, e })} ) );
                } break;
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::I128:
            case ::HIR::CoreType::Isize: {
                int64_t start = H::get_pattern_value_int(sp, pat, pe.start);
                int64_t end   = H::get_pattern_value_int(sp, pat, pe.end  );
                this->push_rule( PatternRule::make_ValueRange( {::MIR::Constant::make_Int({ start, e }), ::MIR::Constant::make_Int({ end, e })} ) );
                } break;
            case ::HIR::CoreType::Bool:
                BUG(sp, "Can't range match on Bool");
                break;
            case ::HIR::CoreType::Char: {
                uint64_t start = H::get_pattern_value_int(sp, pat, pe.start);
                uint64_t end   = H::get_pattern_value_int(sp, pat, pe.end  );
                this->push_rule( PatternRule::make_ValueRange( {::MIR::Constant::make_Uint({ start, e }), ::MIR::Constant::make_Uint({ end, e })} ) );
                } break;
            case ::HIR::CoreType::Str:
                BUG(sp, "Hit match over `str` - must be `&str`");
                break;
            }
            }
        TU_ARM(pat.m_data, Value, pe) {
            switch(e)
            {
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64: {
                // Yes, this is valid.
                double val = H::get_pattern_value_float(sp, pat, pe.val);
                this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Float({ val, e }) ) );
                } break;
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::U128:
            case ::HIR::CoreType::Usize: {
                uint64_t val = H::get_pattern_value_int(sp, pat, pe.val);
                this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Uint({ val, e }) ) );
                } break;
            case ::HIR::CoreType::Char: {
                // Char is just another name for 'u32'... but with a restricted range
                uint64_t val = H::get_pattern_value_int(sp, pat, pe.val);
                this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Uint({ val, e }) ) );
                } break;
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::I128:
            case ::HIR::CoreType::Isize: {
                int64_t val = H::get_pattern_value_int(sp, pat, pe.val);
                this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Int({ val, e }) ) );
                } break;
            case ::HIR::CoreType::Bool:
                // TODO: Support values from `const` too
                this->push_rule( PatternRule::make_Bool( pe.val.as_Integer().value != 0 ) );
                break;
            case ::HIR::CoreType::Str:
                BUG(sp, "Hit match over `str` - must be `&str`");
                break;
            }
            }
        }
        }
    TU_ARM(ty.m_data, Tuple, e) {
        m_field_path.push_back(0);
        TU_MATCH_DEF(::HIR::Pattern::Data, (pat.m_data), (pe),
        ( BUG(sp, "Matching tuple with invalid pattern - " << pat); ),
        (Any,
            for(const auto& sty : e) {
                this->append_from(sp, empty_pattern, sty);
                m_field_path.back() ++;
            }
            ),
        (Tuple,
            assert(e.size() == pe.sub_patterns.size());
            for(unsigned int i = 0; i < e.size(); i ++) {
                this->append_from(sp, pe.sub_patterns[i], e[i]);
                m_field_path.back() ++;
            }
            ),
        (SplitTuple,
            assert(e.size() >= pe.leading.size() + pe.trailing.size());
            unsigned trailing_start = e.size() - pe.trailing.size();
            for(unsigned int i = 0; i < e.size(); i ++) {
                if( i < pe.leading.size() )
                    this->append_from(sp, pe.leading[i], e[i]);
                else if( i < trailing_start )
                    this->append_from(sp, ::HIR::Pattern(), e[i]);
                else
                    this->append_from(sp, pe.trailing[i-trailing_start], e[i]);
                m_field_path.back() ++;
            }
            )
        )
        m_field_path.pop_back();
        }
    TU_ARM(ty.m_data, Path, e) {
        // This is either a struct destructure or an enum
        TU_MATCH_HDRA( (e.binding), {)
        TU_ARMA(Unbound, pbe) {
            BUG(sp, "Encounterd unbound path - " << e.path);
            }
        TU_ARMA(Opaque, be) {
            TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
            ( BUG(sp, "Matching opaque type with invalid pattern - " << pat); ),
            (Any,
                this->push_rule( PatternRule::make_Any({}) );
                )
            )
            }
        TU_ARMA(Struct, pbe) {
            auto monomorph = [&](const auto& ty) {
                auto rv = monomorphise_type(sp, pbe->m_params, e.path.m_data.as_Generic().m_params, ty);
                this->m_resolve.expand_associated_types(sp, rv);
                return rv;
                };
            const auto& str_data = pbe->m_data;

            if( m_lang_Box && e.path.m_data.as_Generic().m_path == *m_lang_Box )
            {
                const auto& inner_ty = e.path.m_data.as_Generic().m_params.m_types.at(0);
                TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
                ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
                (Any,
                    // _ on a box, recurse into the box type.
                    m_field_path.push_back(FIELD_DEREF);
                    this->append_from(sp, empty_pattern, inner_ty);
                    m_field_path.pop_back();
                    ),
                (Box,
                    m_field_path.push_back(FIELD_DEREF);
                    this->append_from(sp, *pe.sub, inner_ty);
                    m_field_path.pop_back();
                    )
                )
                break;
            }
            TU_MATCHA( (str_data), (sd),
            (Unit,
                TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
                ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
                (Any,
                    // _ on a unit-like type, unconditional
                    ),
                (StructValue,
                    // Unit-like struct value, nothing to match (it's unconditional)
                    ),
                (Value,
                    // Unit-like struct value, nothing to match (it's unconditional)
                    )
                )
                ),
            (Tuple,
                m_field_path.push_back(0);
                TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
                ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
                (Any,
                    // - Recurse into type using an empty pattern
                    for(const auto& fld : sd)
                    {
                        ::HIR::TypeRef  tmp;
                        const auto& sty_mono = (monomorphise_type_needed(fld.ent) ? tmp = monomorph(fld.ent) : fld.ent);
                        this->append_from(sp, empty_pattern, sty_mono);
                        m_field_path.back() ++;
                    }
                    ),
                (StructTuple,
                    assert( sd.size() == pe.sub_patterns.size() );
                    for(unsigned int i = 0; i < sd.size(); i ++)
                    {
                        const auto& fld = sd[i];
                        const auto& fld_pat = pe.sub_patterns[i];

                        ::HIR::TypeRef  tmp;
                        const auto& sty_mono = (monomorphise_type_needed(fld.ent) ? tmp = monomorph(fld.ent) : fld.ent);
                        this->append_from(sp, fld_pat, sty_mono);
                        m_field_path.back() ++;
                    }
                    )
                )
                m_field_path.pop_back();
                ),
            (Named,
                TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
                ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
                (Any,
                    m_field_path.push_back(0);
                    for(const auto& fld : sd)
                    {
                        ::HIR::TypeRef  tmp;
                        const auto& sty_mono = (monomorphise_type_needed(fld.second.ent) ? tmp = monomorph(fld.second.ent) : fld.second.ent);
                        this->append_from(sp, empty_pattern, sty_mono);
                        m_field_path.back() ++;
                    }
                    m_field_path.pop_back();
                    ),
                (Struct,
                    m_field_path.push_back(0);
                    // NOTE: Sort field patterns to ensure that patterns are in order between arms
                    for(const auto& fld : sd)
                    {
                        ::HIR::TypeRef  tmp;
                        const auto& sty_mono = (monomorphise_type_needed(fld.second.ent) ? tmp = monomorph(fld.second.ent) : fld.second.ent);

                        auto it = ::std::find_if( pe.sub_patterns.begin(), pe.sub_patterns.end(), [&](const auto& x){ return x.first == fld.first; } );
                        if( it == pe.sub_patterns.end() )
                        {
                            this->append_from(sp, empty_pattern, sty_mono);
                        }
                        else
                        {
                            this->append_from(sp, it->second, sty_mono);
                        }
                        m_field_path.back() ++;
                    }
                    m_field_path.pop_back();
                    )
                )
                )
            )
            }
        TU_ARMA(Union, pbe) {
            TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
            ( TODO(sp, "Match over union - " << ty << " with " << pat); ),
            (Any,
                this->push_rule( PatternRule::make_Any({}) );
                )
            )
            }
        TU_ARMA(ExternType, pbe) {
            TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
            ( BUG(sp, "Matching extern type with invalid pattern - " << pat); ),
            (Any,
                this->push_rule( PatternRule::make_Any({}) );
                )
            )
            }
        TU_ARMA(Enum, pbe) {
            auto monomorph = [&](const auto& ty) {
                auto rv = monomorphise_type(sp, pbe->m_params, e.path.m_data.as_Generic().m_params, ty);
                this->m_resolve.expand_associated_types(sp, rv);
                return rv;
                };
            TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
            ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
            (Any,
                this->push_rule( PatternRule::make_Any({}) );
                ),
            (Value,
                if( ! pe.val.is_Named() )
                    BUG(sp, "Match not allowed, " << ty << " with " << pat);
                // TODO: If the value of this constant isn't known at this point (i.e. it won't be until monomorphisation)
                //       emit a special type of rule.
                TODO(sp, "Match enum with const - " << pat);
                ),
            (EnumValue,
                this->push_rule( PatternRule::make_Variant( {pe.binding_idx, {} } ) );
                ),
            (EnumTuple,
                const auto& variants = pe.binding_ptr->m_data.as_Data();
                const auto& var_def = variants.at(pe.binding_idx);
                const auto& str = *var_def.type.m_data.as_Path().binding.as_Struct();
                const auto& fields_def = str.m_data.as_Tuple();

                // TODO: Unify with the struct pattern code?
                PatternRulesetBuilder   sub_builder { this->m_resolve };
                sub_builder.m_field_path = m_field_path;
                sub_builder.m_field_path.push_back(pe.binding_idx);
                sub_builder.m_field_path.push_back(0);
                for( unsigned int i = 0; i < pe.sub_patterns.size(); i ++ )
                {
                    sub_builder.m_field_path.back() = i;
                    const auto& subpat = pe.sub_patterns[i];
                    const auto& ty_tpl = fields_def[i].ent;

                    ::HIR::TypeRef  tmp;
                    const auto& subty = (monomorphise_type_needed(ty_tpl) ? tmp = monomorph(ty_tpl) : ty_tpl);

                    sub_builder.append_from( sp, subpat, subty );
                }
                if( sub_builder.m_is_impossible )
                    this->m_is_impossible = true;
                this->push_rule( PatternRule::make_Variant({ pe.binding_idx, mv$(sub_builder.m_rules) }) );
                ),
            (EnumStruct,
                const auto& variants = pe.binding_ptr->m_data.as_Data();
                const auto& var_def = variants.at(pe.binding_idx);
                const auto& str = *var_def.type.m_data.as_Path().binding.as_Struct();
                const auto& fields_def = str.m_data.as_Named();

                // 1. Create a vector of pattern indexes for each field in the variant.
                ::std::vector<unsigned int> tmp;
                tmp.resize( fields_def.size(), ~0u );
                for( unsigned int i = 0; i < pe.sub_patterns.size(); i ++ )
                {
                    const auto& fld_pat = pe.sub_patterns[i];
                    unsigned idx = ::std::find_if( fields_def.begin(), fields_def.end(), [&](const auto& x){ return x.first == fld_pat.first; } ) - fields_def.begin();
                    assert(idx < tmp.size());
                    assert(tmp[idx] == ~0u);
                    tmp[idx] = i;
                }
                // 2. Iterate this list and recurse on the patterns
                PatternRulesetBuilder   sub_builder { this->m_resolve };
                sub_builder.m_field_path = m_field_path;
                sub_builder.m_field_path.push_back(pe.binding_idx);
                sub_builder.m_field_path.push_back(0);
                for( unsigned int i = 0; i < tmp.size(); i ++ )
                {
                    sub_builder.m_field_path.back() = i;

                    auto subty = monomorph(fields_def[i].second.ent);
                    if( tmp[i] == ~0u ) {
                        sub_builder.append_from( sp, ::HIR::Pattern(), subty );
                    }
                    else {
                        const auto& subpat = pe.sub_patterns[ tmp[i] ].second;
                        sub_builder.append_from( sp, subpat, subty );
                    }
                }
                if( sub_builder.m_is_impossible )
                    this->m_is_impossible = true;
                this->push_rule( PatternRule::make_Variant({ pe.binding_idx, mv$(sub_builder.m_rules) }) );
                )
            )
            }
        }
        }
    TU_ARM(ty.m_data, Generic, e) {
        // Generics don't destructure, so the only valid pattern is `_`
        TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
        ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
        (Any,
            this->push_rule( PatternRule::make_Any({}) );
            )
        )
        }
    TU_ARM(ty.m_data, TraitObject, e) {
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a trait object");
        }
        }
    TU_ARM(ty.m_data, ErasedType, e) {
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over an erased type");
        }
        }
    TU_ARM(ty.m_data, Array, e) {
        // Sequential match just like tuples.
        m_field_path.push_back(0);
        TU_MATCH_DEF(::HIR::Pattern::Data, (pat.m_data), (pe),
        ( BUG(sp, "Matching array with invalid pattern - " << pat); ),
        (Any,
            for(unsigned int i = 0; i < e.size_val; i ++) {
                this->append_from(sp, empty_pattern, *e.inner);
                m_field_path.back() ++;
            }
            ),
        (Slice,
            assert(e.size_val == pe.sub_patterns.size());
            for(unsigned int i = 0; i < e.size_val; i ++) {
                this->append_from(sp, pe.sub_patterns[i], *e.inner);
                m_field_path.back() ++;
            }
            ),
        (SplitSlice,
            TODO(sp, "Match over array with SplitSlice pattern - " << pat);
            )
        )
        m_field_path.pop_back();
        }
    TU_ARM(ty.m_data, Slice, e) {
        TU_MATCH_DEF(::HIR::Pattern::Data, (pat.m_data), (pe),
        (
            BUG(sp, "Matching over [T] with invalid pattern - " << pat);
            ),
        (Any,
            this->push_rule( PatternRule::make_Any({}) );
            ),
        (Slice,
            // Sub-patterns
            PatternRulesetBuilder   sub_builder { this->m_resolve };
            sub_builder.m_field_path = m_field_path;
            sub_builder.m_field_path.push_back(0);
            ASSERT_BUG(sp, pe.sub_patterns.size() < FIELD_INDEX_MAX, "Too many slice rules to fit encodng");
            for(const auto& subpat : pe.sub_patterns)
            {
                sub_builder.append_from( sp, subpat, *e.inner );
                sub_builder.m_field_path.back() ++;
            }

            // Encodes length check and sub-pattern rules
            this->push_rule( PatternRule::make_Slice({ static_cast<unsigned int>(pe.sub_patterns.size()), mv$(sub_builder.m_rules) }) );
            ),
        (SplitSlice,
            PatternRulesetBuilder   sub_builder { this->m_resolve };
            sub_builder.m_field_path = m_field_path;
            ASSERT_BUG(sp, pe.leading.size() < FIELD_INDEX_MAX, "Too many leading slice rules to fit encodng");
            sub_builder.m_field_path.push_back(0);
            for(const auto& subpat : pe.leading)
            {
                sub_builder.append_from( sp, subpat, *e.inner );
                sub_builder.m_field_path.back() ++;
            }
            auto leading = mv$(sub_builder.m_rules);

            if( pe.trailing.size() )
            {
                // Needs a way of encoding the negative offset in the field path
                // - For now, just use a very high number (and assert that it's not more than 128)
                ASSERT_BUG(sp, pe.trailing.size() < FIELD_INDEX_MAX, "Too many trailing slice rules to fit encodng");
                sub_builder.m_field_path.back() = FIELD_INDEX_MAX + (FIELD_INDEX_MAX - pe.trailing.size());
                for(const auto& subpat : pe.trailing)
                {
                    sub_builder.append_from( sp, subpat, *e.inner );
                    sub_builder.m_field_path.back() ++;
                }
            }
            auto trailing = mv$(sub_builder.m_rules);

            this->push_rule( PatternRule::make_SplitSlice({
                static_cast<unsigned int>(pe.leading.size() + pe.trailing.size()),
                static_cast<unsigned int>(pe.trailing.size()),
                mv$(leading), mv$(trailing)
                }) );
            )
        )
        }
    TU_ARM(ty.m_data, Borrow, e) {
        m_field_path.push_back( FIELD_DEREF );
        TU_MATCH_HDR( (pat.m_data), {)
        default:
            BUG(sp, "Matching borrow invalid pattern - " << ty << " with " << pat);
        TU_ARM(pat.m_data, Any, pe) {
            this->append_from( sp, empty_pattern, *e.inner );
            }
        TU_ARM(pat.m_data, Ref, pe) {
            this->append_from( sp, *pe.sub, *e.inner );
            }
        TU_ARM(pat.m_data, Value, pe) {
            // TODO: Check type?
            if( pe.val.is_String() ) {
                const auto& s = pe.val.as_String();
                this->push_rule( PatternRule::make_Value(s) );
            }
            else if( pe.val.is_ByteString() ) {
                const auto& s = pe.val.as_ByteString().v;
                ::std::vector<uint8_t>  data;
                data.reserve(s.size());
                for(auto c : s)
                    data.push_back(c);

                this->push_rule( PatternRule::make_Value( mv$(data) ) );
            }
            // TODO: Handle named values
            else {
                BUG(sp, "Matching borrow invalid pattern - " << pat);
            }
            }
        }
        m_field_path.pop_back();
        }
    TU_ARM(ty.m_data, Pointer, e) {
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a pointer");
        }
        }
    TU_ARM(ty.m_data, Function, e) {
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a functon pointer");
        }
        }
    TU_ARM(ty.m_data, Closure, e) {
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a closure");
        }
        }
    }
    for(size_t i = 0; i < pat.m_implicit_deref_count; i ++)
    {
        m_field_path.pop_back();
    }
}

namespace {
    // Order rules ignoring inner rules
    Ordering ord_rule_compatible(const PatternRule& a, const PatternRule& b)
    {
        if(a.tag() != b.tag())
            return ::ord( (unsigned)a.tag(), (unsigned)b.tag() );

        TU_MATCHA( (a, b), (ae, be),
        (Any,
            return OrdEqual;
            ),
        (Variant,
            return ::ord(ae.idx, be.idx);
            ),
        (Slice,
            return ::ord(ae.len, be.len);
            ),
        (SplitSlice,
            auto v = ::ord(ae.leading.size(), be.leading.size());
            if(v != OrdEqual)   return v;
            v = ::ord(ae.trailing.size(), be.trailing.size());
            if(v != OrdEqual)   return v;
            return OrdEqual;
            ),
        (Bool,
            return ::ord(ae, be);
            ),
        (Value,
            return ::ord(ae, be);
            ),
        (ValueRange,
            auto v = ::ord(ae.first, be.first);
            if(v != OrdEqual)   return v;
            return ::ord(ae.last, be.last);
            )
        )
        throw "";
    }
    bool rule_compatible(const PatternRule& a, const PatternRule& b)
    {
        return ord_rule_compatible(a,b) == OrdEqual;
    }

    bool rules_overlap(const PatternRule& a, const PatternRule& b)
    {
        if( a.is_Any() || b.is_Any() )
            return true;

        // Defensive: If a constant is encountered, assume it overlaps with anything
        if(const auto* ae = a.opt_Value()) {
            if(ae->is_Const())
                return true;
        }
        if(const auto* be = b.opt_Value()) {
            if(be->is_Const())
                return true;
        }

        // Value Range: Overlaps with contained values.
        if(const auto* ae = a.opt_ValueRange() )
        {
            if(const auto* be = b.opt_Value() )
            {
                return ( ae->first <= *be && *be <= ae->last );
            }
            else if( const auto* be = b.opt_ValueRange() )
            {
                // Start of B within A
                if( ae->first <= be->first && be->first <= ae->last )
                    return true;
                // End of B within A
                if( ae->first <= be->last && be->last <= ae->last )
                    return true;
                // Start of A within B
                if( be->first <= ae->first && ae->first <= be->last )
                    return true;
                // End of A within B
                if( be->first <= ae->last && ae->last <= be->last )
                    return true;

                // Disjoint
                return false;
            }
            else
            {
                TODO(Span(), "Check overlap of " << a << " and " << b);
            }
        }
        if(const auto* be = b.opt_ValueRange())
        {
            if(const auto* ae = a.opt_Value() )
            {
                return (be->first <= *ae && *ae <= be->last);
            }
            // Note: A can't be ValueRange
            else
            {
                TODO(Span(), "Check overlap of " << a << " and " << b);
            }
        }

        // SplitSlice patterns overlap with other SplitSlice patterns and larger slices
        if(const auto* ae = a.opt_SplitSlice())
        {
            if( b.is_SplitSlice() )
            {
                return true;
            }
            else if( const auto* be = b.opt_Slice() )
            {
                return be->len >= ae->min_len;
            }
            else
            {
                TODO(Span(), "Check overlap of " << a << " and " << b);
            }
        }
        if(const auto* be = b.opt_SplitSlice())
        {
            if( const auto* ae = a.opt_Slice() )
            {
                return ae->len >= be->min_len;
            }
            else
            {
                TODO(Span(), "Check overlap of " << a << " and " << b);
            }
        }

        // Otherwise, If rules are approximately equal, they overlap
        return ( ord_rule_compatible(a, b) == OrdEqual );
    }
}
void sort_rulesets(RulesetRef rulesets, size_t idx)
{
    if(rulesets.size() < 2)
        return ;

    bool found_non_any = false;
    for(size_t i = 0; i < rulesets.size(); i ++)
        if( !rulesets[i][idx].is_Any() )
            found_non_any = true;
    if( found_non_any )
    {
        TRACE_FUNCTION_F(idx);
        for(size_t i = 0; i < rulesets.size(); i ++)
            DEBUG("- " << i << ": " << rulesets[i]);

        bool action_taken;
        do
        {
            action_taken = false;
            for(size_t i = 0; i < rulesets.size()-1; i ++)
            {
                if( rules_overlap(rulesets[i][idx], rulesets[i+1][idx]) )
                {
                    // Don't move
                }
                else if( ord_rule_compatible(rulesets[i][idx], rulesets[i+1][idx]) == OrdGreater )
                {
                    rulesets.swap(i, i+1);
                    action_taken = true;
                }
                else
                {
                }
            }
        } while(action_taken);
        for(size_t i = 0; i < rulesets.size(); i ++)
            DEBUG("- " << i << ": " << rulesets[i]);

        // TODO: Print sorted ruleset

        // Where compatible, sort insides
        size_t  start = 0;
        for(size_t i = 1; i < rulesets.size(); i++)
        {
            if( ord_rule_compatible(rulesets[i][idx], rulesets[start][idx]) != OrdEqual )
            {
                sort_rulesets_inner(rulesets.slice(start, i-start), idx);
                start = i;
            }
        }
        sort_rulesets_inner(rulesets.slice(start, rulesets.size()-start), idx);

        // Iterate onwards where rules are equal
        if( idx + 1 < rulesets[0].size() )
        {
            size_t  start = 0;
            for(size_t i = 1; i < rulesets.size(); i++)
            {
                if( rulesets[i][idx] != rulesets[start][idx] )
                {
                    sort_rulesets(rulesets.slice(start, i-start), idx+1);
                    start = i;
                }
            }
            sort_rulesets(rulesets.slice(start, rulesets.size()-start), idx+1);
        }
    }
    else
    {
        if( idx + 1 < rulesets[0].size() )
        {
            sort_rulesets(rulesets, idx + 1);
        }
    }
}
void sort_rulesets_inner(RulesetRef rulesets, size_t idx)
{
    TRACE_FUNCTION_F(idx << " - " << rulesets[0][idx].tag_str());
    if( const auto* re = rulesets[0][idx].opt_Variant() )
    {
        // Sort rules based on contents of enum
        if( re->sub_rules.size() > 0 )
        {
            sort_rulesets(RulesetRef(rulesets, idx), 0);
        }
    }
}

namespace {
    void get_ty_and_val(
        const Span& sp, MirBuilder& builder,
        const ::HIR::TypeRef& top_ty, const ::MIR::LValue& top_val,
        const field_path_t& field_path, unsigned int field_path_ofs,
        /*Out ->*/ ::HIR::TypeRef& out_ty, ::MIR::LValue& out_val
        )
    {
        const StaticTraitResolve& resolve = builder.resolve();
        ::MIR::LValue   lval = top_val.clone();
        ::HIR::TypeRef  tmp_ty;
        const ::HIR::TypeRef* cur_ty = &top_ty;

        // TODO: Cache the correspondance of path->type (lval can be inferred)
        ASSERT_BUG(sp, field_path_ofs <= field_path.size(), "Field path offset " << field_path_ofs << " is larger than the path [" << field_path << "]");
        for(unsigned int i = field_path_ofs; i < field_path.size(); i ++ )
        {
            unsigned idx = field_path.data[i];

            TU_MATCHA( (cur_ty->m_data), (e),
            (Infer,   BUG(sp, "Ivar for in match type"); ),
            (Diverge, BUG(sp, "Diverge in match type");  ),
            (Primitive,
                BUG(sp, "Destructuring a primitive");
                ),
            (Tuple,
                ASSERT_BUG(sp, idx < e.size(), "Tuple index out of range");
                lval = ::MIR::LValue::new_Field(mv$(lval), idx);
                cur_ty = &e[idx];
                ),
            (Path,
                if( idx == FIELD_DEREF ) {
                    // TODO: Check that the path is Box
                    lval = ::MIR::LValue::new_Deref( mv$(lval) );
                    cur_ty = &e.path.m_data.as_Generic().m_params.m_types.at(0);
                    break;
                }
                TU_MATCHA( (e.binding), (pbe),
                (Unbound,
                    BUG(sp, "Encounterd unbound path - " << e.path);
                    ),
                (Opaque,
                    BUG(sp, "Destructuring an opaque type - " << *cur_ty);
                    ),
                (ExternType,
                    BUG(sp, "Destructuring an extern type - " << *cur_ty);
                    ),
                (Struct,
                    // TODO: Should this do a call to expand_associated_types?
                    auto monomorph = [&](const auto& ty) {
                        auto rv = monomorphise_type(sp, pbe->m_params, e.path.m_data.as_Generic().m_params, ty);
                        resolve.expand_associated_types(sp, rv);
                        return rv;
                        };
                    TU_MATCHA( (pbe->m_data), (fields),
                    (Unit,
                        BUG(sp, "Destructuring an unit-like tuple - " << *cur_ty);
                        ),
                    (Tuple,
                        assert( idx < fields.size() );
                        const auto& fld = fields[idx];
                        if( monomorphise_type_needed(fld.ent) ) {
                            tmp_ty = monomorph(fld.ent);
                            cur_ty = &tmp_ty;
                        }
                        else {
                            cur_ty = &fld.ent;
                        }
                        lval = ::MIR::LValue::new_Field(mv$(lval), idx);
                        ),
                    (Named,
                        assert( idx < fields.size() );
                        const auto& fld = fields[idx].second;
                        if( monomorphise_type_needed(fld.ent) ) {
                            tmp_ty = monomorph(fld.ent);
                            cur_ty = &tmp_ty;
                        }
                        else {
                            cur_ty = &fld.ent;
                        }
                        lval = ::MIR::LValue::new_Field(mv$(lval), idx);
                        )
                    )
                    ),
                (Union,
                    auto monomorph = [&](const auto& ty) {
                        auto rv = monomorphise_type(sp, pbe->m_params, e.path.m_data.as_Generic().m_params, ty);
                        resolve.expand_associated_types(sp, rv);
                        return rv;
                        };
                    assert(idx < pbe->m_variants.size());
                    const auto& fld = pbe->m_variants[idx];
                    if( monomorphise_type_needed(fld.second.ent) ) {
                        tmp_ty = monomorph(fld.second.ent);
                        cur_ty = &tmp_ty;
                    }
                    else {
                        cur_ty = &fld.second.ent;
                    }
                    lval = ::MIR::LValue::new_Downcast(mv$(lval), idx);
                    ),
                (Enum,
                    auto monomorph_to_ptr = [&](const auto& ty)->const auto* {
                        if( monomorphise_type_needed(ty) ) {
                            auto rv = monomorphise_type(sp, pbe->m_params, e.path.m_data.as_Generic().m_params, ty);
                            resolve.expand_associated_types(sp, rv);
                            tmp_ty = mv$(rv);
                            return &tmp_ty;
                        }
                        else {
                            return &ty;
                        }
                        };
                    ASSERT_BUG(sp, pbe->m_data.is_Data(), "Value enum being destructured - " << *cur_ty);
                    const auto& variants = pbe->m_data.as_Data();
                    ASSERT_BUG(sp, idx < variants.size(), "Variant index (" << idx << ") out of range (" << variants.size() <<  ") for enum " << *cur_ty);
                    const auto& var = variants[idx];

                    cur_ty = monomorph_to_ptr(var.type);
                    lval = ::MIR::LValue::new_Downcast(mv$(lval), idx);
                    )
                )
                ),
            (Generic,
                BUG(sp, "Destructuring a generic - " << *cur_ty);
                ),
            (TraitObject,
                BUG(sp, "Destructuring a trait object - " << *cur_ty);
                ),
            (ErasedType,
                BUG(sp, "Destructuring an erased type - " << *cur_ty);
                ),
            (Array,
                assert(idx < e.size_val);
                cur_ty = &*e.inner;
                if( idx < FIELD_INDEX_MAX )
                    lval = ::MIR::LValue::new_Field(mv$(lval), idx);
                else {
                    idx -= FIELD_INDEX_MAX;
                    idx = FIELD_INDEX_MAX - idx;
                    TODO(sp, "Index " << idx << " from end of array " << lval);
                }
                ),
            (Slice,
                cur_ty = &*e.inner;
                if( idx < FIELD_INDEX_MAX )
                    lval = ::MIR::LValue::new_Field(mv$(lval), idx);
                else {
                    idx -= FIELD_INDEX_MAX;
                    idx = FIELD_INDEX_MAX - idx;
                    // 1. Create an LValue containing the size of this slice subtract `idx`
                    auto len_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue::make_DstMeta({ builder.get_ptr_to_dst(sp, lval) }));
                    auto sub_val = ::MIR::Param(::MIR::Constant::make_Uint({ idx, ::HIR::CoreType::Usize }));
                    auto ofs_val = builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue::make_BinOp({ mv$(len_lval), ::MIR::eBinOp::SUB, mv$(sub_val) }) );
                    // 2. Return _Index with that value
                    lval = ::MIR::LValue::new_Index(mv$(lval), ofs_val.as_Local());
                }
                ),
            (Borrow,
                ASSERT_BUG(sp, idx == FIELD_DEREF, "Destructure of borrow doesn't correspond to a deref in the path");
                DEBUG(i << " " << *cur_ty << " - " << cur_ty << " " << &tmp_ty);
                if( cur_ty == &tmp_ty ) {
                    auto ip = mv$(tmp_ty.m_data.as_Borrow().inner);
                    tmp_ty = mv$(*ip);
                }
                else {
                    cur_ty = &*e.inner;
                }
                DEBUG(i << " " << *cur_ty);
                lval = ::MIR::LValue::new_Deref(mv$(lval));
                ),
            (Pointer,
                ERROR(sp, E0000, "Attempting to match over a pointer");
                ),
            (Function,
                ERROR(sp, E0000, "Attempting to match over a functon pointer");
                ),
            (Closure,
                ERROR(sp, E0000, "Attempting to match over a closure");
                )
            )
        }

        out_ty = (cur_ty == &tmp_ty ? mv$(tmp_ty) : cur_ty->clone());
        out_val = mv$(lval);
    }
}

// --------------------------------------------------------------------
// Dumb and Simple
// --------------------------------------------------------------------
int MIR_LowerHIR_Match_Simple__GeneratePattern(MirBuilder& builder, const Span& sp, const PatternRule* rules, unsigned int num_rules, const ::HIR::TypeRef& ty, const ::MIR::LValue& val, unsigned int field_path_offset,  ::MIR::BasicBlockId fail_bb);

void MIR_LowerHIR_Match_Simple( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arms_code, ::MIR::BasicBlockId first_cmp_block )
{
    TRACE_FUNCTION;

    // 1. Generate pattern matches
    builder.set_cur_block( first_cmp_block );
    for( unsigned int arm_idx = 0; arm_idx < node.m_arms.size(); arm_idx ++ )
    {
        const auto& arm = node.m_arms[arm_idx];
        auto& arm_code = arms_code[arm_idx];

        auto next_arm_bb = builder.new_bb_unlinked();

        for( unsigned int i = 0; i < arm.m_patterns.size(); i ++ )
        {
            if( arm_code.destructures[i] == 0 )
                continue ;

            size_t rule_idx = 0;
            for(; rule_idx < arm_rules.size(); rule_idx++)
                if( arm_rules[rule_idx].arm_idx == arm_idx && arm_rules[rule_idx].pat_idx == i )
                    break;
            const auto& pat_rule = arm_rules[rule_idx];
            bool is_last_pat = (i+1 == arm.m_patterns.size());
            auto next_pattern_bb = (!is_last_pat ? builder.new_bb_unlinked() : next_arm_bb);

            // 1. Check
            // - If the ruleset is empty, this is a _ arm over a value
            if( pat_rule.m_rules.size() > 0 )
            {
                MIR_LowerHIR_Match_Simple__GeneratePattern(builder, arm.m_code->span(), pat_rule.m_rules.data(), pat_rule.m_rules.size(), node.m_value->m_res_type, match_val, 0, next_pattern_bb);
            }
            builder.end_block( ::MIR::Terminator::make_Goto(arm_code.destructures[i]) );
            builder.set_cur_block( arm_code.destructures[i] );

            // - Go to code/condition check
            if( arm_code.has_condition )
            {
                builder.end_block( ::MIR::Terminator::make_Goto(arm_code.cond_start) );
            }
            else
            {
                builder.end_block( ::MIR::Terminator::make_Goto(arm_code.code) );
            }

            if( !is_last_pat )
            {
                builder.set_cur_block( next_pattern_bb );
            }
        }
        if( arm_code.has_condition )
        {
            builder.set_cur_block( arm_code.cond_false );
            builder.end_block( ::MIR::Terminator::make_Goto(next_arm_bb) );
        }
        builder.set_cur_block( next_arm_bb );
    }
    // - Kill the final pattern block (which is dead code)
    builder.end_block( ::MIR::Terminator::make_Diverge({}) );
}

int MIR_LowerHIR_Match_Simple__GeneratePattern(MirBuilder& builder, const Span& sp, const PatternRule* rules, unsigned int num_rules, const ::HIR::TypeRef& top_ty, const ::MIR::LValue& top_val, unsigned int field_path_ofs,  ::MIR::BasicBlockId fail_bb)
{
    TRACE_FUNCTION_F("top_ty = " << top_ty << ", rules = [" << FMT_CB(os, for(const auto* r = rules; r != rules+num_rules; r++) os << *r << ","; ));
    for(unsigned int rule_idx = 0; rule_idx < num_rules; rule_idx ++)
    {
        const auto& rule = rules[rule_idx];
        DEBUG("rule = " << rule);

        // Don't emit anything for '_' matches
        if( rule.is_Any() )
            continue ;

        ::MIR::LValue   val;
        ::HIR::TypeRef  ity;

        get_ty_and_val(sp, builder, top_ty, top_val,  rule.field_path, field_path_ofs,  ity, val);
        DEBUG("ty = " << ity << ", val = " << val);

        const auto& ty = ity;
        TU_MATCH_HDRA( (ty.m_data), {)
        TU_ARMA(Infer, _te) {
            BUG(sp, "Hit _ in type - " << ty);
            }
        TU_ARMA(Diverge, _te) {
            BUG(sp, "Matching over !");
            }
        TU_ARMA(Primitive, te) {
            switch(te)
            {
            case ::HIR::CoreType::Bool: {
                ASSERT_BUG(sp, rule.is_Bool(), "PatternRule for bool isn't _Bool");
                bool test_val = rule.as_Bool();

                auto succ_bb = builder.new_bb_unlinked();

                if( test_val ) {
                    builder.end_block( ::MIR::Terminator::make_If({ val.clone(), succ_bb, fail_bb }) );
                }
                else {
                    builder.end_block( ::MIR::Terminator::make_If({ val.clone(), fail_bb, succ_bb }) );
                }
                builder.set_cur_block(succ_bb);
                } break;
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::U128:
            case ::HIR::CoreType::Usize:
                TU_MATCH_DEF( PatternRule, (rule), (re),
                (
                    BUG(sp, "PatternRule for integer is not Value or ValueRange");
                    ),
                (Value,
                    auto succ_bb = builder.new_bb_unlinked();

                    auto test_val = ::MIR::Param( ::MIR::Constant::make_Uint({ re.as_Uint().v, te }));
                    builder.push_stmt_assign(sp, builder.get_if_cond(), ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::EQ, mv$(test_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ builder.get_if_cond(), succ_bb, fail_bb }) );
                    builder.set_cur_block(succ_bb);
                    ),
                (ValueRange,
                    auto succ_bb = builder.new_bb_unlinked();
                    auto test_bb_2 = builder.new_bb_unlinked();

                    auto test_lt_val = ::MIR::Param(::MIR::Constant::make_Uint({ re.first.as_Uint().v, te }));
                    auto test_gt_val = ::MIR::Param(::MIR::Constant::make_Uint({ re.last.as_Uint().v, te }));

                    // IF `val` < `first` : fail_bb
                    auto cmp_lt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::LT, mv$(test_lt_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lt_lval), fail_bb, test_bb_2 }) );

                    builder.set_cur_block(test_bb_2);

                    // IF `val` > `last` : fail_bb
                    auto cmp_gt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::GT, mv$(test_gt_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_gt_lval), fail_bb, succ_bb }) );

                    builder.set_cur_block(succ_bb);
                    )
                )
                break;
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::I128:
            case ::HIR::CoreType::Isize:
                TU_MATCH_DEF( PatternRule, (rule), (re),
                (
                    BUG(sp, "PatternRule for integer is not Value or ValueRange");
                    ),
                (Value,
                    auto succ_bb = builder.new_bb_unlinked();

                    auto test_val = ::MIR::Param(::MIR::Constant::make_Int({ re.as_Int().v, te }));
                    auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::EQ, mv$(test_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), succ_bb, fail_bb }) );
                    builder.set_cur_block(succ_bb);
                    ),
                (ValueRange,
                    auto succ_bb = builder.new_bb_unlinked();
                    auto test_bb_2 = builder.new_bb_unlinked();

                    auto test_lt_val = ::MIR::Param(::MIR::Constant::make_Int({ re.first.as_Int().v, te }));
                    auto test_gt_val = ::MIR::Param(::MIR::Constant::make_Int({ re.last.as_Int().v, te }));

                    // IF `val` < `first` : fail_bb
                    auto cmp_lt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::LT, mv$(test_lt_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lt_lval), fail_bb, test_bb_2 }) );

                    builder.set_cur_block(test_bb_2);

                    // IF `val` > `last` : fail_bb
                    auto cmp_gt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::GT, mv$(test_gt_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_gt_lval), fail_bb, succ_bb }) );

                    builder.set_cur_block(succ_bb);
                    )
                )
                break;
            case ::HIR::CoreType::Char:
                TU_MATCH_DEF( PatternRule, (rule), (re),
                (
                    BUG(sp, "PatternRule for char is not Value or ValueRange");
                    ),
                (Value,
                    auto succ_bb = builder.new_bb_unlinked();

                    auto test_val = ::MIR::Param(::MIR::Constant::make_Uint({ re.as_Uint().v, te }));
                    auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::EQ, mv$(test_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), succ_bb, fail_bb }) );
                    builder.set_cur_block(succ_bb);
                    ),
                (ValueRange,
                    auto succ_bb = builder.new_bb_unlinked();
                    auto test_bb_2 = builder.new_bb_unlinked();

                    auto test_lt_val = ::MIR::Param(::MIR::Constant::make_Uint({ re.first.as_Uint().v, te }));
                    auto test_gt_val = ::MIR::Param(::MIR::Constant::make_Uint({ re.last.as_Uint().v, te }));

                    // IF `val` < `first` : fail_bb
                    auto cmp_lt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::LT, mv$(test_lt_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lt_lval), fail_bb, test_bb_2 }) );

                    builder.set_cur_block(test_bb_2);

                    // IF `val` > `last` : fail_bb
                    auto cmp_gt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::GT, mv$(test_gt_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_gt_lval), fail_bb, succ_bb }) );

                    builder.set_cur_block(succ_bb);
                    )
                )
                break;
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64:
                TU_MATCH_DEF( PatternRule, (rule), (re),
                (
                    BUG(sp, "PatternRule for float is not Value or ValueRange");
                    ),
                (Value,
                    auto succ_bb = builder.new_bb_unlinked();

                    auto test_val = ::MIR::Param(::MIR::Constant::make_Float({ re.as_Float().v, te }));
                    auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::EQ, mv$(test_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), succ_bb, fail_bb }) );
                    builder.set_cur_block(succ_bb);
                    ),
                (ValueRange,
                    auto succ_bb = builder.new_bb_unlinked();
                    auto test_bb_2 = builder.new_bb_unlinked();

                    auto test_lt_val = ::MIR::Param(::MIR::Constant::make_Float({ re.first.as_Float().v, te }));
                    auto test_gt_val = ::MIR::Param(::MIR::Constant::make_Float({ re.last.as_Float().v, te }));

                    // IF `val` < `first` : fail_bb
                    auto cmp_lt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::LT, mv$(test_lt_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lt_lval), fail_bb, test_bb_2 }) );

                    builder.set_cur_block(test_bb_2);

                    // IF `val` > `last` : fail_bb
                    auto cmp_gt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::GT, mv$(test_gt_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_gt_lval), fail_bb, succ_bb }) );

                    builder.set_cur_block(succ_bb);
                    )
                )
                break;
            case ::HIR::CoreType::Str: {
                ASSERT_BUG(sp, rule.is_Value() && rule.as_Value().is_StaticString(), "");
                const auto& v = rule.as_Value();
                ASSERT_BUG(sp, val.is_Deref(), "");
                val.m_wrappers.pop_back();
                auto str_val = mv$(val);

                auto succ_bb = builder.new_bb_unlinked();

                auto test_val = ::MIR::Param(::MIR::Constant( v.as_StaticString() ));
                auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ mv$(str_val), ::MIR::eBinOp::EQ, mv$(test_val) }));
                builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), succ_bb, fail_bb }) );
                builder.set_cur_block(succ_bb);
                } break;
            }
            }
        TU_ARMA(Path, te) {
            TU_MATCHA( (te.binding), (pbe),
            (Unbound,
                BUG(sp, "Encounterd unbound path - " << te.path);
                ),
            (Opaque,
                BUG(sp, "Attempting to match over opaque type - " << ty);
                ),
            (Struct,
                const auto& str_data = pbe->m_data;
                TU_MATCHA( (str_data), (sd),
                (Unit,
                    BUG(sp, "Attempting to match over unit type - " << ty);
                    ),
                (Tuple,
                    TODO(sp, "Matching on tuple-like struct?");
                    ),
                (Named,
                    TODO(sp, "Matching on struct?");
                    )
                )
                ),
            (Union,
                TODO(sp, "Match over Union");
                ),
            (ExternType,
                TODO(sp, "Match over ExternType");
                ),
            (Enum,
                auto monomorph = [&](const auto& ty) {
                    auto rv = monomorphise_type(sp, pbe->m_params, te.path.m_data.as_Generic().m_params, ty);
                    builder.resolve().expand_associated_types(sp, rv);
                    return rv;
                    };
                ASSERT_BUG(sp, rule.is_Variant(), "Rule for enum isn't Any or Variant");
                const auto& re = rule.as_Variant();
                unsigned int var_idx = re.idx;

                auto next_bb = builder.new_bb_unlinked();
                auto var_count = pbe->num_variants();

                // Generate a switch with only one option different.
                ::std::vector< ::MIR::BasicBlockId> arms(var_count, fail_bb);
                arms[var_idx] = next_bb;
                builder.end_block( ::MIR::Terminator::make_Switch({ val.clone(), mv$(arms) }) );

                builder.set_cur_block(next_bb);

                if( re.sub_rules.size() > 0 )
                {
                    ASSERT_BUG(sp, pbe->m_data.is_Data(), "Sub-rules present for non-data enum");
                    const auto& variants = pbe->m_data.as_Data();
                    const auto& var_ty = variants.at(re.idx).type;
                    ::HIR::TypeRef  tmp;
                    const auto& var_ty_m = (monomorphise_type_needed(var_ty) ? tmp = monomorph(var_ty) : var_ty);

                    // Recurse with the new ruleset
                    MIR_LowerHIR_Match_Simple__GeneratePattern(builder, sp,
                        re.sub_rules.data(), re.sub_rules.size(),
                        var_ty_m, ::MIR::LValue::new_Downcast(val.clone(), var_idx), rule.field_path.size()+1,
                        fail_bb
                        );
                }
                )   // TypePathBinding::Enum
            )
            }  // Type::Data::Path
        TU_ARMA(Generic, _te) {
            BUG(sp, "Attempting to match a generic");
            }
        TU_ARMA(TraitObject, te) {
            BUG(sp, "Attempting to match a trait object");
            }
        TU_ARMA(ErasedType, te) {
            BUG(sp, "Attempting to match an erased type");
            }
        TU_ARMA(Array, te) {
            TODO(sp, "Match directly on array?");
            }
        TU_ARMA(Slice, te) {
            ASSERT_BUG(sp, rule.is_Slice() || rule.is_SplitSlice() || (rule.is_Value() && rule.as_Value().is_Bytes()), "Can only match slice with Bytes or Slice rules - " << rule);
            if( rule.is_Value() ) {
                ASSERT_BUG(sp, *te.inner == ::HIR::CoreType::U8, "Bytes pattern on non-&[u8]");
                auto cloned_val = ::MIR::Constant( rule.as_Value().as_Bytes() );
                auto size_val = ::MIR::Constant::make_Uint({ rule.as_Value().as_Bytes().size(), ::HIR::CoreType::Usize });

                auto succ_bb = builder.new_bb_unlinked();

                ASSERT_BUG(sp, val.is_Deref(), "Slice pattern on non-Deref - " << val);
                auto inner_val = val.clone_unwrapped();

                auto slice_rval = ::MIR::RValue::make_MakeDst({ mv$(cloned_val), mv$(size_val) });
                auto test_lval = builder.lvalue_or_temp(sp, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ty.clone()), mv$(slice_rval));
                auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ mv$(inner_val), ::MIR::eBinOp::EQ, mv$(test_lval) }));
                builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), succ_bb, fail_bb }) );
                builder.set_cur_block(succ_bb);
            }
            else if( rule.is_Slice() ) {
                const auto& re = rule.as_Slice();

                // Compare length
                auto test_val = ::MIR::Param( ::MIR::Constant::make_Uint({ re.len, ::HIR::CoreType::Usize }) );
                auto len_val = builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue::make_DstMeta({ builder.get_ptr_to_dst(sp, val) }));
                auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ mv$(len_val), ::MIR::eBinOp::EQ, mv$(test_val) }));

                auto len_succ_bb = builder.new_bb_unlinked();
                builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), len_succ_bb, fail_bb }) );
                builder.set_cur_block(len_succ_bb);

                // Recurse checking values
                MIR_LowerHIR_Match_Simple__GeneratePattern(builder, sp,
                    re.sub_rules.data(), re.sub_rules.size(),
                    top_ty, top_val, field_path_ofs,
                    fail_bb
                    );
            }
            else if( rule.is_SplitSlice() ) {
                const auto& re = rule.as_SplitSlice();

                // Compare length
                auto test_val = ::MIR::Param( ::MIR::Constant::make_Uint({ re.min_len, ::HIR::CoreType::Usize}) );
                auto len_val = builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue::make_DstMeta({ builder.get_ptr_to_dst(sp, val) }));
                auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ mv$(len_val), ::MIR::eBinOp::LT, mv$(test_val) }));

                auto len_succ_bb = builder.new_bb_unlinked();
                builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), fail_bb, len_succ_bb }) );   // if len < test : FAIL
                builder.set_cur_block(len_succ_bb);

                MIR_LowerHIR_Match_Simple__GeneratePattern(builder, sp,
                    re.leading.data(), re.leading.size(),
                    top_ty, top_val, field_path_ofs,
                    fail_bb
                    );

                if( re.trailing.size() > 0 )
                {
                    TODO(sp, "Match over Slice using SplitSlice with trailing - " << rule);
                }
            }
            else {
                BUG(sp, "Invalid rule type for slice - " << rule);
            }
            }   // Type::Data::Array
        TU_ARMA(Tuple, te) {
            TODO(sp, "Match directly on tuple?");
            }
        TU_ARMA(Borrow, te) {
            TODO(sp, "Match directly on borrow?");
            }   // Type::Data::Borrow
        TU_ARMA(Pointer, te) {
            BUG(sp, "Attempting to match a pointer - " << rule << " against " << ty);
            }
        TU_ARMA(Function, te) {
            BUG(sp, "Attempting to match a function pointer - " << rule << " against " << ty);
            }
        TU_ARMA(Closure, te) {
            BUG(sp, "Attempting to match a closure");
            }
        }
    }
    return 0;
}

// --
// Match v2 Algo - Grouped rules
// --


class t_rules_subset
{
    ::std::vector<const ::std::vector<PatternRule>*>    rule_sets;
    bool is_arm_indexes;
    ::std::vector<size_t>   arm_idxes;

    static ::std::pair<size_t,size_t> decode_arm_idx(size_t v) {
        return ::std::make_pair(v & 0x3FFF, v >> 14);
    }
    static size_t encode_arm_idx(size_t arm_idx, size_t pat_idx) {
        assert(arm_idx <= 0x3FFF);
        assert(pat_idx <= 0x3FFF);
        return arm_idx | (pat_idx << 14);
    }
public:
    t_rules_subset(size_t exp, bool is_arm_indexes):
        is_arm_indexes(is_arm_indexes)
    {
        rule_sets.reserve(exp);
        arm_idxes.reserve(exp);
    }

    size_t size() const {
        return rule_sets.size();
    }
    const ::std::vector<PatternRule>& operator[](size_t n) const {
        return *rule_sets[n];
    }
    bool is_arm() const { return is_arm_indexes; }
    ::std::pair<size_t,size_t> arm_idx(size_t n) const {
        assert(is_arm_indexes);
        return decode_arm_idx( arm_idxes.at(n) );
    }
    ::MIR::BasicBlockId bb_idx(size_t n) const {
        assert(!is_arm_indexes);
        return arm_idxes.at(n);
    }

    void sub_sort(size_t ofs, size_t start, size_t n)
    {
        ::std::vector<size_t>   v;
        for(size_t i = 0; i < n; i++)
            v.push_back(start + i);
        // Sort rules based on just the value (ignore inner rules)
        ::std::stable_sort( v.begin(), v.end(), [&](auto a, auto b){ return ord_rule_compatible( (*rule_sets[a])[ofs], (*rule_sets[b])[ofs]) == OrdLess; } );

        // Reorder contents to above sorting
        {
            decltype(this->rule_sets)   tmp;
            for(auto i : v)
                tmp.push_back(rule_sets[i]);
            ::std::copy( tmp.begin(), tmp.end(), rule_sets.begin() + start );
        }
        {
            decltype(this->arm_idxes)   tmp;
            for(auto i : v)
                tmp.push_back(arm_idxes[i]);
            ::std::copy( tmp.begin(), tmp.end(), arm_idxes.begin() + start );
        }
    }

    t_rules_subset sub_slice(size_t ofs, size_t n)
    {
        t_rules_subset  rv { n, this->is_arm_indexes };
        rv.rule_sets.reserve(n);
        for(size_t i = 0; i < n; i++)
        {
            rv.rule_sets.push_back( this->rule_sets[ofs+i] );
            rv.arm_idxes.push_back( this->arm_idxes[ofs+i] );
        }
        return rv;
    }
    void push_arm(const ::std::vector<PatternRule>& x, size_t arm_idx, size_t pat_idx)
    {
        assert(is_arm_indexes);
        rule_sets.push_back(&x);
        arm_idxes.push_back( encode_arm_idx(arm_idx, pat_idx) );
    }
    void push_bb(const ::std::vector<PatternRule>& x, ::MIR::BasicBlockId bb)
    {
        assert(!is_arm_indexes);
        rule_sets.push_back(&x);
        arm_idxes.push_back(bb);
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const t_rules_subset& x) {
        os << "t_rules_subset{";
        for(size_t i = 0; i < x.rule_sets.size(); i ++)
        {
            if(i != 0)
                os << ", ";
            os << "[";
            if(x.is_arm_indexes)
            {
                auto v = decode_arm_idx(x.arm_idxes[i]);
                os << v.first << "," << v.second;
            }
            else
            {
                os << "bb" << x.arm_idxes[i];
            }
            os << "]";
            os << ": " << *x.rule_sets[i];
        }
        os << "}";
        return os;
    }
};

class MatchGenGrouped
{
    const Span& sp;
    MirBuilder& m_builder;
    const ::HIR::TypeRef& m_top_ty;
    const ::MIR::LValue& m_top_val;
    const ::std::vector<ArmCode>& m_arms_code;

    size_t m_field_path_ofs;
public:
    MatchGenGrouped(MirBuilder& builder, const Span& sp, const ::HIR::TypeRef& top_ty, const ::MIR::LValue& top_val, const ::std::vector<ArmCode>& arms_code, size_t field_path_ofs):
        sp(sp),
        m_builder(builder),
        m_top_ty(top_ty),
        m_top_val(top_val),
        m_arms_code(arms_code),
        m_field_path_ofs(field_path_ofs)
    {
    }

    void gen_for_slice(t_rules_subset rules, size_t ofs, ::MIR::BasicBlockId default_arm);
    void gen_dispatch(const ::std::vector<t_rules_subset>& rules, size_t ofs, const ::std::vector<::MIR::BasicBlockId>& arm_targets, ::MIR::BasicBlockId def_blk);
    void gen_dispatch__primitive(::HIR::TypeRef ty, ::MIR::LValue val, const ::std::vector<t_rules_subset>& rules, size_t ofs, const ::std::vector<::MIR::BasicBlockId>& arm_targets, ::MIR::BasicBlockId def_blk);
    void gen_dispatch__enum(::HIR::TypeRef ty, ::MIR::LValue val, const ::std::vector<t_rules_subset>& rules, size_t ofs, const ::std::vector<::MIR::BasicBlockId>& arm_targets, ::MIR::BasicBlockId def_blk);
    void gen_dispatch__slice(::HIR::TypeRef ty, ::MIR::LValue val, const ::std::vector<t_rules_subset>& rules, size_t ofs, const ::std::vector<::MIR::BasicBlockId>& arm_targets, ::MIR::BasicBlockId def_blk);

    void gen_dispatch_range(const field_path_t& field_path, const ::MIR::Constant& first, const ::MIR::Constant& last, ::MIR::BasicBlockId def_blk);
    void gen_dispatch_splitslice(const field_path_t& field_path, const PatternRule::Data_SplitSlice& e, ::MIR::BasicBlockId def_blk);

    ::MIR::LValue push_compare(::MIR::LValue left, ::MIR::eBinOp op, ::MIR::Param right)
    {
        return m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool,
                ::MIR::RValue::make_BinOp({ mv$(left), op, mv$(right) })
                );
    }
};

namespace {
    void push_flat_rules(::std::vector<PatternRule>& out_rules, PatternRule rule)
    {
        TU_MATCHA( (rule), (e),
        (Variant,
            auto sub_rules = mv$(e.sub_rules);
            out_rules.push_back( mv$(rule) );
            for(auto& sr : sub_rules)
                push_flat_rules(out_rules, mv$(sr));
            ),
        (Slice,
            auto sub_rules = mv$(e.sub_rules);
            out_rules.push_back( mv$(rule) );
            for(auto& sr : sub_rules)
                push_flat_rules(out_rules, mv$(sr));
            ),
        (SplitSlice,
            auto leading = mv$(e.leading);
            auto trailing = mv$(e.trailing);
            out_rules.push_back( mv$(rule) );
            for(auto& sr : leading)
                push_flat_rules(out_rules, mv$(sr));
            // TODO: the trailing rules need a special path format.
            if( !e.trailing.empty() )
            {
                TODO(Span(), "Handle SplitSlice with trailing");
            }
            for(auto& sr : trailing)
                push_flat_rules(out_rules, mv$(sr));
            ),
        (Bool,
            out_rules.push_back( mv$(rule) );
            ),
        (Value,
            out_rules.push_back( mv$(rule) );
            ),
        (ValueRange,
            out_rules.push_back( mv$(rule) );
            ),
        (Any,
            out_rules.push_back( mv$(rule) );
            )
        )
    }
    t_arm_rules flatten_rules(t_arm_rules rules)
    {
        t_arm_rules rv;
        rv.reserve(rules.size());
        for(auto& ruleset : rules)
        {
            ::std::vector<PatternRule>  pattern_rules;
            for( auto& r : ruleset.m_rules )
            {
                push_flat_rules(pattern_rules, mv$(r));
            }
            rv.push_back(PatternRuleset { ruleset.arm_idx, ruleset.pat_idx, mv$(pattern_rules) });
        }
        return rv;
    }
}

void MIR_LowerHIR_Match_Grouped(
        MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val,
        t_arm_rules arm_rules, ::std::vector<ArmCode> arms_code, ::MIR::BasicBlockId first_cmp_block
        )
{
    // TEMPORARY HACK: Grouped fails in complex matches (e.g. librustc_const_math Int::infer)
    //MIR_LowerHIR_Match_Simple( builder, conv, node, mv$(match_val), mv$(arm_rules), mv$(arms_code), first_cmp_block );
    //return;

    TRACE_FUNCTION_F("");

    // Flatten ruleset completely (remove grouping of enum/slice rules)
    arm_rules = flatten_rules( mv$(arm_rules) );

    // - Create a "slice" of the passed rules, suitable for passing to the recursive part of the algo
    t_rules_subset  rules { arm_rules.size(), /*is_arm_indexes=*/true };
    for(const auto& r : arm_rules)
    {
        rules.push_arm( r.m_rules, r.arm_idx, r.pat_idx );
    }

    auto inst = MatchGenGrouped { builder, node.span(), node.m_value->m_res_type, match_val, arms_code, 0 };

    // NOTE: This block should never be used
    auto default_arm = builder.new_bb_unlinked();

    builder.set_cur_block( first_cmp_block );
    inst.gen_for_slice( mv$(rules), 0, default_arm );

    // Make the default infinite loop.
    // - Preferably, it'd abort.
    builder.set_cur_block(default_arm);
    builder.end_block( ::MIR::Terminator::make_Diverge({}) );
}
void MatchGenGrouped::gen_for_slice(t_rules_subset arm_rules, size_t ofs, ::MIR::BasicBlockId default_arm)
{
    TRACE_FUNCTION_F("arm_rules=" << arm_rules << ", ofs="<<ofs << ", default_arm=" << default_arm);
    ASSERT_BUG(sp, arm_rules.size() > 0, "");

    // Quick hack: Skip any layers entirely made up of PatternRule::Any
    for(;;)
    {
        bool is_all_any = true;
        for(size_t i = 0; i < arm_rules.size() && is_all_any; i ++)
        {
            if( arm_rules[i].size() <= ofs )
                is_all_any = false;
            else if( ! arm_rules[i][ofs].is_Any() )
                is_all_any = false;
        }
        if( ! is_all_any )
        {
            break ;
        }
        ofs ++;
        DEBUG("Skip to ofs=" << ofs);
    }

    // Split current set of rules into groups based on _ patterns
    for(size_t idx = 0; idx < arm_rules.size(); )
    {
        // Completed arms
        while( idx < arm_rules.size() && arm_rules[idx].size() <= ofs )
        {
            auto next = idx+1 == arm_rules.size() ? default_arm : m_builder.new_bb_unlinked();
            ASSERT_BUG(sp, arm_rules[idx].size() == ofs, "Offset too large for rule - ofs=" << ofs << ", rules=" << arm_rules[idx]);
            DEBUG(idx << ": Complete");
            // Emit jump to either arm code, or arm condition
            if( arm_rules.is_arm() )
            {
                auto ai = arm_rules.arm_idx(idx);
                ASSERT_BUG(sp, m_arms_code.size() > 0, "Bottom-level ruleset with no arm code information");
                const auto& ac = m_arms_code[ai.first];

                m_builder.end_block( ::MIR::Terminator::make_Goto(ac.destructures[ai.second]) );
                m_builder.set_cur_block( ac.destructures[ai.second] );

                if( ac.has_condition )
                {
                    TODO(sp, "Handle conditionals in Grouped");
                    // TODO: If the condition fails, this should re-try the match on other rules that could have worked.
                    // - For now, conditionals are disabled.

                    // TODO: What if there's multiple patterns on this condition?
                    // - For now, only the first pattern gets edited.
                    // - Maybe clone the blocks used for the condition?
                    m_builder.end_block( ::MIR::Terminator::make_Goto(ac.cond_start) );

                    // Check for marking in `ac` that the block has already been terminated, assert that target is `next`
                    if( ai.second == 0 )
                    {
                        if( ac.cond_fail_tgt != 0)
                        {
                            ASSERT_BUG(sp, ac.cond_fail_tgt == next, "Condition fail target already set with mismatching arm, set to bb" << ac.cond_fail_tgt << " cur is bb" << next);
                        }
                        else
                        {
                            ac.cond_fail_tgt = next;

                            m_builder.set_cur_block( ac.cond_false );
                            m_builder.end_block( ::MIR::Terminator::make_Goto(next) );
                        }
                    }

                    if( next != default_arm )
                        m_builder.set_cur_block(next);
                }
                else
                {
                    m_builder.end_block( ::MIR::Terminator::make_Goto(ac.code) );
                    ASSERT_BUG(sp, idx+1 == arm_rules.size(), "Ended arm with other arms present");
                }
            }
            else
            {
                auto bb = arm_rules.bb_idx(idx);
                m_builder.end_block( ::MIR::Terminator::make_Goto(bb) );
                while( idx+1 < arm_rules.size() && bb == arm_rules.bb_idx(idx) && arm_rules[idx].size() == ofs )
                    idx ++;
                ASSERT_BUG(sp, idx+1 == arm_rules.size(), "Ended arm (inner) with other arms present");
            }
            idx ++;
        }

        // - Value arms
        auto start = idx;
        for(; idx < arm_rules.size() ; idx ++)
        {
            if( arm_rules[idx].size() <= ofs )
                break;
            if( arm_rules[idx][ofs].is_Any() )
                break;
            if( arm_rules[idx][ofs].is_SplitSlice() )
                break;
            // TODO: It would be nice if ValueRange could be combined with Value (if there's no overlap)
            if( arm_rules[idx][ofs].is_ValueRange() )
                break;
        }
        auto first_any = idx;

        // Generate dispatch based on the above list
        // - If there's value ranges they need special handling
        // - Can sort arms within this group (ordering doesn't matter, as long as ranges are handled)
        // - Sort must be stable.

        if( start < first_any )
        {
            DEBUG(start << "+" << (first_any-start) << ": Values");
            bool has_default = (first_any < arm_rules.size());
            auto next = (has_default ? m_builder.new_bb_unlinked() : default_arm);

            // Sort rules before getting compatible runs
            // TODO: Is this a valid operation?
            arm_rules.sub_sort(ofs,  start, first_any - start);

            // Create list of compatible arm slices (runs with the same selector value)
            ::std::vector<t_rules_subset>   slices;
            auto cur_test = start;
            for(auto i = start; i < first_any; i ++)
            {
                // Just check if the decision value differs (don't check nested rules)
                if( ! rule_compatible(arm_rules[i][ofs], arm_rules[cur_test][ofs]) )
                {
                    slices.push_back( arm_rules.sub_slice(cur_test, i - cur_test) );
                    cur_test = i;
                }
            }
            slices.push_back( arm_rules.sub_slice(cur_test, first_any - cur_test) );
            DEBUG("- " << slices.size() << " groupings");
            ::std::vector<::MIR::BasicBlockId>  arm_blocks;
            arm_blocks.reserve( slices.size() );

            auto cur_blk = m_builder.pause_cur_block();
            // > Stable sort list
            ::std::sort( slices.begin(), slices.end(), [&](const auto& a, const auto& b){ return a[0][ofs] < b[0][ofs]; } );
            // TODO: Should this do a stable sort of inner patterns too?
            // - A sort of inner patterns such that `_` (and range?) patterns don't change position.

            // > Get type of match, generate dispatch list.
            for(size_t i = 0; i < slices.size(); i ++)
            {
                auto cur_block = m_builder.new_bb_unlinked();
                m_builder.set_cur_block(cur_block);

                for(size_t j = 0; j < slices[i].size(); j ++)
                {
                    if(j > 0)
                        ASSERT_BUG(sp, slices[i][0][ofs] == slices[i][j][ofs], "Mismatched rules - " << slices[i][0][ofs] << " and " << slices[i][j][ofs]);
                    arm_blocks.push_back(cur_block);
                }

                this->gen_for_slice(slices[i], ofs+1, next);
            }

            m_builder.set_cur_block(cur_blk);

            // Generate decision code
            this->gen_dispatch(slices, ofs, arm_blocks, next);

            if(has_default)
            {
                m_builder.set_cur_block(next);
            }
        }

        // Collate matching blocks at `first_any`
        assert(first_any == idx);
        if( first_any < arm_rules.size() && arm_rules[idx].size() > ofs )
        {
            // Collate all equal rules
            while(idx < arm_rules.size() && arm_rules[idx][ofs] == arm_rules[first_any][ofs])
                idx ++;
            DEBUG(first_any << "-" << idx << ": Multi-match");

            bool has_next = idx < arm_rules.size();
            auto next = (has_next ? m_builder.new_bb_unlinked() : default_arm);

            const auto& rule = arm_rules[first_any][ofs];
            if(const auto* e = rule.opt_ValueRange())
            {
                // Generate branch based on range
                this->gen_dispatch_range(arm_rules[first_any][ofs].field_path, e->first, e->last, next);
            }
            else if(const auto* e = rule.opt_SplitSlice())
            {
                // Generate branch based on slice length being at least required.
                this->gen_dispatch_splitslice(rule.field_path, *e, next);
            }
            else
            {
                ASSERT_BUG(sp, rule.is_Any(), "Didn't expect non-Any rule here, got " << rule.tag_str() << " " << rule);
            }

            // Step deeper into these arms
            auto slice = arm_rules.sub_slice(first_any, idx - first_any);
            this->gen_for_slice(mv$(slice), ofs+1, next);

            if(has_next)
            {
                m_builder.set_cur_block(next);
            }
        }
    }

    ASSERT_BUG(sp, ! m_builder.block_active(), "Block left active after match group");
}

void MatchGenGrouped::gen_dispatch(const ::std::vector<t_rules_subset>& rules, size_t ofs, const ::std::vector<::MIR::BasicBlockId>& arm_targets, ::MIR::BasicBlockId def_blk)
{
    const auto& field_path = rules[0][0][ofs].field_path;
    TRACE_FUNCTION_F("rules=["<<rules <<"], ofs=" << ofs <<", field_path=" << field_path);

    {
        size_t n = 0;
        for(size_t i = 0; i < rules.size(); i++)
        {
            for(size_t j = 0; j < rules[i].size(); j++)
            {
                ASSERT_BUG(sp, rules[i][j][ofs].field_path == field_path, "Field path mismatch, " << rules[i][j][ofs].field_path << " != " << field_path);
                n ++;
            }
        }
        ASSERT_BUG(sp, arm_targets.size() == n, "Arm target count mismatch - " << n << " != " << arm_targets.size());
    }

    ::MIR::LValue   val;
    ::HIR::TypeRef  ty;
    get_ty_and_val(sp, m_builder, m_top_ty, m_top_val,  field_path, m_field_path_ofs,  ty, val);
    DEBUG("ty = " << ty << ", val = " << val);

    TU_MATCHA( (ty.m_data), (te),
    (Infer,
        BUG(sp, "Hit _ in type - " << ty);
        ),
    (Diverge,
        BUG(sp, "Matching over !");
        ),
    (Primitive,
        this->gen_dispatch__primitive(mv$(ty), mv$(val), rules, ofs, arm_targets, def_blk);
        ),
    (Path,
        // Matching over a path can only happen with an enum.
        // TODO: What about `box` destructures?
        // - They're handled via hidden derefs
        TU_MATCH_HDR( (te.binding), { )
        TU_ARM(te.binding, Unbound, pbe) {
            BUG(sp, "Encounterd unbound path - " << te.path);
            }
        TU_ARM(te.binding, Opaque, pbe) {
            BUG(sp, "Attempting to match over opaque type - " << ty);
            }
        TU_ARM(te.binding, Struct, pbe) {
            const auto& str_data = pbe->m_data;
            TU_MATCHA( (str_data), (sd),
            (Unit,
                BUG(sp, "Attempting to match over unit type - " << ty);
                ),
            (Tuple,
                TODO(sp, "Matching on tuple-like struct?");
                ),
            (Named,
                TODO(sp, "Matching on struct? - " << ty);
                )
            )
            }
        TU_ARM(te.binding, Union, pbe) {
            TODO(sp, "Match over Union");
            }
        TU_ARM(te.binding, ExternType, pbe) {
            TODO(sp, "Match over ExternType - " << ty);
            }
        TU_ARM(te.binding, Enum, pbe) {
            this->gen_dispatch__enum(mv$(ty), mv$(val), rules, ofs, arm_targets, def_blk);
            }
        }
        ),
    (Generic,
        BUG(sp, "Attempting to match a generic");
        ),
    (TraitObject,
        BUG(sp, "Attempting to match a trait object");
        ),
    (ErasedType,
        BUG(sp, "Attempting to match an erased type");
        ),
    (Array,
        BUG(sp, "Attempting to match on an Array (should have been destructured)");
        ),
    (Slice,
        // TODO: Slice size matches!
        this->gen_dispatch__slice(mv$(ty), mv$(val), rules, ofs, arm_targets, def_blk);
        ),
    (Tuple,
        BUG(sp, "Match directly on tuple");
        ),
    (Borrow,
        BUG(sp, "Match directly on borrow");
        ),
    (Pointer,
        // TODO: Could this actually be valid?
        BUG(sp, "Attempting to match a pointer - " << ty);
        ),
    (Function,
        // TODO: Could this actually be valid?
        BUG(sp, "Attempting to match a function pointer - " << ty);
        ),
    (Closure,
        BUG(sp, "Attempting to match a closure");
        )
    )
}

void MatchGenGrouped::gen_dispatch__primitive(::HIR::TypeRef ty, ::MIR::LValue val, const ::std::vector<t_rules_subset>& rules, size_t ofs, const ::std::vector<::MIR::BasicBlockId>& arm_targets, ::MIR::BasicBlockId def_blk)
{
    auto te = ty.m_data.as_Primitive();
    switch(te)
    {
    case ::HIR::CoreType::Bool: {
        ASSERT_BUG(sp, rules.size() <= 2, "More than 2 rules for boolean");
        for(size_t i = 0; i < rules.size(); i++)
        {
            ASSERT_BUG(sp, rules[i][0][ofs].is_Bool(), "PatternRule for bool isn't _Bool");
        }

        // False sorts before true.
        auto fail_bb = rules.size() == 2 ? arm_targets[              0] : (rules[0][0][ofs].as_Bool() ? def_blk : arm_targets[0]);
        auto succ_bb = rules.size() == 2 ? arm_targets[rules[0].size()] : (rules[0][0][ofs].as_Bool() ? arm_targets[0] : def_blk);

        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val), succ_bb, fail_bb }) );
        } break;
    case ::HIR::CoreType::U8:
    case ::HIR::CoreType::U16:
    case ::HIR::CoreType::U32:
    case ::HIR::CoreType::U64:
    case ::HIR::CoreType::U128:
    case ::HIR::CoreType::Usize:

    case ::HIR::CoreType::Char:
        if( rules.size() == 1 )
        {
            // Special case, single option, equality only
            const auto& r = rules[0][0][ofs];
            ASSERT_BUG(sp, r.is_Value(), "Matching without _Value pattern - " << r.tag_str());
            const auto& re = r.as_Value();
            auto test_val = ::MIR::Param(re.clone());
            auto cmp_lval = m_builder.get_rval_in_if_cond(sp, ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::EQ, mv$(test_val) }));
            m_builder.end_block( ::MIR::Terminator::make_If({  mv$(cmp_lval), arm_targets[0], def_blk }) );
        }
        else
        {
            // NOTE: Rules are currently sorted
            // TODO: If there are Constant::Const values in the list, they need to come first! (with equality checks)

            ::std::vector< uint64_t>  values;
            ::std::vector< ::MIR::BasicBlockId> targets;
            size_t tgt_ofs = 0;
            for(size_t i = 0; i < rules.size(); i++)
            {
                for(size_t j = 1; j < rules[i].size(); j ++)
                    ASSERT_BUG(sp, arm_targets[tgt_ofs] == arm_targets[tgt_ofs+j], "Mismatched target blocks for Value match");

                const auto& r = rules[i][0][ofs];
                ASSERT_BUG(sp, r.is_Value(), "Matching without _Value pattern - " << r.tag_str());
                const auto& re = r.as_Value();
                if(re.is_Const())
                    TODO(sp, "Handle Constant::Const in match");

                values.push_back( re.as_Uint().v );
                targets.push_back( arm_targets[tgt_ofs] );

                tgt_ofs += rules[i].size();
            }
            m_builder.end_block( ::MIR::Terminator::make_SwitchValue({
                mv$(val), def_blk, mv$(targets), ::MIR::SwitchValues(mv$(values))
                }) );
        }
        break;

    case ::HIR::CoreType::I8:
    case ::HIR::CoreType::I16:
    case ::HIR::CoreType::I32:
    case ::HIR::CoreType::I64:
    case ::HIR::CoreType::I128:
    case ::HIR::CoreType::Isize:
        if( rules.size() == 1 )
        {
            // Special case, single option, equality only
            const auto& r = rules[0][0][ofs];
            ASSERT_BUG(sp, r.is_Value(), "Matching without _Value pattern - " << r.tag_str());
            const auto& re = r.as_Value();
            auto test_val = ::MIR::Param(re.clone());
            auto cmp_lval = m_builder.get_rval_in_if_cond(sp, ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::EQ, mv$(test_val) }));
            m_builder.end_block( ::MIR::Terminator::make_If({  mv$(cmp_lval), arm_targets[0], def_blk }) );
        }
        else
        {
            // NOTE: Rules are currently sorted
            // TODO: If there are Constant::Const values in the list, they need to come first! (with equality checks)

            ::std::vector< int64_t>  values;
            ::std::vector< ::MIR::BasicBlockId> targets;
            size_t tgt_ofs = 0;
            for(size_t i = 0; i < rules.size(); i++)
            {
                for(size_t j = 1; j < rules[i].size(); j ++)
                    ASSERT_BUG(sp, arm_targets[tgt_ofs] == arm_targets[tgt_ofs+j], "Mismatched target blocks for Value match");

                const auto& r = rules[i][0][ofs];
                ASSERT_BUG(sp, r.is_Value(), "Matching without _Value pattern - " << r.tag_str());
                const auto& re = r.as_Value();
                if(re.is_Const())
                    TODO(sp, "Handle Constant::Const in match");

                values.push_back( re.as_Int().v );
                targets.push_back( arm_targets[tgt_ofs] );

                tgt_ofs += rules[i].size();
            }
            m_builder.end_block( ::MIR::Terminator::make_SwitchValue({
                mv$(val), def_blk, mv$(targets), ::MIR::SwitchValues(mv$(values))
                }) );
        }
        break;

    case ::HIR::CoreType::F32:
    case ::HIR::CoreType::F64: {
        // NOTE: Rules are currently sorted
        // TODO: If there are Constant::Const values in the list, they need to come first!
        size_t tgt_ofs = 0;
        for(size_t i = 0; i < rules.size(); i++)
        {
            for(size_t j = 1; j < rules[i].size(); j ++)
                ASSERT_BUG(sp, arm_targets[tgt_ofs] == arm_targets[tgt_ofs+j], "Mismatched target blocks for Value match");

            const auto& r = rules[i][0][ofs];
            ASSERT_BUG(sp, r.is_Value(), "Matching without _Value pattern - " << r.tag_str());
            const auto& re = r.as_Value();
            if(re.is_Const())
                TODO(sp, "Handle Constant::Const in match");

            // IF v < tst : def_blk
            {
                auto cmp_eq_blk = m_builder.new_bb_unlinked();
                auto cmp_lval_lt = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::LT, ::MIR::Param(re.clone()) }));
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval_lt), def_blk, cmp_eq_blk }) );
                m_builder.set_cur_block(cmp_eq_blk);
            }

            // IF v == tst : target
            {
                auto next_cmp_blk = m_builder.new_bb_unlinked();
                auto cmp_lval_eq = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::EQ, ::MIR::Param(re.clone()) }));
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval_eq), arm_targets[tgt_ofs], next_cmp_blk }) );
                m_builder.set_cur_block(next_cmp_blk);
            }

            tgt_ofs += rules[i].size();
        }
        m_builder.end_block( ::MIR::Terminator::make_Goto(def_blk) );
        } break;
    case ::HIR::CoreType::Str: {
        // Remove the deref on the &str
        ASSERT_BUG(sp, !val.m_wrappers.empty() && val.m_wrappers.back().is_Deref(), "&str match on non-Deref lvalue - " << val);
        val.m_wrappers.pop_back();

        ::std::vector< ::MIR::BasicBlockId> targets;
        ::std::vector< ::std::string>   values;
        size_t tgt_ofs = 0;
        for(size_t i = 0; i < rules.size(); i++)
        {
            for(size_t j = 1; j < rules[i].size(); j ++)
                ASSERT_BUG(sp, arm_targets[tgt_ofs] == arm_targets[tgt_ofs+j], "Mismatched target blocks for Value match");

            const auto& r = rules[i][0][ofs];
            ASSERT_BUG(sp, r.is_Value(), "Matching without _Value pattern - " << r.tag_str());
            const auto& re = r.as_Value();
            if(re.is_Const())
                TODO(sp, "Handle Constant::Const in match");

            targets.push_back( arm_targets[tgt_ofs] );
            values.push_back( re.as_StaticString() );

            tgt_ofs += rules[i].size();
        }
        m_builder.end_block( ::MIR::Terminator::make_SwitchValue({
            mv$(val), def_blk, mv$(targets), ::MIR::SwitchValues(mv$(values))
            }) );
       } break;
    }
}

void MatchGenGrouped::gen_dispatch__enum(::HIR::TypeRef ty, ::MIR::LValue val, const ::std::vector<t_rules_subset>& rules, size_t ofs, const ::std::vector<::MIR::BasicBlockId>& arm_targets, ::MIR::BasicBlockId def_blk)
{
    TRACE_FUNCTION;
    auto& te = ty.m_data.as_Path();
    const auto& pbe = te.binding.as_Enum();

    auto decison_arm = m_builder.pause_cur_block();

    auto var_count = pbe->num_variants();
    ::std::vector< ::MIR::BasicBlockId> arms(var_count, def_blk);
    size_t  arm_idx = 0;
    for(size_t i = 0; i < rules.size(); i ++)
    {
        ASSERT_BUG(sp, rules[i][0][ofs].is_Variant(), "Rule for enum isn't Any or Variant - " << rules[i][0][ofs].tag_str());
        const auto& re = rules[i][0][ofs].as_Variant();
        unsigned int var_idx = re.idx;
        DEBUG("Variant " << var_idx);

        ASSERT_BUG(sp, re.sub_rules.size() == 0, "Sub-rules in MatchGenGrouped");

        arms[var_idx] = arm_targets[arm_idx];
        for(size_t j = 0; j < rules[i].size(); j ++)
        {
            assert(arms[var_idx] == arm_targets[arm_idx]);
            arm_idx ++;
        }
    }

    m_builder.set_cur_block(decison_arm);
    m_builder.end_block( ::MIR::Terminator::make_Switch({ mv$(val), mv$(arms) }) );
}

void MatchGenGrouped::gen_dispatch__slice(::HIR::TypeRef ty, ::MIR::LValue val, const ::std::vector<t_rules_subset>& rules, size_t ofs, const ::std::vector<::MIR::BasicBlockId>& arm_targets, ::MIR::BasicBlockId def_blk)
{
    auto val_len = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue::make_DstMeta({ m_builder.get_ptr_to_dst(sp, val) }));

    // TODO: Re-sort the rules list to interleve Constant::Bytes and Slice

    // Just needs to check the lengths, then dispatch.
    size_t tgt_ofs = 0;
    for(size_t i = 0; i < rules.size(); i++)
    {
        const auto& r = rules[i][0][ofs];
        if(const auto* re = r.opt_Slice())
        {
            ASSERT_BUG(sp, re->sub_rules.size() == 0, "Sub-rules in MatchGenGrouped");
            auto val_tst = ::MIR::Constant::make_Uint({ re->len, ::HIR::CoreType::Usize });

            for(size_t j = 0; j < rules[i].size(); j ++)
                assert(arm_targets[tgt_ofs] == arm_targets[tgt_ofs+j]);

            // IF v < tst : target
            if( re->len > 0 )
            {
                auto cmp_eq_blk = m_builder.new_bb_unlinked();
                auto cmp_lval_lt = this->push_compare( val_len.clone(), ::MIR::eBinOp::LT, val_tst.clone() );
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval_lt), def_blk, cmp_eq_blk }) );
                m_builder.set_cur_block(cmp_eq_blk);
            }

            // IF v == tst : target
            {
                auto next_cmp_blk = m_builder.new_bb_unlinked();
                auto cmp_lval_eq = this->push_compare( val_len.clone(), ::MIR::eBinOp::EQ, mv$(val_tst) );
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval_eq), arm_targets[tgt_ofs], next_cmp_blk }) );
                m_builder.set_cur_block(next_cmp_blk);
            }
        }
        else if(const auto* re = r.opt_Value())
        {
            ASSERT_BUG(sp, re->is_Bytes(), "Slice with non-Bytes value - " << *re);
            const auto& b = re->as_Bytes();

            auto val_tst = ::MIR::Constant::make_Uint({ b.size(), ::HIR::CoreType::Usize });
            auto cmp_slice_val = m_builder.lvalue_or_temp(sp,
                    ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef::new_slice(::HIR::CoreType::U8) ),
                    ::MIR::RValue::make_MakeDst({ ::MIR::Param(re->clone()), val_tst.clone() })
                    );

            if( b.size() > 0 )
            {
                auto cmp_eq_blk = m_builder.new_bb_unlinked();
                auto cmp_lval_lt = this->push_compare( val_len.clone(), ::MIR::eBinOp::LT, val_tst.clone() );
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval_lt), def_blk, cmp_eq_blk }) );
                m_builder.set_cur_block(cmp_eq_blk);
            }

            // IF v == tst : target
            {
                auto succ_blk = m_builder.new_bb_unlinked();
                auto next_cmp_blk = m_builder.new_bb_unlinked();
                auto cmp_lval_eq = this->push_compare( val_len.clone(), ::MIR::eBinOp::EQ, mv$(val_tst) );
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval_eq), succ_blk, next_cmp_blk }) );
                m_builder.set_cur_block(succ_blk);

                // TODO: What if `val` isn't a Deref?
                ASSERT_BUG(sp, !val.m_wrappers.empty() && val.m_wrappers.back().is_Deref(), "TODO: Handle non-Deref matches of byte strings - " << val);
                cmp_lval_eq = this->push_compare( val.clone_unwrapped(), ::MIR::eBinOp::EQ, mv$(cmp_slice_val) );
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval_eq), arm_targets[tgt_ofs], def_blk }) );

                m_builder.set_cur_block(next_cmp_blk);
            }
        }
        else
        {
            BUG(sp, "Matching without _Slice pattern - " << r.tag_str() << " - " << r);
        }

        tgt_ofs += rules[i].size();
    }
    m_builder.end_block( ::MIR::Terminator::make_Goto(def_blk) );
}


void MatchGenGrouped::gen_dispatch_range(const field_path_t& field_path, const ::MIR::Constant& first, const ::MIR::Constant& last, ::MIR::BasicBlockId def_blk)
{
    TRACE_FUNCTION_F("field_path="<<field_path<<", " << first << " ... " << last);
    ::MIR::LValue   val;
    ::HIR::TypeRef  ty;
    get_ty_and_val(sp, m_builder, m_top_ty, m_top_val,  field_path, m_field_path_ofs,  ty, val);
    DEBUG("ty = " << ty << ", val = " << val);

    if( const auto* tep = ty.m_data.opt_Primitive() )
    {
        auto te = *tep;

        bool lower_possible = true;
        bool upper_possible = true;

        switch(te)
        {
        case ::HIR::CoreType::Bool:
            BUG(sp, "Range match over Bool");
            break;
        case ::HIR::CoreType::Str:
            BUG(sp, "Range match over Str - is this valid?");
            break;
        case ::HIR::CoreType::U8:
        case ::HIR::CoreType::U16:
        case ::HIR::CoreType::U32:
        case ::HIR::CoreType::U64:
        case ::HIR::CoreType::U128:
        case ::HIR::CoreType::Usize:
            lower_possible = (first.as_Uint().v > 0);
            // TODO: Should this also check for the end being the max value of the type?
            // - Can just leave that to the optimiser
            upper_possible = true;
            break;
        case ::HIR::CoreType::I8:
        case ::HIR::CoreType::I16:
        case ::HIR::CoreType::I32:
        case ::HIR::CoreType::I64:
        case ::HIR::CoreType::I128:
        case ::HIR::CoreType::Isize:
            lower_possible = true;
            upper_possible = true;
            break;
        case ::HIR::CoreType::Char:
            lower_possible = (first.as_Uint().v > 0);
            upper_possible = (first.as_Uint().v <= 0x10FFFF);
            break;
        case ::HIR::CoreType::F32:
        case ::HIR::CoreType::F64:
            // NOTE: No upper or lower limits
            break;
        }

        if( lower_possible )
        {
            auto test_bb_2 = m_builder.new_bb_unlinked();
            // IF `val` < `first` : fail_bb
            auto cmp_lt_lval = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::LT, ::MIR::Param(first.clone()) }));
            m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lt_lval), def_blk, test_bb_2 }) );

            m_builder.set_cur_block(test_bb_2);
        }


        if( upper_possible )
        {
            auto succ_bb = m_builder.new_bb_unlinked();

            // IF `val` > `last` : fail_bb
            auto cmp_gt_lval = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::GT, ::MIR::Param(last.clone()) }));
            m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_gt_lval), def_blk, succ_bb }) );

            m_builder.set_cur_block(succ_bb);
        }
    }
    else
    {
        TODO(sp, "ValueRange on " << ty);
    }
}
void MatchGenGrouped::gen_dispatch_splitslice(const field_path_t& field_path, const PatternRule::Data_SplitSlice& e, ::MIR::BasicBlockId def_blk)
{
    TRACE_FUNCTION_F("field_path="<<field_path<<", [" << e.leading << ", .., " << e.trailing << "]");
    ::MIR::LValue   val;
    ::HIR::TypeRef  ty;
    get_ty_and_val(sp, m_builder, m_top_ty, m_top_val,  field_path, m_field_path_ofs,  ty, val);
    DEBUG("ty = " << ty << ", val = " << val);

    ASSERT_BUG(sp, e.leading.size() == 0, "Sub-rules in MatchGenGrouped");
    ASSERT_BUG(sp, e.trailing.size() == 0, "Sub-rules in MatchGenGrouped");
    ASSERT_BUG(sp, ty.m_data.is_Slice(), "SplitSlice pattern on non-slice - " << ty);

    // Obtain slice length
    auto val_len = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue::make_DstMeta({ m_builder.get_ptr_to_dst(sp, val) }));

    // 1. Check that length is sufficient for the pattern to be used
    // `IF len < min_len : def_blk, next
    {
        auto next = m_builder.new_bb_unlinked();
        auto cmp_val = this->push_compare(val_len.clone(), ::MIR::eBinOp::LT, ::MIR::Constant::make_Uint({ e.min_len, ::HIR::CoreType::Usize }));
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_val), def_blk, next }) );
        m_builder.set_cur_block(next);
    }

    // 2. Recurse into leading patterns.
    if( e.min_len > e.trailing_len )
    {
        auto next = m_builder.new_bb_unlinked();
        auto inner_set = t_rules_subset { 1, /*is_arm_indexes=*/false };
        inner_set.push_bb( e.leading, next );
        auto inst = MatchGenGrouped { m_builder, sp, ty, val, {}, field_path.size() };
        inst.gen_for_slice(inner_set, 0, def_blk);

        m_builder.set_cur_block(next);
    }

    // 3. Recurse into trailing patterns
    if( e.trailing_len != 0 )
    {
        auto next = m_builder.new_bb_unlinked();
        auto inner_set = t_rules_subset { 1, /*is_arm_indexes=*/false };
        inner_set.push_bb( e.trailing, next );
        auto inst = MatchGenGrouped { m_builder, sp, ty, val, {}, field_path.size() };
        inst.gen_for_slice(inner_set, 0, def_blk);

        m_builder.set_cur_block(next);
    }
}

