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
#include <trans/target.hpp>

void MIR_LowerHIR_Match( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val );

namespace {
    void get_ty_and_val(
        const Span& sp, MirBuilder& builder,
        const ::HIR::TypeRef& top_ty, const ::MIR::LValue& top_val,
        const field_path_t& field_path, unsigned int field_path_ofs,
        /*Out ->*/ ::HIR::TypeRef& out_ty, ::MIR::LValue& out_val
    );
}

void MIR_LowerHIR_GetTypeValueForPath(
    const Span& sp, MirBuilder& builder,
    const ::HIR::TypeRef& top_ty, const ::MIR::LValue& top_val,
    const field_path_t& field_path,
    /*Out ->*/ ::HIR::TypeRef& out_ty, ::MIR::LValue& out_val
)
{
    get_ty_and_val(sp, builder, top_ty, top_val, field_path, 0, out_ty, out_val);
}

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
    (ValueRange, struct { ::MIR::Constant first, last; bool is_inclusive; }),
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
        PatternRule clone() const;
    )
    );
::std::ostream& operator<<(::std::ostream& os, const PatternRule& x);
/// Constructed set of rules from a pattern
struct PatternRuleset
{
    unsigned int arm_idx;
    unsigned int pat_idx;

    ::std::vector<PatternRule>  m_rules;
    ::std::vector<PatternBinding> m_bindings;

    static ::Ordering rule_is_before(const PatternRule& l, const PatternRule& r);

    bool is_before(const PatternRuleset& other) const;
};
/// Generated code for an arm
struct ArmCode {
    bool has_condition = false;
    // TODO: Each pattern can have its own condition w/ false
    struct Pattern {
        ::MIR::BasicBlockId   code = 0;
        ::MIR::BasicBlockId   cond_false = ~0u;

        mutable ::MIR::BasicBlockId cond_fail_tgt = 0;
    };
    std::vector<Pattern> patterns;
};

typedef ::std::vector<PatternRuleset>  t_arm_rules;

void MIR_LowerHIR_Match_Simple( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arm_code, ::MIR::BasicBlockId first_cmp_block);
void MIR_LowerHIR_Match_Grouped( MirBuilder& builder, MirConverter& conv, const Span& sp, const HIR::TypeRef& match_ty, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arms_code, ::MIR::BasicBlockId first_cmp_block );
void MIR_LowerHIR_Match_DecisionTree( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arm_code , ::MIR::BasicBlockId first_cmp_block);

/// Helper to construct rules from a passed pattern
struct PatternRulesetBuilder
{
    const StaticTraitResolve&   m_resolve;
    const ::HIR::SimplePath*    m_lang_Box = nullptr;

    // NOTE: Multiple rulesets to handle or-patterns (which multiply the pattern set)
    struct Ruleset {
        bool m_is_impossible;
        ::std::vector<PatternRule>  m_rules;
        ::std::vector<PatternBinding> m_bindings;
        
        Ruleset():
            m_is_impossible(false)
        {
        }
        Ruleset clone() const {
            Ruleset rv;
            rv.m_is_impossible = m_is_impossible;
            for(const auto& e : m_rules)
                rv.m_rules.push_back(e.clone());
            rv.m_bindings = m_bindings;
            return rv;
        }
    };
    std::vector<Ruleset>   m_rulesets;
    size_t subset_start, subset_end;

    field_path_t   m_field_path;

    PatternRulesetBuilder(const StaticTraitResolve& resolve):
        m_resolve(resolve)
        , m_rulesets(1)
        , subset_start(0)
        , subset_end(1)
    {
        if( resolve.m_crate.m_lang_items.count("owned_box") > 0 ) {
            m_lang_Box = &resolve.m_crate.m_lang_items.at("owned_box");
        }
    }

    void append_from_lit(const Span& sp, EncodedLiteralSlice lit, const ::HIR::TypeRef& ty);
    void append_from(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::TypeRef& ty);
private:
    void push_rule(PatternRule r);
    void push_binding(PatternBinding b);
    void push_bindings(std::vector<PatternBinding> b);
    void set_impossible();

    void multiply_rulesets(size_t n, std::function<void(size_t idx)> cb);
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
/// `let` (also used for destructuring arguments) - Introduces arguments into the current scope
///
/// If `else_node` is non-null, a `_` "arm" is added to invoke that block (which must diverge)
void MIR_LowerHIR_Let(MirBuilder& builder, MirConverter& conv, const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue val, const ::HIR::ExprNode* else_node)
{
    TRACE_FUNCTION;

    HIR::TypeRef    outer_ty;
    builder.with_val_type(sp, val, [&](const HIR::TypeRef& ty){ outer_ty = ty.clone_shallow(); });

    auto success_node = builder.new_bb_unlinked();
    auto first_cmp_block = builder.pause_cur_block();

    // - Convert HIR pattern into ruleset
    std::vector<PatternRuleset> arm_rules;
    std::vector<ArmCode>    arm_code;

    auto pat_builder = PatternRulesetBuilder { builder.resolve() };
    pat_builder.append_from(sp, pat, outer_ty);
    for(auto& sr : pat_builder.m_rulesets)
    {
        auto pat_idx = static_cast<unsigned>(&sr - &pat_builder.m_rulesets.front());
        if( sr.m_is_impossible )
        {
            DEBUG("LET PAT #" << pat_idx << " " << pat << " ==> IMPOSSIBLE [" << sr.m_rules << "]");
        }
        else
        {
            DEBUG("LET PAT #" << pat_idx << " " << pat << " ==> [" << sr.m_rules << "]");
            arm_rules.push_back( PatternRuleset { 0, pat_idx, mv$(sr.m_rules), mv$(sr.m_bindings) } );
            ArmCode::Pattern    ap;
            auto pat_node = builder.new_bb_unlinked();
            builder.set_cur_block( pat_node );
            conv.destructure_from_list(sp, outer_ty, val.clone(), arm_rules.back().m_bindings);
            builder.end_block(MIR::Terminator::make_Goto(success_node));
            ap.code = pat_node;
            ArmCode ac;
            ac.patterns.push_back(ap);
            arm_code.push_back(ac);
        }
    }
    if( else_node )
    {
        // Emit a check (similar to match)
        TODO(sp, "Handle let-else");
    }

    MIR_LowerHIR_Match_Grouped( builder, conv, sp, outer_ty, mv$(val), mv$(arm_rules), mv$(arm_code), first_cmp_block );

    builder.set_cur_block( success_node );
}

// Handles lowering non-trivial matches to MIR
// - Non-trivial means that there's more than one pattern
void MIR_LowerHIR_Match( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val )
{
    // TODO: If any arm moves a non-Copy value, then mark `match_val` as moved
    TRACE_FUNCTION;

    bool fall_back_on_simple = false;

    const auto& match_ty = node.m_value->m_res_type;
    auto result_val = builder.new_temporary( node.m_res_type );
    auto next_block = builder.new_bb_unlinked();

    // 1. Stop the current block so we can generate code
    auto first_cmp_block = builder.pause_cur_block();

    auto match_scope = builder.new_scope_split(node.span());

    // Map of arm index to ruleset
    ::std::vector< ArmCode> arm_code;
    t_arm_rules arm_rules;
    for(unsigned int arm_idx = 0; arm_idx < node.m_arms.size(); arm_idx ++)
    {
        TRACE_FUNCTION_FR("ARM " << arm_idx, "ARM " << arm_idx);
        /*const*/ auto& arm = node.m_arms[arm_idx];
        const Span& sp = arm.m_code->span();
        ArmCode ac;

        // Register introduced bindings to be dropped on return/diverge within this scope
        auto drop_scope = builder.new_scope_var( arm.m_code->span() );
        // - Define variables from the first pattern
        conv.define_vars_from(node.span(), arm.m_patterns.front());

        ac.patterns.resize(arm.m_patterns.size());

        auto arm_body_block = builder.new_bb_unlinked();

        auto pat_scope = builder.new_scope_split(node.span());
        for( unsigned int pat_idx = 0; pat_idx < arm.m_patterns.size(); pat_idx ++ )
        {
            const auto& pat = arm.m_patterns[pat_idx];
            auto& ap = ac.patterns[pat_idx];

            // - Convert HIR pattern into ruleset
            auto pat_builder = PatternRulesetBuilder { builder.resolve() };
            pat_builder.append_from(node.span(), pat, match_ty);
            size_t first_rule = arm_rules.size();
            for(auto& sr : pat_builder.m_rulesets)
            {
                size_t i = &sr - &pat_builder.m_rulesets.front();
                if( sr.m_is_impossible )
                {
                    DEBUG("ARM PAT (" << arm_idx << "," << pat_idx << " #" << i << ") " << pat << " ==> IMPOSSIBLE [" << sr.m_rules << "]");
                }
                else
                {
                    DEBUG("ARM PAT (" << arm_idx << "," << pat_idx << " #" << i << ") " << pat << " ==> [" << sr.m_rules << "]");
                    // Ensure that all patterns bindind to the same set of variables (only check the variables)
                    if( first_rule < arm_rules.size() ) {
                        const auto& fr = arm_rules[first_rule];
                        ASSERT_BUG(sp, fr.m_bindings.size() == sr.m_bindings.size(), "Disagreement in bindings between pattern - {" << arm_rules[first_rule].m_bindings << "} vs {" << sr.m_bindings << "}");
                        for(size_t j = 0; j < fr.m_bindings.size(); j ++ ) {
                            ASSERT_BUG(sp, fr.m_bindings[j].binding->m_slot == sr.m_bindings[j].binding->m_slot, "Disagreement in bindings between pattern - {" << arm_rules[first_rule].m_bindings << "} vs {" << sr.m_bindings << "}");
                        }
                    }
                    arm_rules.push_back( PatternRuleset { arm_idx, pat_idx, mv$(sr.m_rules), mv$(sr.m_bindings) } );
                }
            }
            ap.code = builder.new_bb_unlinked();
            builder.set_cur_block( ap.code );

            // Duplicate the condition for each arm
            // - Needed, because the order has to be: match, condition, destructure, code
            if(arm.m_cond)
            {
                TRACE_FUNCTION_FR("CONDITIONAL","CONDITIONAL");
                auto freeze_scope = builder.new_scope_freeze(arm.m_cond->span());
                if( first_rule < arm_rules.size() ) {
                    conv.destructure_aliases_from_list(arm.m_code->span(), match_ty, match_val.clone(), arm_rules[first_rule].m_bindings);
                }

                auto tmp_scope = builder.new_scope_temp(arm.m_cond->span());
                conv.visit_node_ptr( arm.m_cond );
                auto cond_lval = builder.get_result_in_if_cond(arm.m_cond->span());
                builder.terminate_scope( arm.m_code->span(), mv$(tmp_scope) );
                builder.terminate_scope( arm.m_code->span(), mv$(freeze_scope) );

                ap.cond_false = builder.new_bb_unlinked();

                auto destructure_block = builder.new_bb_unlinked();
                builder.end_block(::MIR::Terminator::make_If({ mv$(cond_lval), destructure_block, ap.cond_false }));
                builder.set_cur_block( destructure_block );
            }

            // - Emit code to destructure the matched pattern
            if( first_rule < arm_rules.size() ) {
                conv.destructure_from_list(arm.m_code->span(), match_ty, match_val.clone(), arm_rules[first_rule].m_bindings);
            }
            // TODO: Previous versions had reachable=false here (causing a use-after-free), would having `true` lead to leaks?
            builder.end_split_arm( arm.m_code->span(), pat_scope, /*reachable=*/true );
            builder.end_block(::MIR::Terminator::make_Goto(arm_body_block));
        }
        builder.terminate_scope( sp, mv$(pat_scope) );

        // Condition
        // NOTE: Lack of drop due to early exit from this arm isn't an issue. All captures must be Copy
        // - The above is rustc E0008 "cannot bind by-move into a pattern guard"
        // TODO: Create a special wrapping scope for the conditions that forces any moves to use a drop flag
        if(arm.m_cond)
        {
            ac.has_condition = true;

            // NOTE: Paused so that later code (which knows what the false branch will be) can end it correctly

            // TODO: What to do with contidionals in the fast model?
            // > Could split the match on each conditional - separating such that if a conditional fails it can fall into the other compatible branches.
            fall_back_on_simple = true;
        }
        else
        {
            ac.has_condition = false;
        }

        // Code
        DEBUG("-- Body Code");

        auto tmp_scope = builder.new_scope_temp(arm.m_code->span());
        builder.set_cur_block( arm_body_block );
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
            ASSERT_BUG(node.span(), column_weights.size() == arm_rule.m_rules.size(),
                "Arm " << (&arm_rule - &arm_rules.front()) << " size doesn't match first (" << arm_rule.m_rules.size() << " != " << column_weights.size() << ")" );
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

    // TODO: Combine identical-pattern arms, allowing potential use of condtionals
    // - 
    // If there's a conditional that isn't grouped with an unconditional pattern - then force fallback

    // TODO: SplitSlice is buggy, make it fall back to simple?

    // TODO: Don't generate inner code until decisions are generated (keeps MIR flow nice)
    // - Challenging, as the decision code needs somewhere to jump to.
    // - Allocating a BB and then rewriting references to it is a possibility.

    if( fall_back_on_simple ) {
        MIR_LowerHIR_Match_Simple( builder, conv, node/*.span(), match_ty*/, mv$(match_val), mv$(arm_rules), mv$(arm_code), first_cmp_block );
    }
    else {
        MIR_LowerHIR_Match_Grouped( builder, conv, node.span(), match_ty, mv$(match_val), mv$(arm_rules), mv$(arm_code), first_cmp_block );
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
    TU_MATCH_HDRA( (x), {)
    TU_ARMA(Any, e) {
        os << "_";
        }
    // Enum variant
    TU_ARMA(Variant, e) {
        os << e.idx << " [" << e.sub_rules << "]";
        }
    // Slice pattern
    TU_ARMA(Slice, e) {
        os << "len=" << e.len << " [" << e.sub_rules << "]";
        }
    // SplitSlice
    TU_ARMA(SplitSlice, e) {
        os << "len>=" << e.min_len << " [" << e.leading << ", ..., " << e.trailing << "]";
        }
    // Boolean (different to Constant because of how restricted it is)
    TU_ARMA(Bool, e) {
        os << (e ? "true" : "false");
        }
    // General value
    TU_ARMA(Value, e) {
        os << e;
        }
    TU_ARMA(ValueRange, e) {
        os << e.first << " .." << (e.is_inclusive ? "=" : "") <<  " " << e.last;
        }
    }
    return os;
}

::Ordering PatternRule::ord(const PatternRule& x) const
{
    ORD(static_cast<int>(tag()), static_cast<int>(x.tag()));
    ORD(this->field_path, x.field_path);

    TU_MATCH_HDRA( (*this, x), {)
    TU_ARMA(Any, te, xe) { return OrdEqual; }
    TU_ARMA(Variant, te, xe) {
        if(te.idx != xe.idx)    return ::ord(te.idx, xe.idx);
        assert( te.sub_rules.size() == xe.sub_rules.size() );
        for(unsigned int i = 0; i < te.sub_rules.size(); i ++)
        {
            auto cmp = te.sub_rules[i].ord( xe.sub_rules[i] );
            if( cmp != ::OrdEqual )
                return cmp;
        }
        return ::OrdEqual;
        }
    TU_ARMA(Slice, te, xe) {
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
        }
    TU_ARMA(SplitSlice, te, xe) {
        ORD(te.leading, xe.leading);
        ORD(te.min_len, xe.min_len);
        return ::ord(te.trailing, xe.trailing);
        }
    TU_ARMA(Bool, te, xe) {
        return ::ord( te, xe );
        }
    TU_ARMA(Value, te, xe) {
        return ::ord( te, xe );
        }
    TU_ARMA(ValueRange, te, xe) {
        ORD(te.first, xe.first);
        ORD(te.last, xe.last);
        return ::ord(te.is_inclusive, xe.is_inclusive);
        }
    }
    throw "";
}
PatternRule PatternRule::clone() const
{
    struct H {
        static std::vector<PatternRule> clone_list(const std::vector<PatternRule>& l) {
            std::vector<PatternRule>    rv;
            for(const auto& e : l)
                rv.push_back(e.clone());
            return rv;
        }
        static PatternRule clone_inner(const PatternRule& t) {
            TU_MATCH_HDRA( (t), {)
            TU_ARMA(Any, te)
                return te;

            TU_ARMA(Variant, te)
                return PatternRule::make_Variant({ te.idx, H::clone_list(te.sub_rules) });
            TU_ARMA(Slice, te)
                return PatternRule::make_Slice({ te.len, H::clone_list(te.sub_rules) });
            TU_ARMA(SplitSlice, te)
                return PatternRule::make_SplitSlice({ te.min_len, te.trailing_len, H::clone_list(te.leading), H::clone_list(te.trailing) });

            TU_ARMA(Bool, te)
                return te;
            TU_ARMA(Value, te)
                return te.clone();
            TU_ARMA(ValueRange, te)
                return PatternRule::make_ValueRange({ te.first.clone(), te.last.clone(), te.is_inclusive });
            }
            throw "";
        }
    };

    auto rv = H::clone_inner(*this);
    rv.field_path = this->field_path;
    return rv;
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

    TU_MATCH_HDRA( (l,r), {)
    TU_ARMA(Any, le,re) {
        return ::OrdEqual;
        }
    TU_ARMA(Variant, le,re) {
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
        }
    TU_ARMA(Slice, le,re) {
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
        }
    TU_ARMA(SplitSlice, le,re) {
        TODO(Span(), "Order PatternRule::SplitSlice");
        }
    TU_ARMA(Bool, le,re) {
        return ::ord( le, re );
        }
    TU_ARMA(Value, le,re) {
        TODO(Span(), "Order PatternRule::Value");
        }
    TU_ARMA(ValueRange, le,re) {
        TODO(Span(), "Order PatternRule::ValueRange");
        }
    }
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
    assert(this->subset_start < this->subset_end);
    assert(this->subset_end <= m_rulesets.size());
    for(size_t i = subset_start; i < subset_end; i ++)
    {
        m_rulesets[i].m_rules.push_back(i == subset_end-1 ? std::move(r) : r.clone());
        m_rulesets[i].m_rules.back().field_path = m_field_path;
    }
}
void PatternRulesetBuilder::push_binding(PatternBinding b)
{
    assert(this->subset_start < this->subset_end);
    assert(this->subset_end <= m_rulesets.size());
    for(size_t i = subset_start; i < subset_end; i ++)
    {
        DEBUG(i << " " << b);
        m_rulesets[i].m_bindings.push_back(b);
    }
}
void PatternRulesetBuilder::push_bindings(std::vector<PatternBinding> bindings)
{
    assert(this->subset_start < this->subset_end);
    assert(this->subset_end <= m_rulesets.size());
    for(size_t i = subset_start; i < subset_end; i ++)
    {
        auto& l = m_rulesets[i].m_bindings;
        l.insert(l.end(), bindings.begin(), bindings.end());
        DEBUG(i << " [" << bindings << "] = [" << l << "]");
    }
}
void PatternRulesetBuilder::set_impossible()
{
    assert(this->subset_start < this->subset_end);
    assert(this->subset_end <= m_rulesets.size());
    for(size_t i = subset_start; i < subset_end; i ++)
    {
        m_rulesets[i].m_is_impossible = true;
    }
}
/// Multiply the current subset of the ruleset, then visit every new subset
void PatternRulesetBuilder::multiply_rulesets(size_t n, std::function<void(size_t idx)> cb)
{
    assert(n > 0);
    if( n == 1 ) {
        cb(0);
        return;
    }
    TRACE_FUNCTION_F(n);
    assert(this->subset_start < this->subset_end);
    assert(this->subset_end <= m_rulesets.size());
    size_t subset_size = this->subset_end - this->subset_start;
    size_t ofs = (n - 1) * subset_size;
    assert(ofs > 0);
    size_t new_subset_end = this->subset_start + n * subset_size;
    size_t n_tail = m_rulesets.size() - this->subset_end;
    DEBUG("subset_size=" << subset_size << ", ofs = " << ofs << ", n_tail=" << n_tail);
    m_rulesets.resize( m_rulesets.size() + (n - 1) * subset_size );
    assert(new_subset_end == m_rulesets.size() - n_tail);
    // Copy the tail out of the way (reverse to avoid chasing itself)
    for(size_t i = m_rulesets.size(); i -- > new_subset_end; )
    {
        m_rulesets[i] = std::move(m_rulesets[i-ofs]);
    }
    // Copy `n-1` copies of the current subset after itself
    for(size_t j = 1; j < n; j ++ )
    {
        for(size_t i = 0; i < subset_size; i ++)
        {
            const auto& src = m_rulesets[this->subset_start + i];
            m_rulesets[this->subset_start + j*subset_size + i] = src.clone();
        }
    }
    for(size_t j = this->subset_start+subset_size; j < new_subset_end; j += subset_size)
    {
        for(size_t i = 0; i < subset_size; i ++)
        {
            const auto& exp = m_rulesets[this->subset_start+i];
            const auto& a = m_rulesets[j+i];
            ASSERT_BUG(Span(), a.m_rules == exp.m_rules, "BUG: {" << a.m_rules << "} != {" << exp.m_rules << "}");
            ASSERT_BUG(Span(), a.m_bindings == exp.m_bindings, "BUG: {" << a.m_bindings << "} != {" << exp.m_bindings << "}");
        }
    }
    for(size_t i = this->subset_start; i < new_subset_end; i += 1)
    {
        DEBUG("#" << i << " rules=[" << m_rulesets[i].m_rules << "], bindings=[" << m_rulesets[i].m_bindings << "]");
    }

    // Iterate the new subsets
    size_t saved_start = this->subset_start;
    this->subset_end = this->subset_start;
    for(size_t i = 0; i < n; i ++)
    {
        auto orig_start = this->subset_start;
        this->subset_end += subset_size;
        DEBUG("++ " << i << " " << this->subset_start << " - " << this->subset_end);
        cb(i);
        DEBUG("-- " << i);
        assert(this->subset_start == orig_start);   // This should always be unchanged (even if the callback splits again). The end can change though.
        assert(this->subset_end >= this->subset_start + subset_size);   // The end should always be at least equal to start + size (i.e. hasn't shrunk)
        this->subset_start = this->subset_end;
    }
    // Update the subset again to cover everything
    this->subset_start = saved_start;
    // NOTE: Can't asser that the end is as-expected, as there might be inner subsets created that makes this assumption no longer valid
    //ASSERT_BUG(Span(), this->subset_end == new_subset_end, this->subset_end << " == " << new_subset_end);
    for(size_t i = this->subset_start; i < this->subset_end; i += 1)
    {
        DEBUG("#" << i << " rules=[" << m_rulesets[i].m_rules << "], bindings=[" << m_rulesets[i].m_bindings << "]");
    }
}

void PatternRulesetBuilder::append_from_lit(const Span& sp, EncodedLiteralSlice lit, const ::HIR::TypeRef& ty)
{
    TRACE_FUNCTION_F("lit="<<lit<<", ty="<<ty<<",   m_field_path=[" << m_field_path << "]");

    TU_MATCH_HDRA( (ty.data()), {)
    TU_ARMA(Infer, e)   BUG(sp, "Ivar for in match type");
    TU_ARMA(Diverge, e) BUG(sp, "Diverge in match type");
    TU_ARMA(Primitive, e) {
        switch(e)
        {
        case ::HIR::CoreType::F32:  this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Float({ lit.read_float(4), e }) ) );    break;
        case ::HIR::CoreType::F64:  this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Float({ lit.read_float(8), e }) ) );    break;

        case ::HIR::CoreType::U8:   this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Uint({lit.read_uint(1), e}) ) );    break;
        case ::HIR::CoreType::U16:  this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Uint({lit.read_uint(2), e}) ) );    break;
        case ::HIR::CoreType::U32:  this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Uint({lit.read_uint(4), e}) ) );    break;
        case ::HIR::CoreType::U64:  this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Uint({lit.read_uint(8), e}) ) );    break;
        case ::HIR::CoreType::U128: this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Uint({lit.read_uint(16), e}) ) );    break;
        case ::HIR::CoreType::Usize: this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Uint({lit.read_uint(Target_GetPointerBits()/8), e}) ) );    break;

        case ::HIR::CoreType::I8:   this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Int({lit.read_sint(1), e}) ) );    break;
        case ::HIR::CoreType::I16:  this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Int({lit.read_sint(2), e}) ) );    break;
        case ::HIR::CoreType::I32:  this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Int({lit.read_sint(4), e}) ) );    break;
        case ::HIR::CoreType::I64:  this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Int({lit.read_sint(8), e}) ) );    break;
        case ::HIR::CoreType::I128: this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Int({lit.read_sint(16), e}) ) );    break;
        case ::HIR::CoreType::Isize: this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Int({lit.read_sint(Target_GetPointerBits()/8), e}) ) );    break;

        case ::HIR::CoreType::Bool: this->push_rule( PatternRule::make_Bool(lit.read_uint(1) != 0) );    break;
        // Char is just another name for 'u32'... but with a restricted range
        case ::HIR::CoreType::Char: this->push_rule( PatternRule::make_Value( ::MIR::Constant::make_Uint({lit.read_uint(4), e}) ) );    break;
        case ::HIR::CoreType::Str:
            BUG(sp, "Hit match over `str` - must be `&str`");
            break;
        }
        }
    TU_ARMA(Tuple, e) {
        auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
        ASSERT_BUG(sp, repr, "Matching with generic constant type not valid - " << ty);
        ASSERT_BUG(sp, e.size() == repr->fields.size(), "Matching tuple with mismatched literal size - " << e.size() << " != " << repr->fields.size());

        m_field_path.push_back(0);
        for(unsigned int i = 0; i < e.size(); i ++)
        {
            this->append_from_lit(sp, lit.slice(repr->fields[i].offset), repr->fields[i].ty);
            m_field_path.back() ++;
        }
        m_field_path.pop_back();
        }
    TU_ARMA(Path, e) {
        // This is either a struct destructure or an enum
        TU_MATCH_HDRA( (e.binding), {)
        TU_ARMA(Unbound, pbe) {
            BUG(sp, "Encounterd unbound path - " << e.path);
            }
        TU_ARMA(Opaque, pbe) {
            TODO(sp, "Can an opaque path type be matched with a literal?");
            //ASSERT_BUG(sp, lit.as_List().size() == 0 , "Matching unit struct with non-empty list - " << lit);
            this->push_rule( PatternRule::make_Any({}) );
            }
        TU_ARMA(Struct, pbe) {
            auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
            ASSERT_BUG(sp, repr, "Matching with generic constant type not valid - " << ty);

            m_field_path.push_back(0);
            for(size_t i = 0; i < repr->fields.size(); i ++)
            {
                this->append_from_lit(sp, lit.slice(repr->fields[i].offset), repr->fields[i].ty);
                m_field_path.back() ++;
            }
            m_field_path.pop_back();
            }
        TU_ARMA(ExternType, pbe) {
            TODO(sp, "Match extern type");
            }
        TU_ARMA(Union, pbe) {
            TODO(sp, "Match union");
            }
        TU_ARMA(Enum, pbe) {
            auto* enm_repr = Target_GetTypeRepr(sp, m_resolve, ty);
            ASSERT_BUG(sp, enm_repr, "Matching with generic constant type not valid - " << ty);

            // TODO: Share code with `MIR_Cleanup_LiteralToRValue`
            auto var_info = enm_repr->get_enum_variant(sp, m_resolve, lit);
            unsigned var_idx = var_info.first;
            bool sub_has_tag = var_info.second;

            PatternRulesetBuilder   sub_builder { this->m_resolve };
            if(enm_repr->fields.size() > 1 || enm_repr->variants.is_None())
            {
                sub_builder.m_field_path = m_field_path;
                sub_builder.m_field_path.push_back(var_idx);

                // If the tag is in the sub-type, then ignore.
                const auto& var_ty = enm_repr->fields[var_idx].ty;
                auto var_lit = lit.slice(enm_repr->fields[var_idx].offset);
                // NOTE: The tag is only present if it's an auto-generated struct (i.e. not `()`)
                if( sub_has_tag && var_ty != HIR::TypeRef::new_unit() )
                {
                    // This inner type should be a struct
                    DEBUG("Enum variant type w/ tag field: " << var_ty);
                    auto* inner_repr = Target_GetTypeRepr(sp, m_resolve, var_ty);
                    assert(inner_repr->variants.is_None());
                    assert(inner_repr->fields.size() > 0);
                    sub_builder.m_field_path.push_back(0);
                    for(size_t i = 0; i < inner_repr->fields.size() - 1; i ++)
                    {
                        sub_builder.append_from_lit(sp, var_lit.slice(inner_repr->fields[i].offset), inner_repr->fields[i].ty);
                        sub_builder.m_field_path.back() ++;
                    }
                    sub_builder.m_field_path.pop_back();
                }
                else
                {
                    sub_builder.append_from_lit(sp, var_lit, var_ty);
                }
            }

            ASSERT_BUG(sp, sub_builder.m_rulesets.size() == 1, "Multiple rulesets generated from a literal");
            this->push_rule( PatternRule::make_Variant({ var_idx, mv$(sub_builder.m_rulesets[0].m_rules) }) );
            }
        }
        }
    TU_ARMA(Generic, e) {
        // Generics don't destructure, so the only valid pattern is `_`
        TODO(sp, "Match generic with literal?");
        this->push_rule( PatternRule::make_Any({}) );
        }
    TU_ARMA(TraitObject, e) {
        TODO(sp, "Match trait object with literal?");
        }
    TU_ARMA(ErasedType, e) {
        TODO(sp, "Match erased type with literal?");
        }
    TU_ARMA(Array, e) {
        size_t size = 0;
        ASSERT_BUG(sp, Target_GetSizeOf(sp, m_resolve, e.inner, size), "Matching with generic constant type not valid - " << ty);

        m_field_path.push_back(0);
        size_t ofs = 0;
        for(unsigned int i = 0; i < e.size.as_Known(); i ++)
        {
            this->append_from_lit(sp, lit.slice(ofs, size), e.inner);
            ofs += size;
            m_field_path.back() ++;
        }
        m_field_path.pop_back();
        }
    TU_ARMA(Slice, e) {
#if 0
        size_t size = 0;
        ASSERT_BUG(sp, Target_GetSizeOf(sp, m_resolve, e.inner, size), "Matching with generic constant type not valid - " << ty);

        ASSERT_BUG(sp, lit.is_List(), "Matching array with non-list literal - " << lit);
        const auto& list = lit.as_List();

        PatternRulesetBuilder   sub_builder { this->m_resolve };
        sub_builder.m_field_path = m_field_path;
        sub_builder.m_field_path.push_back(0);
        for(const auto& val : list)
        {
            sub_builder.append_from_lit( sp, val, e.inner );
            sub_builder.m_field_path.back() ++;
        }
        // Encodes length check and sub-pattern rules
        this->push_rule( PatternRule::make_Slice({ static_cast<unsigned int>(list.size()), mv$(sub_builder.m_rules) }) );
#else
        TODO(sp, "Match literal Slice");
#endif
        }
    TU_ARMA(Borrow, e) {
        m_field_path.push_back( FIELD_DEREF );
        if( e.inner == ::HIR::CoreType::Str ) {
            auto ptr_size = Target_GetPointerBits()/8;
            auto ptr = lit.read_uint(ptr_size);
            auto len = lit.slice(ptr_size, ptr_size).read_uint(ptr_size);
            auto* r = lit.get_reloc();
            ASSERT_BUG(sp, r, "Null relocation for string in pattern generation");
            ASSERT_BUG(sp, ptr >= EncodedLiteral::PTR_BASE, "");
            ptr -= EncodedLiteral::PTR_BASE;

            ASSERT_BUG(sp, !r->p, "TODO: Handle &str match constant with non-string relocation - " << *r->p);
            ASSERT_BUG(sp, ptr <= r->bytes.size(), "");
            ASSERT_BUG(sp, len <= r->bytes.size(), "");
            ASSERT_BUG(sp, ptr+len <= r->bytes.size(), "");

            this->push_rule(PatternRule::make_Value( std::string(r->bytes.data() + ptr, r->bytes.data() + ptr + len) ));
        }
        else {
            TODO(sp, "Match literal Borrow: ty=" << ty << " lit=" << lit);
        }
        m_field_path.pop_back();
        }
    TU_ARMA(Pointer, e) {
        TODO(sp, "Match literal with pointer?");
        }
    TU_ARMA(Function, e) {
        ERROR(sp, E0000, "Attempting to match over a functon pointer");
        }
    TU_ARMA(Closure, e) {
        ERROR(sp, E0000, "Attempting to match over a closure");
        }
    TU_ARMA(Generator, e) {
        ERROR(sp, E0000, "Attempting to match over a generator");
        }
    }
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
                return EncodedLiteralSlice(e.binding->m_value_res).read_uint();
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
                return EncodedLiteralSlice(e.binding->m_value_res).read_float();
                )
            )
            throw "";
        }

        static MIR::Constant get_pattern_value(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::Pattern::Value& val, const ::HIR::CoreType& e) {
            switch(e)
            {
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64:
                // Yes, this is valid.
                return ::MIR::Constant::make_Float({ H::get_pattern_value_float(sp, pat, val), e});
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::U128:
            case ::HIR::CoreType::Usize:
                return ::MIR::Constant::make_Uint({ H::get_pattern_value_int(sp, pat, val), e });
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::I128:
            case ::HIR::CoreType::Isize:
                return ::MIR::Constant::make_Int({ static_cast<int64_t>(H::get_pattern_value_int(sp, pat, val)), e });
            case ::HIR::CoreType::Bool:
                BUG(sp, "Can't range match on Bool");
                break;
            case ::HIR::CoreType::Char:
                // Char is just another name for 'u32'... but with a restricted range
                return ::MIR::Constant::make_Uint({ H::get_pattern_value_int(sp, pat, val), e });
            case ::HIR::CoreType::Str:
                BUG(sp, "Hit match over `str` - must be `&str`");
                break;
            }
            throw "";
        }
        static MIR::Constant get_pattern_value_min(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::CoreType& e) {
            switch(e)
            {
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64:
                // Yes, this is valid.
                return ::MIR::Constant::make_Float({ -std::numeric_limits<double>::infinity(), e});
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::U128:
            case ::HIR::CoreType::Usize:
                return ::MIR::Constant::make_Uint({ 0, e });
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::I128:
            case ::HIR::CoreType::Isize:
                return ::MIR::Constant::make_Int({ INT64_MIN, e });
            case ::HIR::CoreType::Bool:
                BUG(sp, "Can't range match on Bool");
                break;
            case ::HIR::CoreType::Char:
                // Char is just another name for 'u32'... but with a restricted range
                return ::MIR::Constant::make_Uint({ 0, e });
            case ::HIR::CoreType::Str:
                BUG(sp, "Hit match over `str` - must be `&str`");
                break;
            }
            throw "";
        }
        static MIR::Constant get_pattern_value_max(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::CoreType& e) {
            switch(e)
            {
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64:
                // Yes, this is valid.
                return ::MIR::Constant::make_Float({ std::numeric_limits<double>::infinity(), e});
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::U128:
            case ::HIR::CoreType::Usize:
                return ::MIR::Constant::make_Uint({ UINT64_MAX, e });
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::I128:
            case ::HIR::CoreType::Isize:
                return ::MIR::Constant::make_Int({ INT64_MAX, e });
            case ::HIR::CoreType::Bool:
                BUG(sp, "Can't range match on Bool");
                break;
            case ::HIR::CoreType::Char:
                // Char is just another name for 'u32'... but with a restricted range
                return ::MIR::Constant::make_Uint({ UINT64_MAX, e });
            case ::HIR::CoreType::Str:
                BUG(sp, "Hit match over `str` - must be `&str`");
                break;
            }
            throw "";
        }
    };

    for(const auto& pb : pat.m_bindings)
    {
        auto path = m_field_path;
        for(size_t i = 0; i < pb.m_implicit_deref_count; i ++)
        {
            path.push_back(FIELD_DEREF);
        }

        this->push_binding(PatternBinding(path, pb));
    }

    const auto* ty_p = &top_ty;
    for(size_t i = 0; i < pat.m_implicit_deref_count; i ++)
    {
        if( !ty_p->data().is_Borrow() )
            BUG(sp, "Deref step " << i << "/" << pat.m_implicit_deref_count << " hit a non-borrow " << *ty_p << " from " << top_ty);
        ty_p = &ty_p->data().as_Borrow().inner;
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

    if(pat.m_data.is_Or())
    {
        // Multiply the current pattern (sub)set out, visit with sub-sets
        const auto& e = pat.m_data.as_Or();
        assert(pat.m_implicit_deref_count == 0);    // Shouldn't have any, so this code doesn't need to pop them.
        assert(e.size() > 0);
        this->multiply_rulesets(e.size(), [&](size_t i){
            this->append_from(sp, e[i], top_ty);
            });
        return ;
    }

    TU_MATCH_HDRA( (ty.data()), {)
    TU_ARMA(Infer, e) {
        BUG(sp, "Ivar for in match type");
        }
    TU_ARMA(Diverge, e) {
        // Since ! can never exist, mark this arm as impossible.
        // TODO: Marking as impossible (and not emitting) leads to exhuaustiveness failure.
        //this->m_is_impossible = true;
        }
    TU_ARMA(Primitive, e) {
        TU_MATCH_HDR( (pat.m_data), {)
        default:
            BUG(sp, "Matching primitive with invalid pattern - " << pat);
        TU_ARM(pat.m_data, Any, pe) {
            this->push_rule( PatternRule::make_Any({}) );
            }
        TU_ARM(pat.m_data, Range, pe) {
            if( !pe.start || !pe.end )
            {
                assert(pe.start || pe.end);
                if(pe.start)
                {
                    this->push_rule( PatternRule::make_ValueRange({
                        H::get_pattern_value(sp, pat, *pe.start, e),
                        H::get_pattern_value_max(sp, pat, e),
                        true    // Inclusive always
                        }) );
                }
                else
                {
                    this->push_rule( PatternRule::make_ValueRange({
                        H::get_pattern_value_min(sp, pat, e),
                        H::get_pattern_value(sp, pat, *pe.end, e),
                        pe.is_inclusive
                        }) );
                }
            }
            else
            {
                this->push_rule( PatternRule::make_ValueRange({
                    H::get_pattern_value(sp, pat, *pe.start, e),
                    H::get_pattern_value(sp, pat, *pe.end, e),
                    pe.is_inclusive
                    }) );
            }
            }
        TU_ARM(pat.m_data, Value, pe) {
            switch(e)
            {
            case ::HIR::CoreType::Bool:
                // TODO: Support values from `const` too
                this->push_rule( PatternRule::make_Bool( pe.val.as_Integer().value != 0 ) );
                break;
            default:
                this->push_rule( H::get_pattern_value(sp, pat, pe.val, e) );
                break;
            }
            }
        }
        }
    TU_ARMA(Tuple, e) {
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
                    this->append_from(sp, empty_pattern, e[i]);
                else
                    this->append_from(sp, pe.trailing[i-trailing_start], e[i]);
                m_field_path.back() ++;
            }
            )
        )
        m_field_path.pop_back();
        }
    TU_ARMA(Path, e) {
        struct PH {
            static void push_pattern_tuple(
                PatternRulesetBuilder& builder, const Span& sp,const ::HIR::Pattern::Data::Data_PathTuple& pe,
                std::function<const HIR::TypeRef&(const HIR::TypeRef&)> maybe_monomorph
                )
            {
                const auto& sd = ::HIR::pattern_get_tuple(sp, pe.path, pe.binding);
                assert( sd.size() >= pe.leading.size() + pe.trailing.size() );
                size_t trailing_start = sd.size() - pe.trailing.size();
                for(unsigned int i = 0; i < sd.size(); i ++)
                {
                    const auto& fld = sd[i];

                    if( i < pe.leading.size() )
                    {
                        builder.append_from(sp, pe.leading[i], maybe_monomorph(fld.ent));
                    }
                    else if( i < trailing_start )
                    {
                        builder.append_from(sp, empty_pattern, maybe_monomorph(fld.ent));
                    }
                    else
                    {
                        builder.append_from(sp, pe.trailing[i - trailing_start], maybe_monomorph(fld.ent));
                    }
                    builder.m_field_path.back() ++;
                }
            }
            static void push_pattern_struct(
                PatternRulesetBuilder& builder, const Span& sp,const ::HIR::Pattern::Data::Data_PathNamed& pe,
                std::function<const HIR::TypeRef&(const HIR::TypeRef&)> maybe_monomorph
                )
            {
                const auto& sd = ::HIR::pattern_get_named(sp, pe.path, pe.binding);
                // NOTE: Iterates in field order (not pattern order) to ensure that patterns are in order between arms
                for(const auto& fld : sd)
                {
                    const auto& sty_mono = maybe_monomorph(fld.second.ent);

                    auto it = ::std::find_if( pe.sub_patterns.begin(), pe.sub_patterns.end(), [&](const auto& x){ return x.first == fld.first; } );
                    if( it == pe.sub_patterns.end() )
                    {
                        builder.append_from(sp, empty_pattern, sty_mono);
                    }
                    else
                    {
                        builder.append_from(sp, it->second, sty_mono);
                    }
                    builder.m_field_path.back() ++;
                }
            }
        };
        ::HIR::TypeRef  tmp;
        auto maybe_monomorph = [&](const auto& ty)->const ::HIR::TypeRef& {
            if(monomorphise_type_needed(ty)) {
                tmp = MonomorphStatePtr(nullptr, &e.path.m_data.as_Generic().m_params, nullptr).monomorph_type(sp, ty);
                this->m_resolve.expand_associated_types(sp, tmp);
                return tmp;
            }
            else {
                return ty;
            }
        };
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
            TU_MATCH_HDRA( (str_data), {)
            TU_ARMA(Unit, sd) {
                TU_MATCH_HDRA( (pat.m_data), {)
                default:
                    BUG(sp, "Match not allowed, " << ty <<  " with " << pat);
                TU_ARMA(Any, pe) {
                    // _ on a unit-like type, unconditional
                    }
                TU_ARMA(PathValue, pe) {
                    // Unit-like struct value, nothing to match (it's unconditional)
                    }
                TU_ARMA(Value, pe) {
                    // Unit-like struct value, nothing to match (it's unconditional)
                    }
                TU_ARMA(PathNamed, pe) {
                    ASSERT_BUG(sp, pe.sub_patterns.size() == 0, "Matching unit-like struct with sub-patterns - " << pat);
                    }
                }
                }
            TU_ARMA(Tuple, sd) {
                m_field_path.push_back(0);
                TU_MATCH_HDRA( (pat.m_data), {)
                default:
                    BUG(sp, "Match not allowed, " << ty <<  " with " << pat);
                TU_ARMA(Any, pe) {
                    // - Recurse into type using an empty pattern
                    for(const auto& fld : sd)
                    {
                        this->append_from(sp, empty_pattern, maybe_monomorph(fld.ent));
                        m_field_path.back() ++;
                    }
                    }
                TU_ARMA(PathTuple, pe) {
                    assert( pe.binding.is_Struct() );
                    PH::push_pattern_tuple(*this, sp, pe, maybe_monomorph);
                    }
                }
                m_field_path.pop_back();
                }
            TU_ARMA(Named, sd) {
                TU_MATCH_HDRA( (pat.m_data), {)
                default:
                    BUG(sp, "Match not allowed, " << ty <<  " with " << pat);
                TU_ARMA(Any, pe) {
                    m_field_path.push_back(0);
                    for(const auto& fld : sd)
                    {
                        this->append_from(sp, empty_pattern, maybe_monomorph(fld.second.ent));
                        m_field_path.back() ++;
                    }
                    m_field_path.pop_back();
                    }
                TU_ARMA(PathNamed, pe) {
                    assert( pe.binding.is_Struct() );
                    m_field_path.push_back(0);
                    PH::push_pattern_struct(*this, sp, pe, maybe_monomorph);
                    m_field_path.pop_back();
                    }
                }
                }
            }
            }
        TU_ARMA(Union, pbe) {
            TU_MATCH_HDRA( (pat.m_data), {)
            default:
                TODO(sp, "Match over union - " << ty << " with " << pat);
            TU_ARMA(Any, pe) {
                this->push_rule( PatternRule::make_Any({}) );
                }
            }
            }
        TU_ARMA(ExternType, pbe) {
            TU_MATCH_HDRA( (pat.m_data), {)
            default:
                BUG(sp, "Match not allowed, " << ty <<  " with " << pat);
            TU_ARMA(Any, pe) {
                this->push_rule( PatternRule::make_Any({}) );
                }
            }
            }
        TU_ARMA(Enum, pbe) {
            TU_MATCH_HDRA( (pat.m_data), {)
            default:
                BUG(sp, "Match not allowed, " << ty <<  " with " << pat);
            TU_ARMA(Any, pe) {
                this->push_rule( PatternRule::make_Any({}) );
                }
            TU_ARMA(Value, pe) {
                if( ! pe.val.is_Named() )
                    BUG(sp, "Match not allowed, " << ty << " with " << pat);
                // TODO: If the value of this constant isn't known at this point (i.e. it won't be until monomorphisation)
                //       emit a special type of rule.
                TODO(sp, "Match enum with const - " << pat);
                }
            TU_ARMA(PathValue, pe) {
                assert(pe.binding.is_Enum());
                this->push_rule( PatternRule::make_Variant( {pe.binding.as_Enum().var_idx, {} } ) );
                }
            TU_ARMA(PathTuple, pe) {
                assert(pe.binding.is_Enum());
                const auto& be = pe.binding.as_Enum();

                PatternRulesetBuilder   sub_builder { this->m_resolve };
                sub_builder.m_field_path = m_field_path;
                sub_builder.m_field_path.push_back(be.var_idx);
                sub_builder.m_field_path.push_back(0);

                PH::push_pattern_tuple(sub_builder, sp, pe, maybe_monomorph);

                this->multiply_rulesets(sub_builder.m_rulesets.size(), [&](size_t i) {
                    auto& sr = sub_builder.m_rulesets[i];
                    if( sr.m_is_impossible )
                        this->set_impossible();
                    this->push_rule( PatternRule::make_Variant({ be.var_idx, mv$(sr.m_rules) }) );
                    this->push_bindings( mv$(sr.m_bindings) );
                    });
                }
            TU_ARMA(PathNamed, pe) {
                assert(pe.binding.is_Enum());
                const auto& be = pe.binding.as_Enum();

                PatternRulesetBuilder   sub_builder { this->m_resolve };
                sub_builder.m_field_path = m_field_path;
                sub_builder.m_field_path.push_back(be.var_idx);
                sub_builder.m_field_path.push_back(0);

                // Empty variants can be matched with `Var { [..] }` even if they're not struct-like
                if( be.ptr->is_value() ) {
                    assert( pe.sub_patterns.empty() );
                }
                else if( be.ptr->m_data.as_Data().at(be.var_idx).type == HIR::TypeRef::new_unit() ) {
                    assert( pe.sub_patterns.empty() );
                }
                else if( !be.ptr->m_data.as_Data().at(be.var_idx).is_struct ) {
                    assert( pe.sub_patterns.empty() );
                    const auto& sd = ::HIR::pattern_get_tuple(sp, pe.path, pe.binding);
                    for(unsigned int i = 0; i < sd.size(); i ++)
                    {
                        const auto& fld = sd[i];
                        sub_builder.append_from(sp, empty_pattern, maybe_monomorph(fld.ent));
                        sub_builder.m_field_path.back() ++;
                    }
                }
                else {
                    PH::push_pattern_struct(sub_builder, sp, pe, maybe_monomorph);
                }

                this->multiply_rulesets(sub_builder.m_rulesets.size(), [&](size_t i) {
                    auto& sr = sub_builder.m_rulesets[i];
                    if( sr.m_is_impossible )
                        this->set_impossible();
                    this->push_rule( PatternRule::make_Variant({ be.var_idx, mv$(sr.m_rules) }) );
                    this->push_bindings( mv$(sr.m_bindings) );
                    });
                }
            }
            }
        }
        }
    TU_ARMA(Generic, e) {
        // Generics don't destructure, so the only valid pattern is `_`
        TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
        ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
        (Any,
            this->push_rule( PatternRule::make_Any({}) );
            )
        )
        }
    TU_ARMA(TraitObject, e) {
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a trait object");
        }
        }
    TU_ARMA(ErasedType, e) {
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over an erased type");
        }
        }
    TU_ARMA(Array, e) {
        // If the size is unknown, just push a `_` pattern.
        // OR: don't push anything?
        if( !e.size.is_Known() ) {
            DEBUG("Matching over unknown-sized array - " << e.size);
            ASSERT_BUG(sp, pat.m_data.is_Any(), "Matching generic-sized array with non `_` pattern - " << pat);
            this->push_rule( PatternRule::make_Any({}) );
            break;
        }
        // Sequential match just like tuples.
        m_field_path.push_back(0);
        TU_MATCH_HDRA( (pat.m_data), {)
        default:
            BUG(sp, "Matching array with invalid pattern - " << pat);
        TU_ARMA(Any, pe) {
            for(unsigned int i = 0; i < e.size.as_Known(); i ++) {
                this->append_from(sp, empty_pattern, e.inner);
                m_field_path.back() ++;
            }
            }
        TU_ARMA(Slice, pe) {
            ASSERT_BUG(sp, e.size.as_Known() == pe.sub_patterns.size(), "Pattern size mismatch");
            for(const auto& v : pe.sub_patterns) {
                this->append_from(sp, v, e.inner);
                m_field_path.back() ++;
            }
            }
        TU_ARMA(SplitSlice, pe) {
            ASSERT_BUG(sp, pe.leading.size() < FIELD_INDEX_MAX, "Too many leading slice rules to fit encodng");
            ASSERT_BUG(sp, pe.leading.size() < e.size.as_Known(), "Too many leading slice rules for array type");
            ASSERT_BUG(sp, pe.trailing.size() < e.size.as_Known(), "Too many leading slice rules for array type");
            for(const auto& subpat : pe.leading)
            {
                this->append_from( sp, subpat, e.inner );
                m_field_path.back() ++;
            }
            while(m_field_path.back() < e.size.as_Known() - pe.trailing.size())
            {
                this->append_from(sp, empty_pattern, e.inner);
                m_field_path.back() ++;
            }
            for(const auto& subpat : pe.trailing)
            {
                this->append_from( sp, subpat, e.inner );
                m_field_path.back() ++;
            }

            if(pe.extra_bind.is_valid())
            {
                TODO(sp, "Insert binding for SplitSlice (Array)");
            }
            }
        }
        m_field_path.pop_back();
        }
    TU_ARMA(Slice, e) {
        TU_MATCH_HDRA( (pat.m_data), {)
        default:
            BUG(sp, "Matching over [T] with invalid pattern - " << pat);
        TU_ARMA(Any, pe) {
            this->push_rule( PatternRule::make_Any({}) );
            }
        TU_ARMA(Slice, pe) {
            // Sub-patterns
            PatternRulesetBuilder   sub_builder { this->m_resolve };
            sub_builder.m_field_path = m_field_path;
            sub_builder.m_field_path.push_back(0);
            ASSERT_BUG(sp, pe.sub_patterns.size() < FIELD_INDEX_MAX, "Too many slice rules to fit encodng");
            for(const auto& subpat : pe.sub_patterns)
            {
                sub_builder.append_from( sp, subpat, e.inner );
                sub_builder.m_field_path.back() ++;
            }

            // Encodes length check and sub-pattern rules
            this->multiply_rulesets(sub_builder.m_rulesets.size(), [&](size_t i) {
                auto& sr = sub_builder.m_rulesets[i];
                if( sr.m_is_impossible )
                    this->set_impossible();
                this->push_rule( PatternRule::make_Slice({ static_cast<unsigned int>(pe.sub_patterns.size()), mv$(sr.m_rules) }) );
                this->push_bindings(mv$(sr.m_bindings));
                });
            }
        TU_ARMA(SplitSlice, pe) {
            PatternRulesetBuilder   sub_builder { this->m_resolve };
            sub_builder.m_field_path = m_field_path;
            ASSERT_BUG(sp, pe.leading.size() < FIELD_INDEX_MAX, "Too many leading slice rules to fit encodng");
            sub_builder.m_field_path.push_back(0);
            for(const auto& subpat : pe.leading)
            {
                sub_builder.append_from( sp, subpat, e.inner );
                sub_builder.m_field_path.back() ++;
            }
            auto leading_rulesets = mv$(sub_builder.m_rulesets);
            sub_builder.m_rulesets.clear();
            sub_builder.m_rulesets.resize(1);

            if( pe.trailing.size() )
            {
                // Needs a way of encoding the negative offset in the field path
                // - For now, just use a very high number (and assert that it's not more than 128)
                ASSERT_BUG(sp, pe.trailing.size() < FIELD_INDEX_MAX, "Too many trailing slice rules to fit encodng");
                sub_builder.m_field_path.back() = FIELD_INDEX_MAX + (FIELD_INDEX_MAX - pe.trailing.size());
                for(const auto& subpat : pe.trailing)
                {
                    sub_builder.append_from( sp, subpat, e.inner );
                    sub_builder.m_field_path.back() ++;
                }
            }
            auto trailing_rulesets = mv$(sub_builder.m_rulesets);

            if(pe.extra_bind.is_valid())
            {
                ASSERT_BUG(sp, pe.extra_bind.m_implicit_deref_count == 0, "");
                PatternBinding  pb(m_field_path, pe.extra_bind);
                pb.split_slice = std::make_pair( pe.leading.size(), pe.trailing.size() );
                this->push_binding(mv$(pb));
            }

            this->multiply_rulesets(leading_rulesets.size() * trailing_rulesets.size(), [&](size_t i) {
                size_t i_l = i % leading_rulesets.size();
                size_t i_t = i / leading_rulesets.size();
                auto& sr_l = leading_rulesets[i_l];
                auto& sr_t = trailing_rulesets[i_t];
                if(sr_l.m_is_impossible || sr_t.m_is_impossible)
                    this->set_impossible();

                this->push_rule( PatternRule::make_SplitSlice({
                    static_cast<unsigned int>(pe.leading.size() + pe.trailing.size()),
                    static_cast<unsigned int>(pe.trailing.size()),
                    mv$(sr_l.m_rules), mv$(sr_t.m_rules)
                    }) );
                this->push_bindings(mv$(sr_l.m_bindings));
                this->push_bindings(mv$(sr_t.m_bindings));
                });
            }
        }
        }
    TU_ARMA(Borrow, e) {
        m_field_path.push_back( FIELD_DEREF );
        TU_MATCH_HDR( (pat.m_data), {)
        default:
            BUG(sp, "Matching borrow invalid pattern - " << ty << " with " << pat);
        TU_ARM(pat.m_data, Any, pe) {
            this->append_from( sp, empty_pattern, e.inner );
            }
        TU_ARM(pat.m_data, Ref, pe) {
            this->append_from( sp, *pe.sub, e.inner );
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
    TU_ARMA(Pointer, e) {
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a pointer");
        }
        }
    TU_ARMA(Function, e) {
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a functon pointer");
        }
        }
    TU_ARMA(Closure, e) {
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a closure");
        }
        }
    TU_ARMA(Generator, e) {
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a generator");
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

        TU_MATCH_HDRA( (a, b), { )
        TU_ARMA(Any, ae, be) {
            return OrdEqual;
            }
        TU_ARMA(Variant, ae, be) {
            return ::ord(ae.idx, be.idx);
            }
        TU_ARMA(Slice, ae, be) {
            return ::ord(ae.len, be.len);
            }
        TU_ARMA(SplitSlice, ae, be) {
            ORD(ae.leading , be.leading );
            // TODO: lengths?
            ORD(ae.trailing, be.trailing);
            return OrdEqual;
            }
        TU_ARMA(Bool, ae, be) {
            return ::ord(ae, be);
            }
        TU_ARMA(Value, ae, be) {
            return ::ord(ae, be);
            }
        TU_ARMA(ValueRange, ae, be) {
            ORD(ae.first, be.first);
            ORD(ae.last, be.last);
            return ::ord(ae.is_inclusive, be.is_inclusive);
            }
        }
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

        // Checks if the value is within the righthand edge of the range
        auto is_within_right = [](const MIR::Constant& c, const PatternRule::Data_ValueRange& e)->bool {
            return (e.is_inclusive ? c <= e.last : c < e.last);
            };

        // Value Range: Overlaps with contained values.
        if(const auto* ae = a.opt_ValueRange() )
        {
            if(const auto* be = b.opt_Value() )
            {
                return ( ae->first <= *be && is_within_right(*be, *ae) );
            }
            else if( const auto* be = b.opt_ValueRange() )
            {
                auto check_ends = []( const PatternRule::Data_ValueRange& lo, const PatternRule::Data_ValueRange& hi)->bool {
                    return lo.is_inclusive == hi.is_inclusive ? lo.last <= hi.last
                        : (lo.is_inclusive
                            ? lo.last < hi.last // Lower side is inclusive, higher side exlusive - must be less than higher side
                            : throw "TODO" // Lower side is excl, higher side incl - lower+1 < higher = lower < higher-1 = lower
                            );
                    };
                assert(ae->is_inclusive && "TODO: Exclusive ranges");
                assert(be->is_inclusive && "TODO: Exclusive ranges");
                // Start of B within A
                if( ae->first <= be->first && is_within_right(be->first, *ae) )
                    return true;
                // End of B within A
                if( is_within_right(ae->first, *be) && be->last <= ae->last ) // TODO: Right-exclusive (if equal type then original check, otherwise complex)
                    return true;
                // Start of A within B
                if( be->first <= ae->first && is_within_right(ae->first, *be) )
                    return true;
                // End of A within B
                if( is_within_right(be->first, *ae) && ae->last <= be->last ) // TODO: Right-exclusive
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
                if(be->is_inclusive)
                {
                    return (be->first <= *ae && *ae <= be->last);
                }
                else
                {
                    return (be->first <= *ae && *ae < be->last);
                }
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

    // NOTE: Assumption kinda breaks with byte string literals
    //for(size_t i = 0; i < rulesets.size(); i ++)
    //    assert(rulesets[i].size() == rulesets[0].size());

    // Multiple rules, but no checks within then (can happen with `match () { _ if foo => ..., _ => ... }`)
    if(rulesets[0].size() == 0)
        return ;

    bool found_non_any = false;
    for(size_t i = 0; i < rulesets.size(); i ++)
    {
        assert(idx < rulesets[i].size());
        if( !rulesets[i][idx].is_Any() )
            found_non_any = true;
    }
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
            DEBUG("> " << *cur_ty << " #" << idx);

            TU_MATCH_HDRA( (cur_ty->data()), {)
            TU_ARMA(Infer, e)   BUG(sp, "Ivar for in match type");
            TU_ARMA(Diverge, e) BUG(sp, "Diverge in match type");
            TU_ARMA(Primitive, e)   BUG(sp, "Destructuring a primitive");
            TU_ARMA(Tuple, e) {
                ASSERT_BUG(sp, idx < e.size(), "Tuple index out of range");
                lval = ::MIR::LValue::new_Field(mv$(lval), idx);
                cur_ty = &e[idx];
                }
            TU_ARMA(Path, e) {
                if( idx == FIELD_DEREF ) {
                    // TODO: Check that the path is Box
                    lval = ::MIR::LValue::new_Deref( mv$(lval) );
                    cur_ty = &e.path.m_data.as_Generic().m_params.m_types.at(0);
                    break;
                }
                auto monomorph_to_ptr = [&](const auto& ty)->const auto* {
                    if( monomorphise_type_needed(ty) ) {
                        auto rv = MonomorphStatePtr(nullptr, &e.path.m_data.as_Generic().m_params, nullptr).monomorph_type(sp, ty);
                        resolve.expand_associated_types(sp, rv);
                        tmp_ty = mv$(rv);
                        return &tmp_ty;
                    }
                    else {
                        return &ty;
                    }
                    };
                TU_MATCH_HDRA( (e.binding), {)
                TU_ARMA(Unbound, pbe) {
                    BUG(sp, "Encounterd unbound path - " << e.path);
                    }
                TU_ARMA(Opaque, pbe) {
                    BUG(sp, "Destructuring an opaque type - " << *cur_ty);
                    }
                TU_ARMA(ExternType, pbe) {
                    BUG(sp, "Destructuring an extern type - " << *cur_ty);
                    }
                TU_ARMA(Struct, pbe) {
                    TU_MATCH_HDRA( (pbe->m_data), { )
                    TU_ARMA(Unit, fields) {
                        BUG(sp, "Destructuring an unit-like tuple - " << *cur_ty);
                        }
                    TU_ARMA(Tuple, fields) {
                        ASSERT_BUG(sp, idx < fields.size(), "Tuple struct index (" << idx << ") out of range (" << fields.size() << ") in " << *cur_ty);
                        const auto& fld = fields[idx];
                        cur_ty = monomorph_to_ptr(fld.ent);
                        lval = ::MIR::LValue::new_Field(mv$(lval), idx);
                        }
                    TU_ARMA(Named, fields) {
                        ASSERT_BUG(sp, idx < fields.size(), "Tuple struct index (" << idx << ") out of range (" << fields.size() << ") in " << *cur_ty);
                        const auto& fld = fields[idx].second;
                        cur_ty = monomorph_to_ptr(fld.ent);
                        lval = ::MIR::LValue::new_Field(mv$(lval), idx);
                        }
                    }
                    }
                TU_ARMA(Union, pbe) {
                    ASSERT_BUG(sp, idx < pbe->m_variants.size(), "Union variant index (" << idx << ") out of range (" << pbe->m_variants.size() << ") in " << *cur_ty);
                    const auto& fld = pbe->m_variants[idx];
                    cur_ty = monomorph_to_ptr(fld.second.ent);
                    lval = ::MIR::LValue::new_Downcast(mv$(lval), idx);
                    }
                TU_ARMA(Enum, pbe) {
                    ASSERT_BUG(sp, pbe->m_data.is_Data(), "Value enum being destructured - " << *cur_ty);
                    const auto& variants = pbe->m_data.as_Data();
                    ASSERT_BUG(sp, idx < variants.size(), "Variant index (" << idx << ") out of range (" << variants.size() <<  ") for enum " << *cur_ty);
                    const auto& var = variants[idx];

                    cur_ty = monomorph_to_ptr(var.type);
                    lval = ::MIR::LValue::new_Downcast(mv$(lval), idx);
                    }
                }
                }
            TU_ARMA(Generic, e) {
                BUG(sp, "Destructuring a generic - " << *cur_ty);
                }
            TU_ARMA(TraitObject, e) {
                BUG(sp, "Destructuring a trait object - " << *cur_ty);
                }
            TU_ARMA(ErasedType, e) {
                BUG(sp, "Destructuring an erased type - " << *cur_ty);
                }
            TU_ARMA(Array, e) {
                cur_ty = &e.inner;
                if( idx < FIELD_INDEX_MAX ) {
                    ASSERT_BUG(sp, idx < e.size.as_Known(), "Index out of range");
                    lval = ::MIR::LValue::new_Field(mv$(lval), idx);
                }
                else {
                    idx -= FIELD_INDEX_MAX;
                    idx = FIELD_INDEX_MAX - idx;
                    ASSERT_BUG(sp, idx < e.size.as_Known(), "Index out of range");
                    TODO(sp, "Index " << idx << " from end of array " << lval);
                }
                }
            TU_ARMA(Slice, e) {
                cur_ty = &e.inner;
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
                }
            TU_ARMA(Borrow, e) {
                ASSERT_BUG(sp, idx == FIELD_DEREF, "Destructure of borrow doesn't correspond to a deref in the path");
                //DEBUG(i << " " << *cur_ty << " - " << cur_ty << " " << &tmp_ty);
                if( cur_ty == &tmp_ty ) {
                    tmp_ty = HIR::TypeRef(tmp_ty.data().as_Borrow().inner);
                }
                else {
                    cur_ty = &e.inner;
                }
                //DEBUG(i << " " << *cur_ty);
                lval = ::MIR::LValue::new_Deref(mv$(lval));
                }
            TU_ARMA(Pointer, e) {
                ERROR(sp, E0000, "Attempting to match over a pointer");
                }
            TU_ARMA(Function, e) {
                ERROR(sp, E0000, "Attempting to match over a functon pointer");
                }
            TU_ARMA(Closure, e) {
                ERROR(sp, E0000, "Attempting to match over a closure");
                }
            TU_ARMA(Generator, e) {
                ERROR(sp, E0000, "Attempting to match over a generator");
                }
            }
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
            builder.end_block( ::MIR::Terminator::make_Goto(arm_code.patterns[i].code) );

            // - Go to code/condition check
            if( arm_code.has_condition )
            {
                builder.set_cur_block( arm_code.patterns[i].cond_false );
                builder.end_block( ::MIR::Terminator::make_Goto(next_arm_bb) );
            }

            if( !is_last_pat )
            {
                builder.set_cur_block( next_pattern_bb );
            }
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
        TU_MATCH_HDRA( (ty.data()), {)
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
                TU_MATCH_HDRA((rule), {)
                default:
                    BUG(sp, "PatternRule for integer is not Value or ValueRange");
                TU_ARMA(Value, re) {
                    auto succ_bb = builder.new_bb_unlinked();

                    auto test_val = ::MIR::Param( ::MIR::Constant::make_Uint({ re.as_Uint().v, te }));
                    builder.push_stmt_assign(sp, builder.get_if_cond(), ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::EQ, mv$(test_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ builder.get_if_cond(), succ_bb, fail_bb }) );
                    builder.set_cur_block(succ_bb);
                    }
                TU_ARMA(ValueRange, re) {
                    auto succ_bb = builder.new_bb_unlinked();

                    // IF `val` < `first` : fail_bb
                    if( re.first.as_Uint().v != 0 ) {
                        auto test_bb_2 = builder.new_bb_unlinked();
                        auto test_lt_val = ::MIR::Param(::MIR::Constant::make_Uint({ re.first.as_Uint().v, te }));
                        auto cmp_lt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::LT, mv$(test_lt_val) }));
                        builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lt_lval), fail_bb, test_bb_2 }) );

                        builder.set_cur_block(test_bb_2);
                    }

                    // IF `val` > `last` : fail_bb
                    if(re.last.as_Uint().v == UINT64_MAX && re.is_inclusive) {
                        builder.end_block( ::MIR::Terminator::make_Goto({ succ_bb }) );
                    }
                    else {
                        auto test_gt_val = ::MIR::Param(::MIR::Constant::make_Uint({ re.last.as_Uint().v, te }));
                        auto op = re.is_inclusive ?  ::MIR::eBinOp::GT : ::MIR::eBinOp::GE;
                        auto cmp_gt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), op, mv$(test_gt_val) }));
                        builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_gt_lval), fail_bb, succ_bb }) );
                    }

                    builder.set_cur_block(succ_bb);
                    }
                }
                break;
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::I128:
            case ::HIR::CoreType::Isize:
                TU_MATCH_HDRA((rule), {)
                default:
                    BUG(sp, "PatternRule for integer is not Value or ValueRange");
                TU_ARMA(Value, re) {
                    auto succ_bb = builder.new_bb_unlinked();

                    auto test_val = ::MIR::Param(::MIR::Constant::make_Int({ re.as_Int().v, te }));
                    auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::EQ, mv$(test_val) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), succ_bb, fail_bb }) );
                    builder.set_cur_block(succ_bb);
                    }
                TU_ARMA(ValueRange, re) {
                    auto succ_bb = builder.new_bb_unlinked();

                    // IF `val` < `first` : fail_bb
                    if( re.first.as_Int().v != INT64_MIN ) {
                        auto test_bb_2 = builder.new_bb_unlinked();
                        auto test_lt_val = ::MIR::Param(::MIR::Constant::make_Int({ re.first.as_Int().v, te }));
                        auto cmp_lt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::LT, mv$(test_lt_val) }));
                        builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lt_lval), fail_bb, test_bb_2 }) );
                        builder.set_cur_block(test_bb_2);
                    }

                    // IF `val` > `last` : fail_bb
                    if(re.last.as_Int().v == INT64_MAX && re.is_inclusive) {
                        builder.end_block( ::MIR::Terminator::make_Goto({ succ_bb }) );
                    }
                    else {
                        auto test_gt_val = ::MIR::Param(::MIR::Constant::make_Int({ re.last.as_Int().v, te }));
                        auto op = re.is_inclusive ?  ::MIR::eBinOp::GT : ::MIR::eBinOp::GE;
                        auto cmp_gt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), op, mv$(test_gt_val) }));
                        builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_gt_lval), fail_bb, succ_bb }) );
                    }

                    builder.set_cur_block(succ_bb);
                    }
                }
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

                    // IF `val` < `first` : fail_bb
                    if( re.first.as_Uint().v != 0 ) {
                        auto test_bb_2 = builder.new_bb_unlinked();

                        auto test_lt_val = ::MIR::Param(::MIR::Constant::make_Uint({ re.first.as_Uint().v, te }));
                        auto cmp_lt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::LT, mv$(test_lt_val) }));
                        builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lt_lval), fail_bb, test_bb_2 }) );

                        builder.set_cur_block(test_bb_2);
                    }

                    // IF `val` > `last` : fail_bb
                    if(re.last.as_Uint().v == UINT64_MAX ) {
                        assert(re.is_inclusive);
                        builder.end_block( ::MIR::Terminator::make_Goto({ succ_bb }) );
                    }
                    else {
                        auto test_gt_val = ::MIR::Param(::MIR::Constant::make_Uint({ re.last.as_Uint().v, te }));
                        auto op = re.is_inclusive ?  ::MIR::eBinOp::GT : ::MIR::eBinOp::GE;
                        auto cmp_gt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), op, mv$(test_gt_val) }));
                        builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_gt_lval), fail_bb, succ_bb }) );
                    }

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

                    // IF `val` < `first` : fail_bb
                    if( re.first.as_Float().v == -std::numeric_limits<double>::infinity()) {
                    }
                    else {
                        auto test_bb_2 = builder.new_bb_unlinked();
                        auto test_lt_val = ::MIR::Param(::MIR::Constant::make_Float({ re.first.as_Float().v, te }));
                        auto cmp_lt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::LT, mv$(test_lt_val) }));
                        builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lt_lval), fail_bb, test_bb_2 }) );
                        builder.set_cur_block(test_bb_2);
                    }

                    // IF `val` > `last` : fail_bb
                    if( re.first.as_Float().v == std::numeric_limits<double>::infinity() && re.is_inclusive ) {
                        builder.end_block( ::MIR::Terminator::make_Goto({ succ_bb }) );
                    }
                    else {
                        auto test_gt_val = ::MIR::Param(::MIR::Constant::make_Float({ re.last.as_Float().v, te }));
                        auto op = re.is_inclusive ?  ::MIR::eBinOp::GT : ::MIR::eBinOp::GE;
                        auto cmp_gt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), op, mv$(test_gt_val) }));
                        builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_gt_lval), fail_bb, succ_bb }) );
                    }

                    builder.set_cur_block(succ_bb);
                    )
                )
                break;
            case ::HIR::CoreType::Str: {
                ASSERT_BUG(sp, rule.is_Value() && rule.as_Value().is_StaticString(), "Unexpected use of non-value pattern on `str`");
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
            TU_MATCH_HDRA( (te.binding), {)
            TU_ARMA(Unbound, pbe) {
                BUG(sp, "Encounterd unbound path - " << te.path);
                }
            TU_ARMA(Opaque, pbe) {
                BUG(sp, "Attempting to match over opaque type - " << ty);
                }
            TU_ARMA(Struct, pbe) {
                const auto& str_data = pbe->m_data;
                TU_MATCH_HDRA( (str_data), {)
                TU_ARMA(Unit, sd) {
                    BUG(sp, "Attempting to match over unit type - " << ty);
                    }
                TU_ARMA(Tuple, sd) {
                    TODO(sp, "Matching on tuple-like struct?");
                    }
                TU_ARMA(Named, sd) {
                    TODO(sp, "Matching on struct?");
                    }
                }
                }
            TU_ARMA(Union, pbe) {
                TODO(sp, "Match over Union");
                }
            TU_ARMA(ExternType, pbe) {
                TODO(sp, "Match over ExternType");
                }
            TU_ARMA(Enum, pbe) {
                auto monomorph = [&](const auto& ty) {
                    auto rv = MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr).monomorph_type(sp, ty);
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
                }   // TypePathBinding::Enum
            }
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
                ASSERT_BUG(sp, te.inner == ::HIR::CoreType::U8, "Bytes pattern on non-&[u8]");
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

                MIR_LowerHIR_Match_Simple__GeneratePattern(builder, sp,
                    re.trailing.data(), re.trailing.size(),
                    top_ty, top_val, field_path_ofs,
                    fail_bb
                );
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
        TU_ARMA(Generator, te) {
            BUG(sp, "Attempting to match a generator");
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
            os << ": [" << *x.rule_sets[i] << "]";
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

    void gen_dispatch_range(const field_path_t& field_path, const ::MIR::Constant& first, const ::MIR::Constant& last, bool is_inclusive, ::MIR::BasicBlockId def_blk);
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
        TU_MATCH_HDRA( (rule), {)
        TU_ARMA(Variant, e) {
            auto sub_rules = mv$(e.sub_rules);
            out_rules.push_back( mv$(rule) );
            for(auto& sr : sub_rules)
                push_flat_rules(out_rules, mv$(sr));
            }
        TU_ARMA(Slice, e) {
            auto sub_rules = mv$(e.sub_rules);
            out_rules.push_back( mv$(rule) );
            for(auto& sr : sub_rules)
                push_flat_rules(out_rules, mv$(sr));
            }
        TU_ARMA(SplitSlice, e) {
            auto leading = mv$(e.leading);
            auto trailing = mv$(e.trailing);
            auto idx = out_rules.size();
            out_rules.push_back( mv$(rule) );
            for(auto& sr : leading)
                push_flat_rules(out_rules, mv$(sr));
            // Trailing rules are complex as they break the assumption that patterns across the same type share a prefix
            // - So, flatten them into the "flattened" rule
            for(auto& sr : trailing)
                push_flat_rules(out_rules[idx].as_SplitSlice().trailing, mv$(sr));
            }
        TU_ARMA(Bool, e) {
            out_rules.push_back( mv$(rule) );
            }
        TU_ARMA(Value, e) {
            out_rules.push_back( mv$(rule) );
            }
        TU_ARMA(ValueRange, e) {
            out_rules.push_back( mv$(rule) );
            }
        TU_ARMA(Any, e) {
            out_rules.push_back( mv$(rule) );
            }
        }
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
        MirBuilder& builder, MirConverter& conv, const Span& sp, const HIR::TypeRef& match_ty, ::MIR::LValue match_val,
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

    auto inst = MatchGenGrouped { builder, sp, match_ty, match_val, arms_code, 0 };

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
                const auto& ap = ac.patterns[ai.second];

                m_builder.end_block( ::MIR::Terminator::make_Goto(ap.code) );

                if( ac.has_condition )
                {
                    TODO(sp, "Handle conditionals in Grouped");
                    // TODO: If the condition fails, this should re-try the match on other rules that could have worked.
                    // - For now, conditionals are disabled.

                    // TODO: What if there's multiple patterns on this condition?
                    // - For now, only the first pattern gets edited.
                    // - Maybe clone the blocks used for the condition?

                    // Check for marking in `ac` that the block has already been terminated, assert that target is `next`
                    if( ai.second == 0 )
                    {
                        if( ap.cond_fail_tgt != 0 )
                        {
                            ASSERT_BUG(sp, ap.cond_fail_tgt == next, "Condition fail target already set with mismatching arm, set to bb" << ap.cond_fail_tgt << " cur is bb" << next);
                        }
                        else
                        {
                            ap.cond_fail_tgt = next;

                            m_builder.set_cur_block( ap.cond_false );
                            m_builder.end_block( ::MIR::Terminator::make_Goto(next) );
                        }
                    }

                    if( next != default_arm )
                        m_builder.set_cur_block(next);
                }
                else
                {
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
                this->gen_dispatch_range(arm_rules[first_any][ofs].field_path, e->first, e->last, e->is_inclusive, next);
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

/// <summary>
/// Generate dispatch code for the provided pattern list
/// </summary>
/// <param name="rules">A list of equivalent pattern rules (at the given offset)</param>
/// <param name="ofs">Offset into sub-patterns</param>
/// <param name="arm_targets">Target blocks for each arm in `rules`</param>
/// <param name="def_blk">Default block for if no arm matched</param>
void MatchGenGrouped::gen_dispatch(const ::std::vector<t_rules_subset>& rules, size_t ofs, const ::std::vector<::MIR::BasicBlockId>& arm_targets, ::MIR::BasicBlockId def_blk)
{
    const auto& field_path = rules[0][0][ofs].field_path;
    TRACE_FUNCTION_F("rules=["<<rules <<"], ofs=" << ofs <<", field_path=" << field_path);
    
    // Assert that all patterns combined here are over the same field
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

    TU_MATCH_HDRA( (ty.data()), {)
    TU_ARMA(Infer, te) {
        BUG(sp, "Hit _ in type - " << ty);
        }
    TU_ARMA(Diverge, te) {
        BUG(sp, "Matching over !");
        }
    TU_ARMA(Primitive, te) {
        this->gen_dispatch__primitive(mv$(ty), mv$(val), rules, ofs, arm_targets, def_blk);
        }
    TU_ARMA(Path, te) {
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
            TU_MATCH_HDRA( (str_data), {)
            TU_ARMA(Unit, sd) {
                BUG(sp, "Attempting to match over unit type - " << ty);
                }
            TU_ARMA(Tuple, sd) {
                TODO(sp, "Matching on tuple-like struct?");
                }
            TU_ARMA(Named, sd) {
                TODO(sp, "Matching on struct? - " << ty);
                }
            }
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
        }
    TU_ARMA(Generic, te) {
        BUG(sp, "Attempting to match a generic");
        }
    TU_ARMA(TraitObject, te) {
        BUG(sp, "Attempting to match a trait object");
        }
    TU_ARMA(ErasedType, te) {
        BUG(sp, "Attempting to match an erased type");
        }
    TU_ARMA(Array, te) {
        // Byte strings?
        // Remove the deref on the &str
        ASSERT_BUG(sp, !val.m_wrappers.empty() && val.m_wrappers.back().is_Deref(), "&[T; N] match on non-Deref lvalue - " << val);
        val.m_wrappers.pop_back();

        ::std::vector< ::MIR::BasicBlockId> targets;
        ::std::vector< ::std::vector<uint8_t> >   values;
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
            values.push_back( re.as_Bytes() );

            tgt_ofs += rules[i].size();
        }
        m_builder.end_block( ::MIR::Terminator::make_SwitchValue({
            mv$(val), def_blk, mv$(targets), ::MIR::SwitchValues(mv$(values))
            }) );
        }
    TU_ARMA(Slice, te) {
        this->gen_dispatch__slice(mv$(ty), mv$(val), rules, ofs, arm_targets, def_blk);
        }
    TU_ARMA(Tuple, te) {
        BUG(sp, "Match directly on tuple");
        }
    TU_ARMA(Borrow, te) {
        BUG(sp, "Match directly on borrow");
        }
    TU_ARMA(Pointer, te) {
        // TODO: Could this actually be valid?
        BUG(sp, "Attempting to match a pointer - " << ty);
        }
    TU_ARMA(Function, te) {
        // TODO: Could this actually be valid?
        BUG(sp, "Attempting to match a function pointer - " << ty);
        }
    TU_ARMA(Closure, te) {
        BUG(sp, "Attempting to match a closure");
        }
    TU_ARMA(Generator, te) {
        BUG(sp, "Attempting to match a generator");
        }
    }
}

void MatchGenGrouped::gen_dispatch__primitive(::HIR::TypeRef ty, ::MIR::LValue val, const ::std::vector<t_rules_subset>& rules, size_t ofs, const ::std::vector<::MIR::BasicBlockId>& arm_targets, ::MIR::BasicBlockId def_blk)
{
    auto te = ty.data().as_Primitive();
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
    auto& te = ty.data().as_Path();
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

            auto val_tst_len = ::MIR::Constant::make_Uint({ b.size(), ::HIR::CoreType::Usize });

            // IF v == tst : target
            {
                auto next_cmp_blk = m_builder.new_bb_unlinked();

                // TODO: What if `val` isn't a Deref?
                ASSERT_BUG(sp, !val.m_wrappers.empty() && val.m_wrappers.back().is_Deref(), "TODO: Handle non-Deref matches of byte strings - " << val);
                auto cmp_slice_val = m_builder.lvalue_or_temp(sp,
                    ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef::new_slice(::HIR::CoreType::U8) ),
                    ::MIR::RValue::make_MakeDst({ ::MIR::Param(re->clone()), val_tst_len.clone() })
                    );
                auto cmp_lval_eq = this->push_compare( val.clone_unwrapped(), ::MIR::eBinOp::EQ, mv$(cmp_slice_val) );
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval_eq), arm_targets[tgt_ofs], next_cmp_blk }) );

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


void MatchGenGrouped::gen_dispatch_range(const field_path_t& field_path, const ::MIR::Constant& first, const ::MIR::Constant& last, bool is_inclusive, ::MIR::BasicBlockId def_blk)
{
    TRACE_FUNCTION_F("field_path="<<field_path<<", " << first << " .." << (is_inclusive ? "=" : "") << " " << last);
    ::MIR::LValue   val;
    ::HIR::TypeRef  ty;
    get_ty_and_val(sp, m_builder, m_top_ty, m_top_val,  field_path, m_field_path_ofs,  ty, val);
    DEBUG("ty = " << ty << ", val = " << val);

    if( const auto* tep = ty.data().opt_Primitive() )
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
            upper_possible = is_inclusive ? (last.as_Uint().v < UINT64_MAX) : true;
            break;
        case ::HIR::CoreType::I8:
        case ::HIR::CoreType::I16:
        case ::HIR::CoreType::I32:
        case ::HIR::CoreType::I64:
        case ::HIR::CoreType::I128:
        case ::HIR::CoreType::Isize:
            lower_possible = (first.as_Int().v > INT64_MIN);
            upper_possible = is_inclusive ? (last.as_Int().v < INT64_MAX) : true;
            break;
        case ::HIR::CoreType::Char:
            lower_possible = (first.as_Uint().v > 0);
            upper_possible = is_inclusive ? (last.as_Uint().v <= 0x10FFFF) : (last.as_Uint().v < 0x10FFFF);
            break;
        case ::HIR::CoreType::F32:
        case ::HIR::CoreType::F64:
            // NOTE: No upper or lower limits
            lower_possible = (first.as_Float().v > -std::numeric_limits<double>::infinity());
            upper_possible = (last .as_Float().v <  std::numeric_limits<double>::infinity());
            break;
        }

        if( lower_possible )
        {
            auto test_bb_2 = m_builder.new_bb_unlinked();
            // IF `val` < `first` : fail_bb
            auto cmp_lt_lval = m_builder.get_rval_in_if_cond(sp, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), ::MIR::eBinOp::LT, ::MIR::Param(first.clone()) }));
            m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lt_lval), def_blk, test_bb_2 }) );

            m_builder.set_cur_block(test_bb_2);
        }


        if( upper_possible )
        {
            auto succ_bb = m_builder.new_bb_unlinked();

            // IF `val` > `last` : fail_bb
            auto op = is_inclusive ? ::MIR::eBinOp::GT : ::MIR::eBinOp::GE;
            auto cmp_gt_lval = m_builder.get_rval_in_if_cond(sp, ::MIR::RValue::make_BinOp({ ::MIR::Param(val.clone()), op, ::MIR::Param(last.clone()) }));
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
    ASSERT_BUG(sp, ty.data().is_Slice(), "SplitSlice pattern on non-slice - " << ty);

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
    // TODO: This is dead code (leading patterns should have been expanded, and there's an assert above for it)
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

