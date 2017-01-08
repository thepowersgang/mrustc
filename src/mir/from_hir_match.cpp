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
    // _ pattern
    (Any, struct {}),
    // Enum variant
    (Variant, struct { unsigned int idx; ::std::vector<PatternRule> sub_rules; }),
    // Slice (includes desired length)
    (Slice, struct { unsigned int len; ::std::vector<PatternRule> sub_rules; }),
    // SplitSlice
    // TODO: How can the negative offsets in the `trailing` be handled correctly? (both here and in the destructure)
    (SplitSlice, struct { unsigned int min_len; ::std::vector<PatternRule> leading, trailing; }),
    // Boolean (different to Constant because of how restricted it is)
    (Bool, bool),
    // General value
    (Value, ::MIR::Constant),
    (ValueRange, struct { ::MIR::Constant first, last; })
    ),
    ( , field_path(mv$(x.field_path)) ), (field_path = mv$(x.field_path);),
    (
        field_path_t    field_path;
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
    ::MIR::BasicBlockId   cond_end;
    ::MIR::LValue   cond_lval;
    ::std::vector< ::MIR::BasicBlockId> destructures;   // NOTE: Incomplete
};

typedef ::std::vector<PatternRuleset>  t_arm_rules;

void MIR_LowerHIR_Match_Simple( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arm_code, ::MIR::BasicBlockId first_cmp_block);
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
                return !builder.lvalue_is_copy( sp, ::MIR::LValue::make_Variable( pat.m_binding.m_slot) );
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

    bool has_move_pattern = false;
    for(const auto& arm : node.m_arms)
    {
        for(const auto& pat : arm.m_patterns)
        {
            has_move_pattern |= H::is_pattern_move(node.span(), builder, pat);
            if( has_move_pattern )
                break ;
        }
        if( has_move_pattern )
            break ;
    }

    auto match_scope = builder.new_scope_split(node.span());

    // Map of arm index to ruleset
    ::std::vector< ArmCode> arm_code;
    t_arm_rules arm_rules;
    for(unsigned int arm_idx = 0; arm_idx < node.m_arms.size(); arm_idx ++)
    {
        TRACE_FUNCTION_FR("ARM " << arm_idx, "ARM" << arm_idx);
        /*const*/ auto& arm = node.m_arms[arm_idx];
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

            // - Emit code to destructure the matched pattern
            ac.destructures.push_back( builder.new_bb_unlinked() );
            builder.set_cur_block( ac.destructures.back() );
            conv.destructure_from( arm.m_code->span(), pat, match_val.clone(), true );
            builder.end_split_arm( arm.m_code->span(), pat_scope, /*reachable=*/false );    // HACK: Mark as not reachable, this scope isn't for codegen.
            builder.pause_cur_block();
            // NOTE: Paused block resumed upon successful match
        }
        builder.terminate_scope( arm.m_code->span(), mv$(pat_scope) );

        // TODO: If this pattern ignores fields with Drop impls, this will lead to leaks.
        // - Ideally, this would trigger a drop of whatever wasn't already taken by the pattern.
        if( has_move_pattern )
        {
            builder.moved_lvalue(node.span(), match_val);
        }

        // Condition
        // NOTE: Lack of drop due to early exit from this arm isn't an issue. All captures must be Copy
        // - The above is rustc E0008 "cannot bind by-move into a pattern guard"
        if(arm.m_cond)
        {
            DEBUG("-- Condition Code");
            ac.has_condition = true;
            ac.cond_start = builder.new_bb_unlinked();
            builder.set_cur_block( ac.cond_start );

            auto tmp_scope = builder.new_scope_temp(arm.m_cond->span());
            conv.visit_node_ptr( arm.m_cond );
            ac.cond_lval = builder.get_result_in_lvalue(arm.m_cond->span(), ::HIR::TypeRef(::HIR::CoreType::Bool));
            // NOTE: Terminating the scope slightly early is safe, because the resulting boolean temp isn't invalidated.
            builder.terminate_scope( arm.m_code->span(), mv$(tmp_scope) );
            ac.cond_end = builder.pause_cur_block();

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

        ac.code = builder.new_bb_unlinked();
        auto tmp_scope = builder.new_scope_temp(arm.m_code->span());
        builder.set_cur_block( ac.code );
        conv.visit_node_ptr( arm.m_code );

        if( !builder.block_active() && !builder.has_result() ) {
            DEBUG("Arm diverged");
            // Nothing need be done, as the block diverged.
            // - Drops were handled by the diverging block (if not, the below will panic)
            { auto _ = mv$(tmp_scope); }
            { auto _ = mv$(drop_scope); }
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
        DEBUG("> (" << arm_rule.arm_idx << ", " << arm_rule.pat_idx << ") - " << arm_rule.m_rules);
    }

    // TODO: Don't generate inner code until decisions are generated (keeps MIR flow nice)

    // TODO: Detect if a rule is ordering-dependent. In this case we currently have to fall back on the simple match code
    // - A way would be to search for `_` rules with non _ rules following. Would false-positive in some cases, but shouldn't false negative
    // TODO: Merge equal rulesets if there's one with no condition.

    if( fall_back_on_simple ) {
        MIR_LowerHIR_Match_Simple( builder, conv, node, mv$(match_val), mv$(arm_rules), mv$(arm_code), first_cmp_block );
    }
    else {
        MIR_LowerHIR_Match_DecisionTree( builder, conv, node, mv$(match_val), mv$(arm_rules), mv$(arm_code), first_cmp_block );
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
            this->push_rule( PatternRule::make_Value( ::MIR::Constant(val) ) );
            } break;
        case ::HIR::CoreType::U8:
        case ::HIR::CoreType::U16:
        case ::HIR::CoreType::U32:
        case ::HIR::CoreType::U64:
        case ::HIR::CoreType::U128:
        case ::HIR::CoreType::Usize: {
            ASSERT_BUG(sp, lit.is_Integer(), "Matching integer type with non-integer literal - " << lit);
            uint64_t val = lit.as_Integer();
            this->push_rule( PatternRule::make_Value( ::MIR::Constant(val) ) );
            } break;
        case ::HIR::CoreType::I8:
        case ::HIR::CoreType::I16:
        case ::HIR::CoreType::I32:
        case ::HIR::CoreType::I64:
        case ::HIR::CoreType::I128:
        case ::HIR::CoreType::Isize: {
            ASSERT_BUG(sp, lit.is_Integer(), "Matching integer type with non-integer literal - " << lit);
            int64_t val = static_cast<int64_t>( lit.as_Integer() );
            this->push_rule( PatternRule::make_Value( ::MIR::Constant(val) ) );
            } break;
        case ::HIR::CoreType::Bool:
            ASSERT_BUG(sp, lit.is_Integer(), "Matching boolean with non-integer literal - " << lit);
            this->push_rule( PatternRule::make_Bool( lit.as_Integer() != 0 ) );
            break;
        case ::HIR::CoreType::Char: {
            // Char is just another name for 'u32'... but with a restricted range
            ASSERT_BUG(sp, lit.is_Integer(), "Matching char with non-integer literal - " << lit);
            uint64_t val = lit.as_Integer();
            this->push_rule( PatternRule::make_Value( ::MIR::Constant(val) ) );
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
        (Union,
            TODO(sp, "Match union");
            ),
        (Enum,
            ASSERT_BUG(sp, lit.is_Variant(), "Matching enum non-variant literal - " << lit);
            auto var_idx = lit.as_Variant().idx;
            const auto& list = lit.as_Variant().vals;
            auto monomorph = [&](const auto& ty) {
                auto rv = monomorphise_type(sp, pbe->m_params, e.path.m_data.as_Generic().m_params, ty);
                this->m_resolve.expand_associated_types(sp, rv);
                return rv;
                };

            ASSERT_BUG(sp, var_idx < pbe->m_variants.size(), "Literal refers to a variant out of range");
            const auto& var_def = pbe->m_variants.at(var_idx);

            PatternRulesetBuilder   sub_builder { this->m_resolve };
            sub_builder.m_field_path = m_field_path;
            sub_builder.m_field_path.push_back(0);

            TU_MATCH( ::HIR::Enum::Variant, (var_def.second), (fields_def),
            (Unit,
                ),
            (Value,
                ),
            (Tuple,
                ASSERT_BUG(sp, fields_def.size() == list.size(), "");

                for( unsigned int i = 0; i < list.size(); i ++ )
                {
                    sub_builder.m_field_path.back() = i;
                    const auto& val = list[i];
                    const auto& ty_tpl = fields_def[i].ent;

                    ::HIR::TypeRef  tmp;
                    const auto& subty = (monomorphise_type_needed(ty_tpl) ? tmp = monomorph(ty_tpl) : ty_tpl);

                    sub_builder.append_from_lit( sp, val, subty );
                }
                ),
            (Struct,
                ASSERT_BUG(sp, fields_def.size() == list.size(), "");

                for( unsigned int i = 0; i < list.size(); i ++ )
                {
                    sub_builder.m_field_path.back() = i;
                    const auto& val = list[i];
                    const auto& ty_tpl = fields_def[i].second.ent;

                    ::HIR::TypeRef  tmp;
                    const auto& subty = (monomorphise_type_needed(ty_tpl) ? tmp = monomorph(ty_tpl) : ty_tpl);

                    sub_builder.append_from_lit( sp, val, subty );
                }
                )
            )

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
void PatternRulesetBuilder::append_from(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::TypeRef& ty)
{
    TRACE_FUNCTION_F("pat="<<pat<<", ty="<<ty<<",   m_field_path=[" << m_field_path << "]");
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

    // TODO: Outer handling for Value::Named patterns
    // - Convert them into either a pattern, or just a variant of this function that operates on ::HIR::Literal
    //  > It does need a way of handling unknown-value constants (e.g. <GenericT as Foo>::CONST)
    //  > Those should lead to a simple match? Or just a custom rule type that indicates that they're checked early
    TU_IFLET( ::HIR::Pattern::Data, pat.m_data, Value, pe,
        TU_IFLET( ::HIR::Pattern::Value, pe.val, Named, pve,
            if( pve.binding )
            {
                this->append_from_lit(sp, pve.binding->m_value_res, ty);
                return ;
            }
            else
            {
                TODO(sp, "Match with an unbound constant - " << pve.path);
            }
        )
    )

    TU_MATCHA( (ty.m_data), (e),
    (Infer,   BUG(sp, "Ivar for in match type"); ),
    (Diverge,
        // Since ! can never exist, mark this arm as impossible.
        // TODO: Marking as impossible (and not emitting) leads to exhuaustiveness failure.
        //this->m_is_impossible = true;
        ),
    (Primitive,
        TU_MATCH_DEF(::HIR::Pattern::Data, (pat.m_data), (pe),
        ( BUG(sp, "Matching primitive with invalid pattern - " << pat); ),
        (Any,
            this->push_rule( PatternRule::make_Any({}) );
            ),
        (Range,
            switch(e)
            {
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64: {
                double start = H::get_pattern_value_float(sp, pat, pe.start);
                double end   = H::get_pattern_value_float(sp, pat, pe.end  );
                this->push_rule( PatternRule::make_ValueRange( {::MIR::Constant(start), ::MIR::Constant(end)} ) );
                } break;
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::U128:
            case ::HIR::CoreType::Usize: {
                uint64_t start = H::get_pattern_value_int(sp, pat, pe.start);
                uint64_t end   = H::get_pattern_value_int(sp, pat, pe.end  );
                this->push_rule( PatternRule::make_ValueRange( {::MIR::Constant(start), ::MIR::Constant(end)} ) );
                } break;
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::I128:
            case ::HIR::CoreType::Isize: {
                int64_t start = H::get_pattern_value_int(sp, pat, pe.start);
                int64_t end   = H::get_pattern_value_int(sp, pat, pe.end  );
                this->push_rule( PatternRule::make_ValueRange( {::MIR::Constant(start), ::MIR::Constant(end)} ) );
                } break;
            case ::HIR::CoreType::Bool:
                BUG(sp, "Can't range match on Bool");
                break;
            case ::HIR::CoreType::Char: {
                uint64_t start = H::get_pattern_value_int(sp, pat, pe.start);
                uint64_t end   = H::get_pattern_value_int(sp, pat, pe.end  );
                this->push_rule( PatternRule::make_ValueRange( {::MIR::Constant(start), ::MIR::Constant(end)} ) );
                } break;
            case ::HIR::CoreType::Str:
                BUG(sp, "Hit match over `str` - must be `&str`");
                break;
            }
            ),
        (Value,
            switch(e)
            {
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64: {
                // Yes, this is valid.
                double val = H::get_pattern_value_float(sp, pat, pe.val);
                this->push_rule( PatternRule::make_Value( ::MIR::Constant(val) ) );
                } break;
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::U128:
            case ::HIR::CoreType::Usize: {
                uint64_t val = H::get_pattern_value_int(sp, pat, pe.val);
                this->push_rule( PatternRule::make_Value( ::MIR::Constant(val) ) );
                } break;
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::I128:
            case ::HIR::CoreType::Isize: {
                int64_t val = H::get_pattern_value_int(sp, pat, pe.val);
                this->push_rule( PatternRule::make_Value( ::MIR::Constant(val) ) );
                } break;
            case ::HIR::CoreType::Bool:
                // TODO: Support values from `const` too
                this->push_rule( PatternRule::make_Bool( pe.val.as_Integer().value != 0 ) );
                break;
            case ::HIR::CoreType::Char: {
                // Char is just another name for 'u32'... but with a restricted range
                uint64_t val = H::get_pattern_value_int(sp, pat, pe.val);
                this->push_rule( PatternRule::make_Value( ::MIR::Constant(val) ) );
                } break;
            case ::HIR::CoreType::Str:
                BUG(sp, "Hit match over `str` - must be `&str`");
                break;
            }
            )
        )
        ),
    (Tuple,
        m_field_path.push_back(0);
        TU_MATCH_DEF(::HIR::Pattern::Data, (pat.m_data), (pe),
        ( BUG(sp, "Matching tuple with invalid pattern - " << pat); ),
        (Any,
            for(const auto& sty : e) {
                this->append_from(sp, pat, sty);
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
            assert(e.size() > pe.leading.size() + pe.trailing.size());
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
        ),
    (Path,
        // This is either a struct destructure or an enum
        TU_MATCHA( (e.binding), (pbe),
        (Unbound,
            BUG(sp, "Encounterd unbound path - " << e.path);
            ),
        (Opaque,
            TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
            ( BUG(sp, "Matching opaque type with invalid pattern - " << pat); ),
            (Any,
                this->push_rule( PatternRule::make_Any({}) );
                )
            )
            ),
        (Struct,
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
                    this->append_from(sp, pat, inner_ty);
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
                    // - Recurse into type and use the same pattern again
                    for(const auto& fld : sd)
                    {
                        ::HIR::TypeRef  tmp;
                        const auto& sty_mono = (monomorphise_type_needed(fld.ent) ? tmp = monomorph(fld.ent) : fld.ent);
                        this->append_from(sp, pat, sty_mono);
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
                        this->append_from(sp, pat, sty_mono);
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
                            ::HIR::Pattern  any_pat {};
                            this->append_from(sp, any_pat, sty_mono);
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
            ),
        (Union,
            TODO(sp, "Match over union - " << ty);
            ),
        (Enum,
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
                const auto& var_def = pe.binding_ptr->m_variants.at(pe.binding_idx);

                const auto& fields_def = var_def.second.as_Tuple();
                PatternRulesetBuilder   sub_builder { this->m_resolve };
                sub_builder.m_field_path = m_field_path;
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
                const auto& var_def = pe.binding_ptr->m_variants.at(pe.binding_idx);
                const auto& fields_def = var_def.second.as_Struct();
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
            )
        )
        ),
    (Generic,
        // Generics don't destructure, so the only valid pattern is `_`
        TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
        ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
        (Any,
            this->push_rule( PatternRule::make_Any({}) );
            )
        )
        ),
    (TraitObject,
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a trait object");
        }
        ),
    (ErasedType,
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over an erased type");
        }
        ),
    (Array,
        // Sequential match just like tuples.
        m_field_path.push_back(0);
        TU_MATCH_DEF(::HIR::Pattern::Data, (pat.m_data), (pe),
        ( BUG(sp, "Matching array with invalid pattern - " << pat); ),
        (Any,
            for(unsigned int i = 0; i < e.size_val; i ++) {
                this->append_from(sp, pat, *e.inner);
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
        ),
    (Slice,
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
            sub_builder.m_field_path.push_back(0);
            for(const auto& subpat : pe.leading)
            {
                sub_builder.append_from( sp, subpat, *e.inner );
                sub_builder.m_field_path.back() ++;
            }
            auto leading = mv$(sub_builder.m_rules);

            sub_builder.m_field_path.back() = 0;
            if( pe.trailing.size() )
            {
                TODO(sp, "SplitSlice on [T] with trailing - " << pat);
            }
            auto trailing = mv$(sub_builder.m_rules);

            this->push_rule( PatternRule::make_SplitSlice({
                static_cast<unsigned int>(pe.leading.size() + pe.trailing.size()),
                mv$(leading), mv$(trailing)
                }) );
            )
        )
        ),
    (Borrow,
        m_field_path.push_back( FIELD_DEREF );
        TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
        ( BUG(sp, "Matching borrow invalid pattern - " << pat); ),
        (Any,
            this->append_from( sp, pat, *e.inner );
            ),
        (Ref,
            this->append_from( sp, *pe.sub, *e.inner );
            ),
        (Value,
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
            )
        )
        m_field_path.pop_back();
        ),
    (Pointer,
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a pointer");
        }
        ),
    (Function,
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a functon pointer");
        }
        ),
    (Closure,
        if( pat.m_data.is_Any() ) {
        }
        else {
            ERROR(sp, E0000, "Attempting to match over a closure");
        }
        )
    )
}

namespace {
    void get_ty_and_val(
        const Span& sp, const StaticTraitResolve& resolve,
        const ::HIR::TypeRef& top_ty, const ::MIR::LValue& top_val,
        const field_path_t& field_path, unsigned int field_path_ofs,
        /*Out ->*/ ::HIR::TypeRef& out_ty, ::MIR::LValue& out_val
        )
    {
        ::MIR::LValue   lval = top_val.clone();
        ::HIR::TypeRef  tmp_ty;
        const ::HIR::TypeRef* cur_ty = &top_ty;

        // TODO: Cache the correspondance of path->type (lval can be inferred)
        ASSERT_BUG(sp, field_path_ofs <= field_path.size(), "Field path offset " << field_path_ofs << " is larger than the path [" << field_path << "]");
        for(unsigned int i = field_path_ofs; i < field_path.size(); i ++ )
        {
            auto idx = field_path.data[i];

            TU_MATCHA( (cur_ty->m_data), (e),
            (Infer,   BUG(sp, "Ivar for in match type"); ),
            (Diverge, BUG(sp, "Diverge in match type");  ),
            (Primitive,
                BUG(sp, "Destructuring a primitive");
                ),
            (Tuple,
                ASSERT_BUG(sp, idx < e.size(), "Tuple index out of range");
                lval = ::MIR::LValue::make_Field({ box$(lval), idx });
                cur_ty = &e[idx];
                ),
            (Path,
                if( idx == FIELD_DEREF ) {
                    // TODO: Check that the path is Box
                    lval = ::MIR::LValue::make_Deref({ box$(lval) });
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
                        lval = ::MIR::LValue::make_Field({ box$(lval), idx });
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
                        lval = ::MIR::LValue::make_Field({ box$(lval), idx });
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
                    lval = ::MIR::LValue::make_Downcast({ box$(lval), idx });
                    ),
                (Enum,
                    BUG(sp, "Destructuring an enum - " << *cur_ty);
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
                lval = ::MIR::LValue::make_Field({ box$(lval), idx });
                ),
            (Slice,
                cur_ty = &*e.inner;
                lval = ::MIR::LValue::make_Field({ box$(lval), idx });
                ),
            (Borrow,
                ASSERT_BUG(sp, idx == FIELD_DEREF, "Destructure of borrow doesn't correspond to a deref in the path");
                if( cur_ty == &tmp_ty ) {
                    tmp_ty = mv$(*tmp_ty.m_data.as_Borrow().inner);
                }
                else {
                    cur_ty = &*e.inner;
                }
                lval = ::MIR::LValue::make_Deref({ box$(lval) });
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
    unsigned int rule_idx = 0;
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

            rule_idx ++;
        }
        if( arm_code.has_condition )
        {
            builder.set_cur_block( arm_code.cond_end );
            builder.end_block( ::MIR::Terminator::make_If({ mv$(arm_code.cond_lval), arm_code.code, next_arm_bb }) );
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

        get_ty_and_val(sp, builder.resolve(), top_ty, top_val,  rule.field_path, field_path_ofs,  ity, val);
        DEBUG("ty = " << ity << ", val = " << val);

        const auto& ty = ity;
        TU_MATCHA( (ty.m_data), (te),
        (Infer,
            BUG(sp, "Hit _ in type - " << ty);
            ),
        (Diverge,
            BUG(sp, "Matching over !");
            ),
        (Primitive,
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

                    auto test_lval = builder.lvalue_or_temp(sp, te, ::MIR::Constant(re.as_Uint()));
                    auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::EQ, mv$(test_lval) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), succ_bb, fail_bb }) );
                    builder.set_cur_block(succ_bb);
                    ),
                (ValueRange,
                    TODO(sp, "Simple match over primitive - " << ty << " - ValueRange");
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

                    auto test_lval = builder.lvalue_or_temp(sp, te, ::MIR::Constant(re.as_Int()));
                    auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::EQ, mv$(test_lval) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), succ_bb, fail_bb }) );
                    builder.set_cur_block(succ_bb);
                    ),
                (ValueRange,
                    TODO(sp, "Simple match over primitive - " << ty << " - ValueRange");
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

                    auto test_lval = builder.lvalue_or_temp(sp, te, ::MIR::Constant(re.as_Uint()));
                    auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::EQ, mv$(test_lval) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), succ_bb, fail_bb }) );
                    builder.set_cur_block(succ_bb);
                    ),
                (ValueRange,
                    auto succ_bb = builder.new_bb_unlinked();
                    auto test_bb_2 = builder.new_bb_unlinked();

                    // IF `val` < `first` : fail_bb
                    auto test_lt_lval = builder.lvalue_or_temp(sp, te, ::MIR::Constant(re.first.as_Uint()));
                    auto cmp_lt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::LT, mv$(test_lt_lval) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lt_lval), fail_bb, test_bb_2 }) );

                    builder.set_cur_block(test_bb_2);

                    // IF `val` > `last` : fail_bb
                    auto test_gt_lval = builder.lvalue_or_temp(sp, te, ::MIR::Constant(re.last.as_Uint()));
                    auto cmp_gt_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ val.clone(), ::MIR::eBinOp::GT, mv$(test_gt_lval) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_gt_lval), fail_bb, succ_bb }) );

                    builder.set_cur_block(succ_bb);
                    )
                )
                break;
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64:
                TODO(sp, "Simple match over float - " << ty);
                break;
            case ::HIR::CoreType::Str: {
                ASSERT_BUG(sp, rule.is_Value() && rule.as_Value().is_StaticString(), "");
                const auto& v = rule.as_Value();

                auto succ_bb = builder.new_bb_unlinked();

                auto test_lval = builder.lvalue_or_temp(sp, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ty.clone()), ::MIR::RValue(::MIR::Constant( v.as_StaticString() )));
                auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ mv$(*val.as_Deref().val), ::MIR::eBinOp::EQ, mv$(test_lval) }));
                builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), succ_bb, fail_bb }) );
                builder.set_cur_block(succ_bb);
                } break;
            }
            ),
        (Path,
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
            (Enum,
                auto monomorph = [&](const auto& ty) { return monomorphise_type(sp, pbe->m_params, te.path.m_data.as_Generic().m_params, ty); };
                ASSERT_BUG(sp, rule.is_Variant(), "Rule for enum isn't Any or Variant");
                const auto& re = rule.as_Variant();
                unsigned int var_idx = re.idx;

                auto next_bb = builder.new_bb_unlinked();
                auto var_count = pbe->m_variants.size();

                // Generate a switch with only one option different.
                ::std::vector< ::MIR::BasicBlockId> arms(var_count, fail_bb);
                arms[var_idx] = next_bb;
                builder.end_block( ::MIR::Terminator::make_Switch({ val.clone(), mv$(arms) }) );

                builder.set_cur_block(next_bb);

                if( re.sub_rules.size() > 0 )
                {
                    const auto& var_data = pbe->m_variants.at(re.idx).second;
                    TU_MATCHA( (var_data), (ve),
                    (Unit,
                        // Nothing to recurse
                        ),
                    (Value,
                        // Nothing to recurse
                        ),
                    (Tuple,
                        // Create a dummy tuple to contain the inner types.
                        ::std::vector< ::HIR::TypeRef>  fake_ty_ents;
                        fake_ty_ents.reserve( ve.size() );
                        for(unsigned int i = 0; i < ve.size(); i ++)
                        {
                            fake_ty_ents.push_back( monomorph(ve[i].ent) );
                        }
                        ::HIR::TypeRef fake_tup = ::HIR::TypeRef( mv$(fake_ty_ents) );

                        // Recurse with the new ruleset
                        MIR_LowerHIR_Match_Simple__GeneratePattern(builder, sp,
                            re.sub_rules.data(), re.sub_rules.size(),
                            fake_tup, ::MIR::LValue::make_Downcast({ box$(val.clone()), var_idx }), rule.field_path.size(),
                            fail_bb
                            );
                        ),
                    (Struct,
                        // Create a dummy tuple to contain the inner types.
                        ::std::vector< ::HIR::TypeRef>  fake_ty_ents;
                        fake_ty_ents.reserve( ve.size() );
                        for(unsigned int i = 0; i < ve.size(); i ++)
                        {
                            fake_ty_ents.push_back( monomorph(ve[i].second.ent) );
                        }
                        ::HIR::TypeRef fake_tup = ::HIR::TypeRef( mv$(fake_ty_ents) );

                        // Recurse with the new ruleset
                        MIR_LowerHIR_Match_Simple__GeneratePattern(builder, sp,
                            re.sub_rules.data(), re.sub_rules.size(),
                            fake_tup, ::MIR::LValue::make_Downcast({ box$(val.clone()), var_idx }), rule.field_path.size(),
                            fail_bb
                            );
                        )
                    )
                }
                )   // TypePathBinding::Enum
            )
            ),  // Type::Data::Path
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
            TODO(sp, "Match directly on array?");
            #if 0
            unsigned int total = 0;
            for( unsigned int i = 0; i < te.size_val; i ++ ) {
                unsigned int cnt = MIR_LowerHIR_Match_Simple__GeneratePattern(
                    builder, sp,
                    rules, num_rules, *te.inner,
                    ::MIR::LValue::make_Field({ box$(match_val.clone()), i }),
                    fail_bb
                    );
                total += cnt;
                rules += cnt;
                num_rules -= cnt;
                if( num_rules == 0 )
                    return total;
            }
            #endif
            ),
        (Slice,
            ASSERT_BUG(sp, rule.is_Slice() || rule.is_SplitSlice() || (rule.is_Value() && rule.as_Value().is_Bytes()), "Can only match slice with Bytes or Slice rules - " << rule);
            if( rule.is_Value() ) {
                ASSERT_BUG(sp, *te.inner == ::HIR::CoreType::U8, "Bytes pattern on non-&[u8]");
                auto cloned_val = ::MIR::Constant( rule.as_Value().as_Bytes() );

                auto succ_bb = builder.new_bb_unlinked();

                auto inner_val = val.as_Deref().val->clone();

                auto test_lval = builder.lvalue_or_temp(sp, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ty.clone()), ::MIR::RValue(mv$(cloned_val)));
                auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ mv$(inner_val), ::MIR::eBinOp::EQ, mv$(test_lval) }));
                builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), succ_bb, fail_bb }) );
                builder.set_cur_block(succ_bb);
            }
            else if( rule.is_Slice() ) {
                const auto& re = rule.as_Slice();

                // Compare length
                auto test_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue( ::MIR::Constant::make_Uint(re.len) ));
                auto len_val = builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue::make_DstMeta({ builder.get_ptr_to_dst(sp, val).clone() }));
                auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ mv$(len_val), ::MIR::eBinOp::EQ, mv$(test_lval) }));

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
                auto test_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue( ::MIR::Constant::make_Uint(re.min_len) ));
                auto len_val = builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue::make_DstMeta({ builder.get_ptr_to_dst(sp, val).clone() }));
                auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ mv$(len_val), ::MIR::eBinOp::LT, mv$(test_lval) }));

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
            ),
        (Tuple,
            TODO(sp, "Match directly on tuple?");
            ),
        (Borrow,
            TODO(sp, "Match directly on borrow?");
            ),  // Type::Data::Borrow
        (Pointer,
            BUG(sp, "Attempting to match a pointer - " << rule << " against " << ty);
            ),
        (Function,
            BUG(sp, "Attempting to match a function pointer - " << rule << " against " << ty);
            ),
        (Closure,
            BUG(sp, "Attempting to match a closure");
            )
        )
    }
    return 0;
}

// --------------------------------------------------------------------
// Decision Tree
// --------------------------------------------------------------------

// ## Create descision tree in-memory based off the ruleset
// > Tree contains an lvalue and a set of possibilities (PatternRule) connected to another tree or to a branch index
struct DecisionTreeNode
{
    TAGGED_UNION( Branch, Unset,
        (Unset, struct{}),
        (Subtree, ::std::unique_ptr<DecisionTreeNode>),
        (Terminal, unsigned int)
        );

    template<typename T>
    struct Range
    {
        T   start;
        T   end;

        // `x` starts after this range ends
        bool operator<(const Range<T>& x) const {
            return (end < x.start);
        }
        // `x` falls above the end of this range
        bool operator<(const T& x) const {
            return (end < x);
        }

        // `x` ends before this starts, or overlaps
        bool operator>=(const Range<T>& x) const {
            return start > x.end || ovelaps(x);
        }
        // `x` is before or within this range
        bool operator>=(const T& x) const {
            return start > x || contains(x);
        }

        bool operator>(const Range<T>& x) const {
            return (start > x.end);
        }
        bool operator>(const T& x) const {
            return (start > x);
        }

        bool operator==(const Range<T>& x) const {
            return start == x.start && end == x.end;
        }
        bool operator!=(const Range<T>& x) const {
            return start != x.start || end != x.end;
        }

        bool contains(const T& x) const {
            return (start <= x && x <= end);
        }
        bool overlaps(const Range<T>& x) const {
            return (x.start <= start && start <= x.end) || (x.start <= end && end <= x.end);
        }

        friend ::std::ostream& operator<<(::std::ostream& os, const Range<T>& x) {
            if( x.start == x.end ) {
                return os << x.start;
            }
            else {
                return os << x.start << " ... " << x.end;
            }
        }
    };

    TAGGED_UNION( Values, Unset,
        (Unset, struct {}),
        (Bool, struct { Branch false_branch, true_branch; }),
        (Variant, ::std::vector< ::std::pair<unsigned int, Branch> >),
        (Unsigned, ::std::vector< ::std::pair< Range<uint64_t>, Branch> >),
        (Signed, ::std::vector< ::std::pair< Range<int64_t>, Branch> >),
        (Float, ::std::vector< ::std::pair< Range<double>, Branch> >),
        (String, ::std::vector< ::std::pair< ::std::string, Branch> >),
        (Slice, struct {
            ::std::vector< ::std::pair< unsigned int, Branch> > fixed_arms;
            //::std::vector< ::std::pair< unsigned int, Branch> > variable_arms;
            })
        );

    // TODO: Arm specialisation?
    field_path_t   m_field_path;
    Values  m_branches;
    Branch  m_default;

    DecisionTreeNode( field_path_t field_path ):
        // TODO: This is commented out fo a reason, but I don't know why.
        //m_field_path( mv$(field_path) ),
        m_branches(),
        m_default()
    {}

    static Branch clone(const Branch& b);
    static Values clone(const Values& x);
    DecisionTreeNode clone() const;

    void populate_tree_from_rule(const Span& sp, unsigned int arm_index, const PatternRule* first_rule, unsigned int rule_count) {
        populate_tree_from_rule(sp, first_rule, rule_count, [sp,arm_index](auto& branch){
            TU_MATCHA( (branch), (e),
            (Unset,
                // Good
                ),
            (Subtree,
                if( e->m_branches.is_Unset() && e->m_default.is_Unset() ) {
                    // Good.
                }
                else {
                    BUG(sp, "Duplicate terminal - branch="<<branch);
                }
                ),
            (Terminal,
                // TODO: This is ok if it's due to overlapping rules (e.g. ranges)
                //BUG(sp, "Duplicate terminal - Existing goes to arm " << e << ", new goes to arm " << arm_index );
                )
            )
            branch = Branch::make_Terminal(arm_index);
            });
    }
    // `and_then` - Closure called after processing the final rule
    void populate_tree_from_rule(const Span& sp, const PatternRule* first_rule, unsigned int rule_count, ::std::function<void(Branch&)> and_then);

    /// Simplifies the tree by eliminating nodes that don't make a decision
    void simplify();
    /// Propagate the m_default arm's contents to value arms, and vice-versa
    void propagate_default();
    /// HELPER: Unfies the rules from the provided branch with this node
    void unify_from(const Branch& b);

    ::MIR::LValue get_field(const ::MIR::LValue& base, unsigned int base_depth) const {
        ::MIR::LValue   cur = base.clone();
        for(unsigned int i = base_depth; i < m_field_path.size(); i ++ ) {
            const auto idx = m_field_path.data[i];
            if( idx == FIELD_DEREF ) {
                cur = ::MIR::LValue::make_Deref({ box$(cur) });
            }
            else {
                cur = ::MIR::LValue::make_Field({ box$(cur), idx });
            }
        }
        return cur;
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const Branch& x);
    friend ::std::ostream& operator<<(::std::ostream& os, const DecisionTreeNode& x);
};

struct DecisionTreeGen
{
    MirBuilder& m_builder;
    const ::std::vector< ::MIR::BasicBlockId>&  m_rule_blocks;

    DecisionTreeGen(MirBuilder& builder, const ::std::vector< ::MIR::BasicBlockId >& rule_blocks):
        m_builder( builder ),
        m_rule_blocks( rule_blocks )
    {}

    ::MIR::BasicBlockId get_block_for_rule(unsigned int rule_index) {
        return m_rule_blocks.at( rule_index );
    }

    void generate_tree_code(const Span& sp, const DecisionTreeNode& node, const ::HIR::TypeRef& ty, const ::MIR::LValue& val) {
        generate_tree_code(sp, node, ty, 0, val, [&](const auto& n){
            DEBUG("node = " << n);
            // - Recurse on this method
            this->generate_tree_code(sp, n, ty, val);
            });
    }
    void generate_tree_code(
        const Span& sp,
        const DecisionTreeNode& node,
        const ::HIR::TypeRef& ty, unsigned int path_ofs, const ::MIR::LValue& base_val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );

    void generate_branch(const DecisionTreeNode::Branch& branch, ::std::function<void(const DecisionTreeNode&)> cb);

    void generate_branches_Signed(
        const Span& sp,
        const DecisionTreeNode::Branch& default_branch,
        const DecisionTreeNode::Values::Data_Signed& branches,
        const ::HIR::TypeRef& ty, ::MIR::LValue val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
    void generate_branches_Unsigned(
        const Span& sp,
        const DecisionTreeNode::Branch& default_branch,
        const DecisionTreeNode::Values::Data_Unsigned& branches,
        const ::HIR::TypeRef& ty, ::MIR::LValue val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
    void generate_branches_Float(
        const Span& sp,
        const DecisionTreeNode::Branch& default_branch,
        const DecisionTreeNode::Values::Data_Float& branches,
        const ::HIR::TypeRef& ty, ::MIR::LValue val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
    void generate_branches_Char(
        const Span& sp,
        const DecisionTreeNode::Branch& default_branch,
        const DecisionTreeNode::Values::Data_Unsigned& branches,
        const ::HIR::TypeRef& ty, ::MIR::LValue val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
    void generate_branches_Bool(
        const Span& sp,
        const DecisionTreeNode::Branch& default_branch,
        const DecisionTreeNode::Values::Data_Bool& branches,
        const ::HIR::TypeRef& ty, ::MIR::LValue val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
    void generate_branches_Borrow_str(
        const Span& sp,
        const DecisionTreeNode::Branch& default_branch,
        const DecisionTreeNode::Values::Data_String& branches,
        const ::HIR::TypeRef& ty, ::MIR::LValue val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );

    void generate_branches_Enum(
        const Span& sp,
        const DecisionTreeNode::Branch& default_branch,
        const DecisionTreeNode::Values::Data_Variant& branches,
        const field_path_t& field_path,   // used to know when to stop handling sub-nodes
        const ::HIR::TypeRef& ty, ::MIR::LValue val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
    void generate_branches_Slice(
        const Span& sp,
        const DecisionTreeNode::Branch& default_branch,
        const DecisionTreeNode::Values::Data_Slice& branches,
        const field_path_t& field_path,
        const ::HIR::TypeRef& ty, ::MIR::LValue val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
    void generate_tree_code__enum(
        const Span& sp,
        const DecisionTreeNode& node, const ::HIR::TypeRef& fake_ty, const ::MIR::LValue& val,
        const field_path_t& path_prefix,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
};

void MIR_LowerHIR_Match_DecisionTree( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arms_code, ::MIR::BasicBlockId first_cmp_block )
{
    TRACE_FUNCTION;

    // XXX XXX XXX: The current codegen (below) will generate incorrect code if ordering matters.
    // ```
    // match ("foo", "bar")
    // {
    // (_, "bar") => {},    // Expected
    // ("foo", _) => {},    // Actual
    // _ => {},
    // }
    // ```

    // TODO: Sort the columns in `arm_rules` to ensure that the most specific rule is parsed first.
    // - Ordering within a pattern doesn't matter, only the order of arms matters.
    // - This sort could be designed such that the above case would match correctly?

    DEBUG("- Generating rule bindings");
    ::std::vector< ::MIR::BasicBlockId> rule_blocks;
    for(const auto& rule : arm_rules)
    {
        const auto& arm_code = arms_code[rule.arm_idx];
        ASSERT_BUG(node.span(), !arm_code.has_condition, "Decision tree doesn't (yet) support conditionals");

        assert( rule.pat_idx < arm_code.destructures.size() );
        // Set the target for when a rule succeeds to the destructuring code for this rule
        rule_blocks.push_back( arm_code.destructures[rule.pat_idx] );
        // - Tie the end of that block to the code block for this arm
        builder.set_cur_block( rule_blocks.back() );
        builder.end_block( ::MIR::Terminator::make_Goto(arm_code.code) );
    }


    // - Build tree by running each arm's pattern across it
    DEBUG("- Building decision tree");
    DecisionTreeNode    root_node({});
    unsigned int rule_idx = 0;
    for( const auto& arm_rule : arm_rules )
    {
        auto arm_idx = arm_rule.arm_idx;
        DEBUG("(" << arm_idx << ", " << arm_rule.pat_idx << "): " << arm_rule.m_rules);
        root_node.populate_tree_from_rule( node.m_arms[arm_idx].m_code->span(), rule_idx, arm_rule.m_rules.data(), arm_rule.m_rules.size() );
        rule_idx += 1;
    }
    DEBUG("root_node = " << root_node);
    root_node.simplify();
    DEBUG("root_node = " << root_node);
    root_node.propagate_default();
    DEBUG("root_node = " << root_node);
    // TODO: Pretty print `root_node`

    // - Convert the above decision tree into MIR
    DEBUG("- Emitting decision tree");
    DecisionTreeGen gen { builder, rule_blocks };
    builder.set_cur_block( first_cmp_block );
    gen.generate_tree_code( node.span(), root_node, node.m_value->m_res_type, mv$(match_val) );
    ASSERT_BUG(node.span(), !builder.block_active(), "Decision tree didn't terminate the final block");
}

#if 0
DecisionTreeNode MIR_LowerHIR_Match_DecisionTree__MakeTree(const Span& sp, t_arm_rules& arm_rules)
{
    ::std::vector<unsigned int> indexes;
    ::std::vector< slice<PatternRule> > rules;
    for(unsigned i = 0; i < arm_rules.size(); i ++)
    {
        rules.push_back( arm_rules[i].m_rules );
        indexes.push_back(i);
    }

    return MIR_LowerHIR_Match_DecisionTree__MakeTree_Node(sp, indexes, rules);
}
DecisionTreeNode MIR_LowerHIR_Match_DecisionTree__MakeTree_Node(const Span& sp, slice<unsigned int> arm_indexes, slice< slice<PaternRule>> arm_rules)
{
    assert( arm_indexes.size() == arm_rules.size() );
    assert( arm_rules.size() > 1 );
    assert( arm_rules[0].size() > 0 );

    // 1. Sort list (should it already be sorted?)
    for(const auto& rules : arm_rules)
    {
        ASSERT_BUG(sp, rules.size() != arm_rules[0].size(), "");
    }

    // 2. Detect all arms being `_` and move on to the next condition
    while( ::std::all_of(arm_rules.begin(), arm_rules.end(), [](const auto& r){ return r.m_rules[0].is_Any(); }) )
    {
        // Delete first rule from all and continue.
        if( arm_rules[0].size() == 1 ) {
            // No rules left?
            BUG(sp, "Duplicate match arms");
        }

        for(auto& rules : arm_rules)
        {
            rules = rules.subslice_from(1);
        }
    }

    // We have a codition.
    for(const auto& rules : arm_rules)
    {
        ASSERT_BUG(sp, rules[0].is_Any() || rules[0].tag() == arm_rules[0][0].tag(), "Mismatched rules in match");
    }

    bool has_any = arm_rules.back()[0].is_Any();

    // All rules must either be _ or the same type, and can't all be _
    switch( arm_rules[0][0].tag() )
    {
    case PatternRule::TAGDEAD:  throw "";
    case PatternRule::TAG_Any:  throw "";

    case PatternRule::TAG_Variant:
        break;
    // TODO: Value and ValueRange can appear together.
    // - They also overlap in non-trivial ways.
    }
}
#endif

// ----------------------------
// DecisionTreeNode
// ----------------------------
DecisionTreeNode::Branch DecisionTreeNode::clone(const DecisionTreeNode::Branch& b) {
    TU_MATCHA( (b), (e),
    (Unset, return Branch(e); ),
    (Subtree, return Branch(box$( e->clone() )); ),
    (Terminal, return Branch(e); )
    )
    throw "";
}
DecisionTreeNode::Values DecisionTreeNode::clone(const DecisionTreeNode::Values& x) {
    TU_MATCHA( (x), (e),
    (Unset, return Values(e); ),
    (Bool,
        return Values::make_Bool({ clone(e.false_branch),  clone(e.true_branch) });
        ),
    (Variant,
        Values::Data_Variant rv;
        rv.reserve(e.size());
        for(const auto& v : e)
            rv.push_back( ::std::make_pair(v.first, clone(v.second)) );
        return Values( mv$(rv) );
        ),
    (Unsigned,
        Values::Data_Unsigned rv;
        rv.reserve(e.size());
        for(const auto& v : e)
            rv.push_back( ::std::make_pair(v.first, clone(v.second)) );
        return Values( mv$(rv) );
        ),
    (Signed,
        Values::Data_Signed rv;
        rv.reserve(e.size());
        for(const auto& v : e)
            rv.push_back( ::std::make_pair(v.first, clone(v.second)) );
        return Values( mv$(rv) );
        ),
    (Float,
        Values::Data_Float rv;
        rv.reserve(e.size());
        for(const auto& v : e)
            rv.push_back( ::std::make_pair(v.first, clone(v.second)) );
        return Values( mv$(rv) );
        ),
    (String,
        Values::Data_String rv;
        rv.reserve(e.size());
        for(const auto& v : e)
            rv.push_back( ::std::make_pair(v.first, clone(v.second)) );
        return Values( mv$(rv) );
        ),
    (Slice,
        Values::Data_Slice rv;
        rv.fixed_arms.reserve(e.fixed_arms.size());
        for(const auto& v : e.fixed_arms)
            rv.fixed_arms.push_back( ::std::make_pair(v.first, clone(v.second)) );
        return Values( mv$(rv) );
        )
    )
    throw "";
}
DecisionTreeNode DecisionTreeNode::clone() const {
    DecisionTreeNode    rv(m_field_path);
    rv.m_field_path = m_field_path;
    rv.m_branches = clone(m_branches);
    rv.m_default = clone(m_default);
    return rv;
}

// Helpers for `populate_tree_from_rule`
namespace
{
    DecisionTreeNode::Branch new_branch_subtree(field_path_t path)
    {
        return DecisionTreeNode::Branch( box$(DecisionTreeNode( mv$(path) )) );
    }

    // Common code for numerics (Int, Uint, and Float)
    template<typename T>
    static void from_rule_valuerange(
        const Span& sp,
        ::std::vector< ::std::pair< DecisionTreeNode::Range<T>, DecisionTreeNode::Branch> >& be, T ve_start, T ve_end,
        const char* name, const field_path_t& field_path, ::std::function<void(DecisionTreeNode::Branch&)> and_then
        )
    {
        ASSERT_BUG(sp, ve_start <= ve_end, "Range pattern with a start after the end - " << ve_start << "..." << ve_end);

        TRACE_FUNCTION_F("[" << FMT_CB(os, for(const auto& i:be) os << i.first <<" , ";) << "]");
        // - Find the first entry that ends after the new one starts.
        auto it = ::std::find_if(be.begin(), be.end(), [&](const auto& v){ return v.first.end >= ve_start; });
        while(ve_start < ve_end)
        {
            if( it == be.end() ) {
            DEBUG("new = (" << ve_start << "..." << ve_end << "), exist=END");
                it = be.insert( it, ::std::make_pair( DecisionTreeNode::Range<T> { ve_start,ve_end }, new_branch_subtree(field_path) ) );
                and_then(it->second);
                return ;
            }
            DEBUG("new = (" << ve_start << "..." << ve_end << "), exist=" << it->first);
            // If the located entry starts after the end of this range
            if( it->first.start >= ve_end ) {
                DEBUG("- New free");
                it = be.insert( it, ::std::make_pair( DecisionTreeNode::Range<T> { ve_start,ve_end }, new_branch_subtree(field_path) ) );
                and_then(it->second);
                return ;
            }
            // If this range is equal to the existing, just recurse into it
            else if( it->first.start == ve_start && it->first.end == ve_end ) {
                DEBUG("- Equal");
                and_then(it->second);
                return ;
            }
            // If the new range starts before the start of this range, add a new entry before the existing one
            else if( it->first.start > ve_start ) {
                DEBUG("- New head, continue");
                it = be.insert( it, ::std::make_pair( DecisionTreeNode::Range<T> { ve_start,it->first.start-1 }, new_branch_subtree(field_path) ) );
                and_then(it->second);
                ++ it;
                ve_start = it->first.start;
            }
            // If the new range ends before the end of this range, split the existing range and recurse into the first
            else if( it->first.end > ve_end ) {
                DEBUG("- Inner");
                assert(ve_start == it->first.start);
                it = be.insert( it, ::std::make_pair( DecisionTreeNode::Range<T> { ve_start, ve_end }, DecisionTreeNode::clone(it->second) ) );
                and_then(it->second);
                (it+1)->first.start = ve_end+1;
                return ;
            }
            // (else) if the new range ends after the end of this range, apply to the rest of this range and advance
            else {
                DEBUG("- Shared head, continue");
                //assert(it->first.start == ve_start);
                assert((it->first.end) < ve_end);

                if( it->first.start != it->first.end )
                    and_then(it->second);
                ve_start = it->first.end + 1;
                ++ it;
            }
        }
    }

    template<typename T>
    static void from_rule_value(
        const Span& sp,
        ::std::vector< ::std::pair< DecisionTreeNode::Range<T>, DecisionTreeNode::Branch> >& be, T ve,
        const char* name, const field_path_t& field_path, ::std::function<void(DecisionTreeNode::Branch&)> and_then
        )
    {
        auto it = ::std::find_if(be.begin(), be.end(), [&](const auto& v){ return v.first.end >= ve; });
        if( it == be.end() || it->first.start > ve ) {
            it = be.insert( it, ::std::make_pair( DecisionTreeNode::Range<T> { ve,ve }, new_branch_subtree(field_path) ) );
        }
        else if( it->first.start == ve && it->first.end == ve ) {
            // Equal, continue and add sub-pat
        }
        else {
            // Collide or overlap!
            TODO(sp, "Value patterns - " << name << " - Overlapping - " << it->first.start << " <= " << ve << " <= " << it->first.end);
        }
        and_then( it->second );
    }
}
void DecisionTreeNode::populate_tree_from_rule(const Span& sp, const PatternRule* first_rule, unsigned int rule_count, ::std::function<void(Branch&)> and_then)
{
    assert( rule_count > 0 );
    const auto& rule = *first_rule;

    if( m_field_path.size() == 0 ) {
        m_field_path = rule.field_path;
    }
    else {
        ASSERT_BUG(sp, m_field_path == rule.field_path, "Patterns with mismatched field paths - " << m_field_path << " != " << rule.field_path);
    }

    #define GET_BRANCHES(fld, var)  (({if( fld.is_Unset() ) {\
            fld = Values::make_##var({}); \
        } \
        else if( !fld.is_##var() ) { \
            BUG(sp, "Mismatched rules - have " #var ", but have seen " << fld.tag_str()); \
        }}), \
        fld.as_##var())


    TU_MATCHA( (rule), (e),
    (Any, {
        if( rule_count == 1 )
        {
            ASSERT_BUG(sp, !m_default.is_Terminal(), "Duplicate terminal rule");
            and_then(m_default);
        }
        else
        {
            if( m_default.is_Unset() ) {
                m_default = new_branch_subtree(rule.field_path);
                m_default.as_Subtree()->populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
            }
            else TU_IFLET( Branch, m_default, Subtree, be,
                be->populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
            )
            else {
                // NOTE: All lists processed as part of the same tree should be the same length
                BUG(sp, "Duplicate terminal rule");
            }
        }
        // TODO: Should this also recurse into branches?
        }),
    (Variant, {
        auto& be = GET_BRANCHES(m_branches, Variant);

        auto it = ::std::find_if( be.begin(), be.end(), [&](const auto& x){ return x.first >= e.idx; });
        // If this variant isn't yet processed, add a new subtree for it
        if( it == be.end() || it->first != e.idx ) {
            it = be.insert(it, ::std::make_pair(e.idx, new_branch_subtree(rule.field_path)));
            assert( it->second.is_Subtree() );
        }
        else {
            if( it->second.is_Terminal() ) {
                BUG(sp, "Duplicate terminal rule - " << it->second.as_Terminal());
            }
            assert( !it->second.is_Unset() );
            assert( it->second.is_Subtree() );
        }
        auto& subtree = *it->second.as_Subtree();

        if( e.sub_rules.size() > 0 && rule_count > 1 )
        {
            subtree.populate_tree_from_rule(sp, e.sub_rules.data(), e.sub_rules.size(), [&](auto& branch){
                TU_MATCH_DEF(Branch, (branch), (be),
                (
                    BUG(sp, "Duplicate terminator");
                    ),
                (Unset,
                    branch = new_branch_subtree(rule.field_path);
                    ),
                (Subtree,
                    )
                )
                branch.as_Subtree()->populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
                });
        }
        else if( e.sub_rules.size() > 0)
        {
            subtree.populate_tree_from_rule(sp, e.sub_rules.data(), e.sub_rules.size(), and_then);
        }
        else if( rule_count > 1 )
        {
            subtree.populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
        }
        else
        {
            and_then(it->second);
        }
        }),
    (Slice,
        auto& be = GET_BRANCHES(m_branches, Slice);

        auto it = ::std::find_if( be.fixed_arms.begin(), be.fixed_arms.end(), [&](const auto& x){ return x.first >= e.len; } );
        if( it == be.fixed_arms.end() || it->first != e.len ) {
            it = be.fixed_arms.insert(it, ::std::make_pair(e.len, new_branch_subtree(rule.field_path)));
        }
        else {
            if( it->second.is_Terminal() ) {
                BUG(sp, "Duplicate terminal rule - " << it->second.as_Terminal());
            }
            assert( !it->second.is_Unset() );
        }
        assert( it->second.is_Subtree() );
        auto& subtree = *it->second.as_Subtree();

        if( e.sub_rules.size() > 0 && rule_count > 1 )
        {
            subtree.populate_tree_from_rule(sp, e.sub_rules.data(), e.sub_rules.size(), [&](auto& branch){
                TU_MATCH_DEF(Branch, (branch), (be),
                (
                    BUG(sp, "Duplicate terminator");
                    ),
                (Unset,
                    branch = new_branch_subtree(rule.field_path);
                    ),
                (Subtree,
                    )
                )
                branch.as_Subtree()->populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
                });
        }
        else if( e.sub_rules.size() > 0)
        {
            subtree.populate_tree_from_rule(sp, e.sub_rules.data(), e.sub_rules.size(), and_then);
        }
        else if( rule_count > 1 )
        {
            subtree.populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
        }
        else
        {
            and_then(it->second);
        }
        ),
    (SplitSlice,
        //auto& be = GET_BRANCHES(m_branches, Slice);
        TODO(sp, "SplitSlice in DTN - " << rule);
        ),
    (Bool,
        auto& be = GET_BRANCHES(m_branches, Bool);

        auto& branch = (e ? be.true_branch : be.false_branch);
        if( branch.is_Unset() ) {
            branch = new_branch_subtree( rule.field_path );
        }
        else if( branch.is_Terminal() ) {
            BUG(sp, "Duplicate terminal rule - " << branch.as_Terminal());
        }
        else {
            // Good.
        }
        if( rule_count > 1 )
        {
            auto& subtree = *branch.as_Subtree();
            subtree.populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
        }
        else
        {
            and_then(branch);
        }
        ),
    (Value,
        TU_MATCHA( (e), (ve),
        (Int,
            auto& be = GET_BRANCHES(m_branches, Signed);

            // TODO: De-duplicate this code between Uint and Float
            from_rule_value(sp, be, ve, "Signed", rule.field_path,
                [&](auto& branch) {
                    if( rule_count > 1 ) {
                        assert( branch.as_Subtree() );
                        auto& subtree = *branch.as_Subtree();
                        subtree.populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
                    }
                    else
                    {
                        and_then(branch);
                    }
                });
            ),
        (Uint,
            auto& be = GET_BRANCHES(m_branches, Unsigned);

            from_rule_value(sp, be, ve, "Unsigned", rule.field_path,
                [&](auto& branch) {
                    if( rule_count > 1 ) {
                        assert( branch.as_Subtree() );
                        auto& subtree = *branch.as_Subtree();
                        subtree.populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
                    }
                    else
                    {
                        and_then(branch);
                    }
                });
            ),
        (Float,
            auto& be = GET_BRANCHES(m_branches, Float);

            from_rule_value(sp, be, ve, "Float", rule.field_path,
                [&](auto& branch) {
                    if( rule_count > 1 ) {
                        assert( branch.as_Subtree() );
                        auto& subtree = *branch.as_Subtree();
                        subtree.populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
                    }
                    else {
                        and_then(branch);
                    }
                });
            ),
        (Bool,
            throw "";
            ),
        (Bytes,
            TODO(sp, "Value patterns - Bytes");
            ),
        (StaticString,
            auto& be = GET_BRANCHES(m_branches, String);

            auto it = ::std::find_if(be.begin(), be.end(), [&](const auto& v){ return v.first >= ve; });
            if( it == be.end() || it->first != ve ) {
                it = be.insert( it, ::std::make_pair(ve, new_branch_subtree(rule.field_path) ) );
            }
            auto& branch = it->second;
            if( rule_count > 1 )
            {
                assert( branch.as_Subtree() );
                auto& subtree = *branch.as_Subtree();
                subtree.populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
            }
            else
            {
                and_then(branch);
            }
            ),
        (Const,
            throw "";
            ),
        (ItemAddr,
            throw "";
            )
        )
        ),
    (ValueRange,

        ASSERT_BUG(sp, e.first.tag() == e.last.tag(), "");
        TU_MATCHA( (e.first, e.last), (ve_start, ve_end),
        (Int,
            auto& be = GET_BRANCHES(m_branches, Signed);
            from_rule_valuerange(sp, be, ve_start, ve_end, "Signed", rule.field_path,
                [&](auto& branch) {
                    if( rule_count > 1 )
                    {
                        assert( branch.as_Subtree() );
                        auto& subtree = *branch.as_Subtree();
                        subtree.populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
                    }
                    else
                    {
                        and_then(branch);
                    }
                });
            ),
        (Uint,
            // TODO: Share code between the three numeric groups
            auto& be = GET_BRANCHES(m_branches, Unsigned);
            from_rule_valuerange(sp, be, ve_start, ve_end, "Unsigned", rule.field_path,
                [&](auto& branch) {
                    if( rule_count > 1 )
                    {
                        assert( branch.as_Subtree() );
                        auto& subtree = *branch.as_Subtree();
                        subtree.populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
                    }
                    else
                    {
                        and_then(branch);
                    }
                });
            ),
        (Float,
            auto& be = GET_BRANCHES(m_branches, Float);
            from_rule_valuerange(sp, be, ve_start, ve_end, "Float", rule.field_path,
                [&](auto& branch) {
                    if( rule_count > 1 )
                    {
                        assert( branch.as_Subtree() );
                        auto& subtree = *branch.as_Subtree();
                        subtree.populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
                    }
                    else
                    {
                        and_then(branch);
                    }
                });
            ),
        (Bool,
            throw "";
            ),
        (Bytes,
            TODO(sp, "ValueRange patterns - Bytes");
            ),
        (StaticString,
            ERROR(sp, E0000, "Use of string in value range patter");
            ),
        (Const,
            throw "";
            ),
        (ItemAddr,
            throw "";
            )
        )
        )
    )
}

void DecisionTreeNode::simplify()
{
    struct H {
        static void simplify_branch(Branch& b)
        {
            TU_IFLET(Branch, b, Subtree, be,
                be->simplify();
                if( be->m_branches.is_Unset() ) {
                    auto v = mv$( be->m_default );
                    b = mv$(v);
                }
            )
        }
    };

    TU_MATCHA( (m_branches), (e),
    (Unset,
        H::simplify_branch(m_default);
        // Replace `this` with `m_default` if `m_default` is a subtree
        // - Fixes the edge case for the top of the tree
        if( m_default.is_Subtree() )
        {
            *this = mv$(*m_default.as_Subtree());
        }
        return ;
        ),
    (Bool,
        H::simplify_branch(e.false_branch);
        H::simplify_branch(e.true_branch);
        ),
    (Variant,
        for(auto& branch : e) {
            H::simplify_branch(branch.second);
        }
        ),
    (Unsigned,
        for(auto& branch : e) {
            H::simplify_branch(branch.second);
        }
        ),
    (Signed,
        for(auto& branch : e) {
            H::simplify_branch(branch.second);
        }
        ),
    (Float,
        for(auto& branch : e) {
            H::simplify_branch(branch.second);
        }
        ),
    (String,
        for(auto& branch : e) {
            H::simplify_branch(branch.second);
        }
        ),
    (Slice,
        for(auto& branch : e.fixed_arms) {
            H::simplify_branch(branch.second);
        }
        )
    )

    H::simplify_branch(m_default);
}

void DecisionTreeNode::propagate_default()
{
    TRACE_FUNCTION_FR(*this, *this);
    struct H {
        static void handle_branch(Branch& b, const Branch& def) {
            TU_IFLET(Branch, b, Subtree, be,
                be->propagate_default();
                if( !def.is_Unset() )
                {
                    DEBUG("Unify " << *be << " with " << def);
                    be->unify_from(def);
                    be->propagate_default();
                }
            )
        }
    };

    TU_MATCHA( (m_branches), (e),
    (Unset,
        ),
    (Bool,
        DEBUG("- false");
        H::handle_branch(e.false_branch, m_default);
        DEBUG("- true");
        H::handle_branch(e.true_branch, m_default);
        ),
    (Variant,
        for(auto& branch : e) {
            DEBUG("- V " << branch.first);
            H::handle_branch(branch.second, m_default);
        }
        ),
    (Unsigned,
        for(auto& branch : e) {
            DEBUG("- U " << branch.first);
            H::handle_branch(branch.second, m_default);
        }
        ),
    (Signed,
        for(auto& branch : e) {
            DEBUG("- S " << branch.first);
            H::handle_branch(branch.second, m_default);
        }
        ),
    (Float,
        for(auto& branch : e) {
            DEBUG("- " << branch.first);
            H::handle_branch(branch.second, m_default);
        }
        ),
    (String,
        for(auto& branch : e) {
            DEBUG("- '" << branch.first << "'");
            H::handle_branch(branch.second, m_default);
        }
        ),
    (Slice,
        for(auto& branch : e.fixed_arms) {
            DEBUG("- [_;" << branch.first << "]");
            H::handle_branch(branch.second, m_default);
        }
        )
    )
    DEBUG("- default");
    TU_IFLET(Branch, m_default, Subtree, be,
        be->propagate_default();

        if( be->m_default.is_Unset() ) {
            // Propagate default from value branches
            TU_MATCHA( (m_branches), (e),
            (Unset,
                ),
            (Bool,
                be->unify_from(e.false_branch);
                be->unify_from(e.true_branch);
                ),
            (Variant,
                for(auto& branch : e) {
                    be->unify_from(branch.second);
                }
                ),
            (Unsigned,
                for(auto& branch : e) {
                    be->unify_from(branch.second);
                }
                ),
            (Signed,
                for(auto& branch : e) {
                    be->unify_from(branch.second);
                }
                ),
            (Float,
                for(auto& branch : e) {
                    be->unify_from(branch.second);
                }
                ),
            (String,
                for(auto& branch : e) {
                    be->unify_from(branch.second);
                }
                ),
            (Slice,
                for(auto& branch : e.fixed_arms) {
                    be->unify_from(branch.second);
                }
                )
            )
        }
    )
}

namespace {
    static void unify_branch(DecisionTreeNode::Branch& dst, const DecisionTreeNode::Branch& src) {
        if( dst.is_Unset() ) {
            dst = DecisionTreeNode::clone(src);
        }
        else if( dst.is_Subtree() ) {
            dst.as_Subtree()->unify_from(src);
        }
        else {
            // Terminal, no unify
        }
    }

    template<typename T>
    void unify_from_vals_range(::std::vector< ::std::pair<T, DecisionTreeNode::Branch>>& dst, const ::std::vector< ::std::pair<T, DecisionTreeNode::Branch>>& src)
    {
        for(const auto& srcv : src)
        {
            // Find the first entry with an end greater than or equal to the start of this entry
            auto it = ::std::find_if( dst.begin(), dst.end(), [&](const auto& x){ return x.first.end >= srcv.first.start; });
            // Not found? Insert a new branch
            if( it == dst.end() ) {
                it = dst.insert(it, ::std::make_pair(srcv.first, DecisionTreeNode::clone(srcv.second)));
            }
            // If the found entry doesn't overlap (the start of `*it` is after the end of `srcv`)
            else if( it->first.start > srcv.first.end ) {
                it = dst.insert(it, ::std::make_pair(srcv.first, DecisionTreeNode::clone(srcv.second)));
            }
            else if( it->first == srcv.first ) {
                unify_branch( it->second, srcv.second );
            }
            else {
                // NOTE: Overlapping doesn't get handled here
            }
        }
    }

    template<typename T>
    void unify_from_vals_pt(::std::vector< ::std::pair<T, DecisionTreeNode::Branch>>& dst, const ::std::vector< ::std::pair<T, DecisionTreeNode::Branch>>& src)
    {
        // Insert items not already present, merge present items
        for(const auto& srcv : src)
        {
            auto it = ::std::find_if( dst.begin(), dst.end(), [&](const auto& x){ return x.first >= srcv.first; });
            // Not found? Insert a new branch
            if( it == dst.end() || it->first != srcv.first ) {
                it = dst.insert(it, ::std::make_pair(srcv.first, DecisionTreeNode::clone(srcv.second)));
            }
            else {
                unify_branch( it->second, srcv.second );
            }
        }
    }
}

void DecisionTreeNode::unify_from(const Branch& b)
{
    TRACE_FUNCTION_FR(*this << " with " << b, *this);

    assert( b.is_Terminal() || b.is_Subtree() );

    if( m_default.is_Unset() ) {
        if( b.is_Terminal() ) {
            m_default = clone(b);
        }
        else {
            m_default = clone(b.as_Subtree()->m_default);
        }
    }

    if( b.is_Subtree() && b.as_Subtree()->m_branches.tag() != m_branches.tag() ) {
        // Is this a bug, or expected (and don't unify in?)
        DEBUG("TODO - Unify mismatched arms? - " << b.as_Subtree()->m_branches.tag_str() << " and " << m_branches.tag_str());
        return ;
    }
    bool should_unify_subtree = b.is_Subtree() && this->m_field_path == b.as_Subtree()->m_field_path;
    //if( b.is_Subtree() ) {
    //    ASSERT_BUG(Span(), this->m_field_path == b.as_Subtree()->m_field_path, "Unifiying DTNs with mismatched paths - " << this->m_field_path << " != " << b.as_Subtree()->m_field_path);
    //}

    TU_MATCHA( (m_branches), (dst),
    (Unset,
        if( b.is_Subtree() ) {
            assert( b.as_Subtree()->m_branches.is_Unset() );
        }
        else {
            // Huh? Terminal matching against an unset branch?
        }
        ),
    (Bool,
        auto* src = (b.is_Subtree() ? &b.as_Subtree()->m_branches.as_Bool() : nullptr);

        unify_branch( dst.false_branch, (src ? src->false_branch : b) );
        unify_branch( dst.true_branch , (src ? src->true_branch : b) );
        ),
    (Variant,
        if( should_unify_subtree ) {
            auto& sb = b.as_Subtree()->m_branches;
            ASSERT_BUG(Span(), sb.is_Variant(), "Unifying Variant with " << sb.tag_str());
            unify_from_vals_pt(dst, sb.as_Variant());
        }
        else {
            // Unify all with terminal branch
            for(auto& dstv : dst)
            {
                unify_branch(dstv.second, b);
            }
        }
        ),
    (Unsigned,
        if( should_unify_subtree ) {
            auto& sb = b.as_Subtree()->m_branches;
            ASSERT_BUG(Span(), sb.is_Unsigned(), "Unifying Unsigned with " << sb.tag_str());
            unify_from_vals_range(dst, sb.as_Unsigned());
        }
        else {
            for(auto& dstv : dst)
            {
                unify_branch(dstv.second, b);
            }
        }
        ),
    (Signed,
        if( should_unify_subtree ) {
            auto& sb = b.as_Subtree()->m_branches;
            ASSERT_BUG(Span(), sb.is_Signed(), "Unifying Signed with " << sb.tag_str());
            unify_from_vals_range(dst, sb.as_Signed());
        }
        else {
            for(auto& dstv : dst)
            {
                unify_branch(dstv.second, b);
            }
        }
        ),
    (Float,
        if( should_unify_subtree ) {
            auto& sb = b.as_Subtree()->m_branches;
            ASSERT_BUG(Span(), sb.is_Float(), "Unifying Float with " << sb.tag_str());
            unify_from_vals_range(dst, sb.as_Float());
        }
        else {
            for(auto& dstv : dst) {
                unify_branch(dstv.second, b);
            }
        }
    ),
    (String,
        if( should_unify_subtree ) {
            auto& sb = b.as_Subtree()->m_branches;
            ASSERT_BUG(Span(), sb.is_String(), "Unifying String with " << sb.tag_str());
            unify_from_vals_pt(dst, sb.as_String());
        }
        else {
            for(auto& dstv : dst) {
                unify_branch( dstv.second, b );
            }
        }
        ),
    (Slice,
        if( should_unify_subtree ) {
            auto& sb = b.as_Subtree()->m_branches;
            ASSERT_BUG(Span(), sb.is_Slice(), "Unifying Slice with " << sb.tag_str());

            const auto& src = sb.as_Slice();
            unify_from_vals_pt(dst.fixed_arms, src.fixed_arms);
        }
        else {
            for(auto& dstv : dst.fixed_arms) {
                unify_branch( dstv.second, b );
            }
        }
        )
    )
}

::std::ostream& operator<<(::std::ostream& os, const DecisionTreeNode::Branch& x) {
    TU_MATCHA( (x), (be),
    (Unset,
        os << "!";
        ),
    (Terminal,
        os << "ARM " << be;
        ),
    (Subtree,
        os << *be;
        )
    )
    return os;
}
::std::ostream& operator<<(::std::ostream& os, const DecisionTreeNode& x) {
    os << "DTN [" << x.m_field_path << "] { ";
    TU_MATCHA( (x.m_branches), (e),
    (Unset,
        os << "!, ";
        ),
    (Bool,
        os << "false = " << e.false_branch << ", true = " << e.true_branch << ", ";
        ),
    (Variant,
        os << "V ";
        for(const auto& branch : e) {
            os << branch.first << " = " << branch.second << ", ";
        }
        ),
    (Unsigned,
        os << "U ";
        for(const auto& branch : e) {
            const auto& range = branch.first;
            if( range.start == range.end ) {
                os << range.start;
            }
            else {
                os << range.start << "..." << range.end;
            }
            os << " = " << branch.second << ", ";
        }
        ),
    (Signed,
        os << "S ";
        for(const auto& branch : e) {
            const auto& range = branch.first;
            if( range.start == range.end ) {
                os << range.start;
            }
            else {
                os << range.start << "..." << range.end;
            }
            os << " = " << branch.second << ", ";
        }
        ),
    (Float,
        os << "F ";
        for(const auto& branch : e) {
            const auto& range = branch.first;
            if( range.start == range.end ) {
                os << range.start;
            }
            else {
                os << range.start << "..." << range.end;
            }
            os << " = " << branch.second << ", ";
        }
        ),
    (String,
        for(const auto& branch : e) {
            os << "\"" << branch.first << "\"" << " = " << branch.second << ", ";
        }
        ),
    (Slice,
        os << "len ";
        for(const auto& branch : e.fixed_arms) {
            os << "=" << branch.first << " = " << branch.second << ", ";
        }
        )
    )

    os << "* = " << x.m_default;
    os << " }";
    return os;
}


// ----------------------------
// DecisionTreeGen
// ----------------------------

void DecisionTreeGen::generate_tree_code(
    const Span& sp,
    const DecisionTreeNode& node,
    const ::HIR::TypeRef& top_ty, unsigned int field_path_ofs, const ::MIR::LValue& top_val,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    TRACE_FUNCTION_F("top_ty=" << top_ty << ", field_path_ofs=" << field_path_ofs << ", top_val=" << top_val << ", node=" << node);

    ::MIR::LValue   val;
    ::HIR::TypeRef  ty;

    get_ty_and_val(sp, m_builder.resolve(), top_ty, top_val,  node.m_field_path, field_path_ofs,  ty, val);
    DEBUG("ty = " << ty << ", val = " << val);

    TU_MATCHA( (ty.m_data), (e),
    (Infer,   BUG(sp, "Ivar for in match type"); ),
    (Diverge, BUG(sp, "Diverge in match type");  ),
    (Primitive,
        switch(e)
        {
        case ::HIR::CoreType::Bool:
            ASSERT_BUG(sp, node.m_branches.is_Bool(), "Tree for bool isn't a _Bool - node="<<node);
            this->generate_branches_Bool(sp, node.m_default, node.m_branches.as_Bool(), ty, mv$(val), mv$(and_then));
            break;
        case ::HIR::CoreType::U8:
        case ::HIR::CoreType::U16:
        case ::HIR::CoreType::U32:
        case ::HIR::CoreType::U64:
        case ::HIR::CoreType::U128:
        case ::HIR::CoreType::Usize:
            ASSERT_BUG(sp, node.m_branches.is_Unsigned(), "Tree for unsigned isn't a _Unsigned - node="<<node);
            this->generate_branches_Unsigned(sp, node.m_default, node.m_branches.as_Unsigned(), ty, mv$(val), mv$(and_then));
            break;
        case ::HIR::CoreType::I8:
        case ::HIR::CoreType::I16:
        case ::HIR::CoreType::I32:
        case ::HIR::CoreType::I64:
        case ::HIR::CoreType::I128:
        case ::HIR::CoreType::Isize:
            ASSERT_BUG(sp, node.m_branches.is_Signed(), "Tree for unsigned isn't a _Signed - node="<<node);
            this->generate_branches_Signed(sp, node.m_default, node.m_branches.as_Signed(), ty, mv$(val), mv$(and_then));
            break;
        case ::HIR::CoreType::Char:
            ASSERT_BUG(sp, node.m_branches.is_Unsigned(), "Tree for char isn't a _Unsigned - node="<<node);
            this->generate_branches_Char(sp, node.m_default, node.m_branches.as_Unsigned(), ty, mv$(val), mv$(and_then));
            break;
        case ::HIR::CoreType::Str:
            ASSERT_BUG(sp, node.m_branches.is_String(), "Tree for &str isn't a _String - node="<<node);
            this->generate_branches_Borrow_str(sp, node.m_default, node.m_branches.as_String(), ty, mv$(val), mv$(and_then));
            break;
        case ::HIR::CoreType::F32:
        case ::HIR::CoreType::F64:
            ASSERT_BUG(sp, node.m_branches.is_Float(), "Tree for float isn't a _Float - node="<<node);
            this->generate_branches_Float(sp, node.m_default, node.m_branches.as_Float(), ty, mv$(val), mv$(and_then));
            break;
        default:
            TODO(sp, "Primitive - " << ty);
            break;
        }
        ),
    (Tuple,
        BUG(sp, "Decision node on tuple - node=" << node);
        ),
    (Path,
        // This is either a struct destructure or an enum
        TU_MATCHA( (e.binding), (pbe),
        (Unbound,
            BUG(sp, "Encounterd unbound path - " << e.path);
            ),
        (Opaque,
            and_then(node);
            ),
        (Struct,
            assert(pbe);
            TU_MATCHA( (pbe->m_data), (fields),
            (Unit,
                and_then(node);
                ),
            (Tuple,
                BUG(sp, "Decision node on tuple struct");
                ),
            (Named,
                BUG(sp, "Decision node on struct");
                )
            )
            ),
        (Union,
            TODO(sp, "Decision node on Union");
            ),
        (Enum,
            ASSERT_BUG(sp, node.m_branches.is_Variant(), "Tree for enum isn't a Variant - node="<<node);
            assert(pbe);
            this->generate_branches_Enum(sp, node.m_default, node.m_branches.as_Variant(), node.m_field_path, ty, mv$(val),  mv$(and_then));
            )
        )
        ),
    (Generic,
        and_then(node);
        ),
    (TraitObject,
        ERROR(sp, E0000, "Attempting to match over a trait object");
        ),
    (ErasedType,
        ERROR(sp, E0000, "Attempting to match over an erased type");
        ),
    (Array,
        // TODO: Slice patterns, sequential comparison/sub-match
        TODO(sp, "Match over array");
        ),
    (Slice,
        ASSERT_BUG(sp, node.m_branches.is_Slice(), "Tree for [T] isn't a _Slice - node="<<node);
        this->generate_branches_Slice(sp, node.m_default, node.m_branches.as_Slice(), node.m_field_path, ty, mv$(val), mv$(and_then));
        ),
    (Borrow,
        if( *e.inner == ::HIR::CoreType::Str ) {
            TODO(sp, "Match over &str");
        }
        else {
            BUG(sp, "Decision node on non-str/[T] borrow - " << ty);
        }
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

void DecisionTreeGen::generate_branch(const DecisionTreeNode::Branch& branch, ::std::function<void(const DecisionTreeNode&)> cb)
{
    assert( !branch.is_Unset() );
    if( branch.is_Terminal() ) {
        this->m_builder.end_block( ::MIR::Terminator::make_Goto( this->get_block_for_rule( branch.as_Terminal() ) ) );
    }
    else {
        assert( branch.is_Subtree() );
        const auto& subnode = *branch.as_Subtree();

        cb(subnode);
    }
}

void DecisionTreeGen::generate_branches_Signed(
    const Span& sp,
    const DecisionTreeNode::Branch& default_branch,
    const DecisionTreeNode::Values::Data_Signed& branches,
    const ::HIR::TypeRef& ty, ::MIR::LValue val,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    auto default_block = m_builder.new_bb_unlinked();

    // TODO: Convert into an integer switch w/ offset instead of chained comparisons

    for( const auto& branch : branches )
    {
        auto next_block = (&branch == &branches.back() ? default_block : m_builder.new_bb_unlinked());

        auto val_start = m_builder.lvalue_or_temp(sp, ty, ::MIR::Constant(branch.first.start));
        auto val_end = (branch.first.end == branch.first.start ? val_start.clone() : m_builder.lvalue_or_temp(sp, ty, ::MIR::Constant(branch.first.end)));

        auto cmp_gt_block = m_builder.new_bb_unlinked();
        auto val_cmp_lt = m_builder.lvalue_or_temp(sp, ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
            val.clone(), ::MIR::eBinOp::LT, mv$(val_start)
            }) );
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_lt), default_block, cmp_gt_block }) );
        m_builder.set_cur_block( cmp_gt_block );
        auto success_block = m_builder.new_bb_unlinked();
        auto val_cmp_gt = m_builder.lvalue_or_temp(sp, ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
            val.clone(), ::MIR::eBinOp::GT, mv$(val_end)
            }) );
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_gt), next_block, success_block }) );

        m_builder.set_cur_block( success_block );
        this->generate_branch(branch.second, and_then);

        m_builder.set_cur_block( next_block );
    }
    assert( m_builder.block_active() );

    if( default_branch.is_Unset() ) {
        // TODO: Emit error if non-exhaustive
        m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
    }
    else {
        this->generate_branch(default_branch, and_then);
    }
}

void DecisionTreeGen::generate_branches_Unsigned(
    const Span& sp,
    const DecisionTreeNode::Branch& default_branch,
    const DecisionTreeNode::Values::Data_Unsigned& branches,
    const ::HIR::TypeRef& ty, ::MIR::LValue val,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    auto default_block = m_builder.new_bb_unlinked();

    // TODO: Convert into an integer switch w/ offset instead of chained comparisons

    for( const auto& branch : branches )
    {
        auto next_block = (&branch == &branches.back() ? default_block : m_builder.new_bb_unlinked());

        auto val_start = m_builder.lvalue_or_temp(sp, ty, ::MIR::Constant(branch.first.start));
        auto val_end = (branch.first.end == branch.first.start ? val_start.clone() : m_builder.lvalue_or_temp(sp, ty, ::MIR::Constant(branch.first.end)));

        auto cmp_gt_block = m_builder.new_bb_unlinked();
        auto val_cmp_lt = m_builder.lvalue_or_temp(sp, ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
            val.clone(), ::MIR::eBinOp::LT, mv$(val_start)
            }) );
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_lt), default_block, cmp_gt_block }) );
        m_builder.set_cur_block( cmp_gt_block );
        auto success_block = m_builder.new_bb_unlinked();
        auto val_cmp_gt = m_builder.lvalue_or_temp(sp, ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
            val.clone(), ::MIR::eBinOp::GT, mv$(val_end)
            }) );
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_gt), next_block, success_block }) );

        m_builder.set_cur_block( success_block );
        this->generate_branch(branch.second, and_then);

        m_builder.set_cur_block( next_block );
    }
    assert( m_builder.block_active() );

    if( default_branch.is_Unset() ) {
        // TODO: Emit error if non-exhaustive
        m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
    }
    else {
        this->generate_branch(default_branch, and_then);
    }
}

void DecisionTreeGen::generate_branches_Float(
    const Span& sp,
    const DecisionTreeNode::Branch& default_branch,
    const DecisionTreeNode::Values::Data_Float& branches,
    const ::HIR::TypeRef& ty, ::MIR::LValue val,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    auto default_block = m_builder.new_bb_unlinked();

    for( const auto& branch : branches )
    {
        auto next_block = (&branch == &branches.back() ? default_block : m_builder.new_bb_unlinked());

        auto val_start = m_builder.lvalue_or_temp(sp, ty, ::MIR::Constant(branch.first.start));
        auto val_end = (branch.first.end == branch.first.start ? val_start.clone() : m_builder.lvalue_or_temp(sp, ty, ::MIR::Constant(branch.first.end)));

        auto cmp_gt_block = m_builder.new_bb_unlinked();
        auto val_cmp_lt = m_builder.lvalue_or_temp(sp, ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
            val.clone(), ::MIR::eBinOp::LT, mv$(val_start)
            }) );
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_lt), default_block, cmp_gt_block }) );
        m_builder.set_cur_block( cmp_gt_block );
        auto success_block = m_builder.new_bb_unlinked();
        auto val_cmp_gt = m_builder.lvalue_or_temp(sp, ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
            val.clone(), ::MIR::eBinOp::GT, mv$(val_end)
            }) );
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_gt), next_block, success_block }) );

        m_builder.set_cur_block( success_block );
        this->generate_branch(branch.second, and_then);

        m_builder.set_cur_block( next_block );
    }
    assert( m_builder.block_active() );

    if( default_branch.is_Unset() ) {
        ERROR(sp, E0000, "Match over floating point with no `_` arm");
    }
    else {
        this->generate_branch(default_branch, and_then);
    }
}

void DecisionTreeGen::generate_branches_Char(
    const Span& sp,
    const DecisionTreeNode::Branch& default_branch,
    const DecisionTreeNode::Values::Data_Unsigned& branches,
    const ::HIR::TypeRef& ty, ::MIR::LValue val,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    auto default_block = m_builder.new_bb_unlinked();

    // TODO: Convert into an integer switch w/ offset instead of chained comparisons

    for( const auto& branch : branches )
    {
        auto next_block = (&branch == &branches.back() ? default_block : m_builder.new_bb_unlinked());

        auto val_start = m_builder.lvalue_or_temp(sp, ty, ::MIR::Constant(branch.first.start));
        auto val_end = (branch.first.end == branch.first.start ? val_start.clone() : m_builder.lvalue_or_temp(sp, ty, ::MIR::Constant(branch.first.end)));

        auto cmp_gt_block = m_builder.new_bb_unlinked();
        auto val_cmp_lt = m_builder.lvalue_or_temp( sp, ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
            val.clone(), ::MIR::eBinOp::LT, mv$(val_start)
            }) );
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_lt), default_block, cmp_gt_block }) );
        m_builder.set_cur_block( cmp_gt_block );
        auto success_block = m_builder.new_bb_unlinked();
        auto val_cmp_gt = m_builder.lvalue_or_temp( sp, ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
            val.clone(), ::MIR::eBinOp::GT, mv$(val_end)
            }) );
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_gt), next_block, success_block }) );

        m_builder.set_cur_block( success_block );
        this->generate_branch(branch.second, and_then);

        m_builder.set_cur_block( next_block );
    }
    assert( m_builder.block_active() );

    if( default_branch.is_Unset() ) {
        // TODO: Error if not exhaustive.
        m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
    }
    else {
        this->generate_branch(default_branch, and_then);
    }
}
void DecisionTreeGen::generate_branches_Bool(
    const Span& sp,
    const DecisionTreeNode::Branch& default_branch,
    const DecisionTreeNode::Values::Data_Bool& branches,
    const ::HIR::TypeRef& ty, ::MIR::LValue val,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    //assert( ty.m_data.is_Boolean() );

    if( default_branch.is_Unset() )
    {
        if( branches.false_branch.is_Unset() || branches.true_branch.is_Unset() ) {
            // Non-exhaustive match - ERROR
        }
    }
    else
    {
        if( branches.false_branch.is_Unset() && branches.true_branch.is_Unset() ) {
            // Unreachable default (NOTE: Not an error here)
        }
    }

    // Emit an if based on the route taken
    auto bb_false = m_builder.new_bb_unlinked();
    auto bb_true  = m_builder.new_bb_unlinked();
    m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val), bb_true, bb_false }) );

    // Recurse into sub-patterns
    const auto& branch_false = ( !branches.false_branch.is_Unset() ? branches.false_branch : default_branch );
    const auto& branch_true  = ( !branches. true_branch.is_Unset() ? branches. true_branch : default_branch );

    m_builder.set_cur_block(bb_true );
    this->generate_branch(branch_true , and_then);
    m_builder.set_cur_block(bb_false);
    this->generate_branch(branch_false, and_then);
}

void DecisionTreeGen::generate_branches_Borrow_str(
    const Span& sp,
    const DecisionTreeNode::Branch& default_branch,
    const DecisionTreeNode::Values::Data_String& branches,
    const ::HIR::TypeRef& ty, ::MIR::LValue val,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    // TODO: Chained comparisons with ordering.
    // - Would this just emit a eBinOp? That implies deep codegen support for strings.
    // - rustc emits calls to PartialEq::eq for this and for slices. mrustc could use PartialOrd and fall back to PartialEq if unavaliable?
    //  > Requires crate access here! - A memcmp call is probably better, probably via a binop
    // NOTE: The below implementation gets the final codegen to call memcmp on the strings by emitting eBinOp::{LT,GT}

    // - Remove the wrapping Deref (which must be there)
    ASSERT_BUG(sp, val.is_Deref(), "Match over str without a deref - " << val);
    auto tmp = mv$( *val.as_Deref().val );
    val = mv$(tmp);

    auto default_bb = m_builder.new_bb_unlinked();

    assert( !branches.empty() );
    for(const auto& branch : branches)
    {
        auto have_val = val.clone();

        auto next_bb = (&branch == &branches.back() ? default_bb : m_builder.new_bb_unlinked());

        auto test_val = m_builder.lvalue_or_temp(sp, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ::HIR::CoreType::Str), ::MIR::Constant(branch.first) );
        auto cmp_gt_bb = m_builder.new_bb_unlinked();

        auto lt_val = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ have_val.clone(), ::MIR::eBinOp::LT, test_val.clone() }) );
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(lt_val), default_bb, cmp_gt_bb }) );
        m_builder.set_cur_block(cmp_gt_bb);

        auto eq_bb = m_builder.new_bb_unlinked();
        auto gt_val = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ mv$(have_val), ::MIR::eBinOp::GT, test_val.clone() }) );
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(gt_val), next_bb, eq_bb }) );
        m_builder.set_cur_block(eq_bb);

        this->generate_branch(branch.second, and_then);

        m_builder.set_cur_block(next_bb);
    }
    this->generate_branch(default_branch, and_then);
}

void DecisionTreeGen::generate_branches_Enum(
    const Span& sp,
    const DecisionTreeNode::Branch& default_branch,
    const DecisionTreeNode::Values::Data_Variant& branches,
    const field_path_t& field_path,
    const ::HIR::TypeRef& ty, ::MIR::LValue val,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    const auto& enum_ref = *ty.m_data.as_Path().binding.as_Enum();
    const auto& enum_path = ty.m_data.as_Path().path.m_data.as_Generic();
    const auto& variants = enum_ref.m_variants;
    auto variant_count = variants.size();
    bool has_any = ! default_branch.is_Unset();

    if( branches.size() < variant_count && ! has_any ) {
        ERROR(sp, E0000, "Non-exhaustive match over " << ty << " - " << branches.size() << " out of " << variant_count << " present");
    }
    // DISABLED: Some complex matches don't directly use some defaults
    //if( branches.size() == variant_count && has_any ) {
    //    ERROR(sp, E0000, "Unreachable _ arm - " << branches.size() << " variants in " << enum_path);
    //}

    auto any_block = (has_any ? m_builder.new_bb_unlinked() : 0);

    // Emit a switch over the variant
    ::std::vector< ::MIR::BasicBlockId> variant_blocks;
    variant_blocks.reserve( variant_count );
    for( const auto& branch : branches )
    {
        if( variant_blocks.size() != branch.first ) {
            assert( variant_blocks.size() < branch.first );
            assert( has_any );
            variant_blocks.resize( branch.first, any_block );
        }
        variant_blocks.push_back( m_builder.new_bb_unlinked() );
    }
    if( variant_blocks.size() != variant_count )
    {
        ASSERT_BUG(sp, variant_blocks.size() < variant_count, "Branch count (" << variant_blocks.size() << ") > variant count (" << variant_count << ") in match of " << ty);
        ASSERT_BUG(sp, has_any, "Non-exhaustive match and no any arm");
        variant_blocks.resize( variant_count, any_block );
    }
    bool any_arm_used = ::std::any_of( variant_blocks.begin(), variant_blocks.end(), [any_block](const auto& blk){ return blk == any_block; } );

    m_builder.end_block( ::MIR::Terminator::make_Switch({
        val.clone(), variant_blocks // NOTE: Copies the list, so it can be used lower down
        }) );

    // Emit sub-patterns, looping over variants
    for( const auto& branch : branches )
    {
        auto bb = variant_blocks[branch.first];
        const auto& var = variants[branch.first];
        DEBUG(branch.first << " " << var.first << " = " << branch.second);

        auto var_lval = ::MIR::LValue::make_Downcast({ box$(val.clone()), branch.first });

        ::HIR::TypeRef  fake_ty;

        TU_MATCHA( (var.second), (e),
        (Unit,
            DEBUG("- Unit");
            ),
        (Value,
            DEBUG("- Value");
            ),
        (Tuple,
            // Make a fake tuple
            ::std::vector< ::HIR::TypeRef>  ents;
            for( const auto& fld : e )
            {
                ents.push_back( monomorphise_type(sp,  enum_ref.m_params, enum_path.m_params,  fld.ent) );
            }
            fake_ty = ::HIR::TypeRef( mv$(ents) );
            m_builder.resolve().expand_associated_types(sp, fake_ty);
            DEBUG("- Tuple - " << fake_ty);
            ),
        (Struct,
            ::std::vector< ::HIR::TypeRef>  ents;
            for( const auto& fld : e )
            {
                ents.push_back( monomorphise_type(sp,  enum_ref.m_params, enum_path.m_params,  fld.second.ent) );
            }
            fake_ty = ::HIR::TypeRef( mv$(ents) );
            m_builder.resolve().expand_associated_types(sp, fake_ty);
            DEBUG("- Struct - " << fake_ty);
            )
        )

        m_builder.set_cur_block( bb );
        if( fake_ty == ::HIR::TypeRef() || fake_ty.m_data.as_Tuple().size() == 0 ) {
            this->generate_branch(branch.second, and_then);
        }
        else {
            this->generate_branch(branch.second, [&](auto& subnode) {
                // Call special handler to determine when the enum is over
                this->generate_tree_code__enum(sp, subnode, fake_ty, var_lval, field_path, and_then);
                });
        }
    }

    if( any_arm_used )
    {
        DEBUG("_ = " << default_branch);
        if( !default_branch.is_Unset() )
        {
            m_builder.set_cur_block(any_block);
            this->generate_branch(default_branch, and_then);
        }
    }
    else
    {
        DEBUG("_ = UNUSED - " << default_branch);
    }
}

void DecisionTreeGen::generate_branches_Slice(
    const Span& sp,
    const DecisionTreeNode::Branch& default_branch,
    const DecisionTreeNode::Values::Data_Slice& branches,
    const field_path_t& field_path,
    const ::HIR::TypeRef& ty, ::MIR::LValue val,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    if( default_branch.is_Unset() ) {
        ERROR(sp, E0000, "Non-exhaustive match over " << ty);
    }

    auto val_len = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::RValue::make_DstMeta({ m_builder.get_ptr_to_dst(sp, val).clone() }));

    // NOTE: Un-deref the slice
    ASSERT_BUG(sp, val.is_Deref(), "slice matches must be passed a deref");
    auto tmp = mv$( *val.as_Deref().val );
    val = mv$(tmp);

    auto any_block = m_builder.new_bb_unlinked();

    // TODO: Select one of three ways of picking the arm:
    // - Integer switch (unimplemented)
    // - Binary search
    // - Sequential comparisons

    // TODO: Binary search instead.
    for( const auto& branch : branches.fixed_arms )
    {
        auto val_des = m_builder.lvalue_or_temp(sp, ::HIR::CoreType::Usize, ::MIR::Constant(static_cast<uint64_t>(branch.first)));

        // Special case - final just does equality
        if( &branch == &branches.fixed_arms.back() )
        {
            auto val_cmp_eq = m_builder.lvalue_or_temp( sp, ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
                val_len.clone(), ::MIR::eBinOp::EQ, mv$(val_des)
                }) );

            auto success_block = m_builder.new_bb_unlinked();
            m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_eq), any_block, success_block }) );

            m_builder.set_cur_block( success_block );
            this->generate_branch(branch.second, and_then);

            m_builder.set_cur_block( any_block );
        }
        // TODO: Special case for zero (which can't have a LT)
        else
        {
            auto next_block = m_builder.new_bb_unlinked();

            auto cmp_gt_block = m_builder.new_bb_unlinked();
            auto val_cmp_lt = m_builder.lvalue_or_temp( sp, ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
                val_len.clone(), ::MIR::eBinOp::LT, val_des.clone()
                }) );
            m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_lt), any_block, cmp_gt_block }) );
            m_builder.set_cur_block( cmp_gt_block );
            auto success_block = m_builder.new_bb_unlinked();
            auto val_cmp_gt = m_builder.lvalue_or_temp( sp, ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
                val_len.clone(), ::MIR::eBinOp::GT, mv$(val_des)
                }) );
            m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_gt), next_block, success_block }) );

            m_builder.set_cur_block( success_block );
            this->generate_branch(branch.second, and_then);

            m_builder.set_cur_block( next_block );
        }
    }
    assert( m_builder.block_active() );

    if( default_branch.is_Unset() ) {
        // TODO: Emit error if non-exhaustive
        m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
    }
    else {
        this->generate_branch(default_branch, and_then);
    }
}

namespace {
    bool path_starts_with(const field_path_t& test, const field_path_t& prefix)
    {
        //DEBUG("test="<<test<<", prefix="<<prefix);
        if( test.size() < prefix.size() )
        {
            return false;
        }
        else if( ! ::std::equal(prefix.data.begin(), prefix.data.end(), test.data.begin()) )
        {
            return false;
        }
        else
        {
            return true;
        }
    }
}

void DecisionTreeGen::generate_tree_code__enum(
    const Span& sp,
    const DecisionTreeNode& node, const ::HIR::TypeRef& fake_ty, const ::MIR::LValue& val,
    const field_path_t& path_prefix,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    if( ! path_starts_with(node.m_field_path, path_prefix) )
    {
        and_then(node);
    }
    else
    {
        this->generate_tree_code(sp, node, fake_ty, path_prefix.size(), val,
            [&](const auto& next_node) {
                if( ! path_starts_with(next_node.m_field_path, path_prefix) )
                {
                    and_then(next_node);
                }
                else
                {
                    this->generate_tree_code__enum(sp, next_node, fake_ty, val, path_prefix, and_then);
                }
            });
    }
}

