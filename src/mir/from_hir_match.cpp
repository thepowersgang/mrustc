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

void MIR_LowerHIR_Match( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val );

#define FIELD_DEREF 255

TAGGED_UNION_EX(PatternRule, (), Any,(
    // _ pattern
    (Any, struct {}),
    // Enum variant
    (Variant, struct { unsigned int idx; ::std::vector<PatternRule> sub_rules; }),
    // Boolean (different to Constant because of how restricted it is)
    (Bool, bool),
    // General value
    (Value, ::MIR::Constant),
    (ValueRange, struct { ::MIR::Constant first, last; })
    ),
    ( , field_path(mv$(x.field_path)) ), (field_path = mv$(x.field_path);),
    (
        typedef ::std::vector<uint8_t>  field_path_t;
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
    ::std::vector<PatternRule>  m_rules;
    PatternRule::field_path_t   m_field_path;
    
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
    //  > TODO: Can this goto be avoided while still being defensive? (Avoiding incomplete blocks)
    auto first_cmp_block = builder.new_bb_unlinked();
    builder.end_block( ::MIR::Terminator::make_Goto(first_cmp_block) );

    auto match_scope = builder.new_scope_split(node.span());

    // Map of arm index to ruleset
    ::std::vector< ArmCode> arm_code;
    t_arm_rules arm_rules;
    for(unsigned int arm_idx = 0; arm_idx < node.m_arms.size(); arm_idx ++)
    {
        DEBUG("ARM " << arm_idx);
        /*const*/ auto& arm = node.m_arms[arm_idx];
        ArmCode ac;
        
        // Register introduced bindings to be dropped on return/diverge within this scope
        auto drop_scope = builder.new_scope_var( arm.m_code->span() );
        // - Define variables from the first pattern
        conv.define_vars_from(node.span(), arm.m_patterns.front());
        
        unsigned int pat_idx = 0;
        for( const auto& pat : arm.m_patterns )
        {
            
            // - Convert HIR pattern into ruleset
            auto pat_builder = PatternRulesetBuilder {};
            pat_builder.append_from(node.span(), pat, node.m_value->m_res_type);
            arm_rules.push_back( PatternRuleset { arm_idx, pat_idx, mv$(pat_builder.m_rules) } );
            
            DEBUG("ARM PAT (" << arm_idx << "," << pat_idx << ") " << pat << " ==> [" << arm_rules.back().m_rules << "]");
            
            // - Emit code to destructure the matched pattern
            ac.destructures.push_back( builder.new_bb_unlinked() );
            builder.set_cur_block( ac.destructures.back() );
            conv.destructure_from( arm.m_code->span(), pat, match_val.clone(), true );
            builder.pause_cur_block();
            // NOTE: Paused block resumed upon successful match
            
            pat_idx += 1;
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
            
            conv.visit_node_ptr( arm.m_cond );
            ac.cond_lval = builder.get_result_in_lvalue(arm.m_cond->span(), ::HIR::TypeRef(::HIR::CoreType::Bool));
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
        builder.set_cur_block( ac.code );
        conv.visit_node_ptr( arm.m_code );
        if( !builder.block_active() && !builder.has_result() ) {
            DEBUG("Arm diverged");
            // Nothing need be done, as the block diverged.
            // - Drops were handled by the diverging block (if not, the below will panic)
            auto _ = mv$(drop_scope);
            builder.end_split_arm( arm.m_code->span(), match_scope, false );
        }
        else {
            DEBUG("Arm result");
            // - Set result
            builder.push_stmt_assign( arm.m_code->span(), result_val.clone(), builder.get_result(arm.m_code->span()) );
            // - Drop all non-moved values from this scope
            builder.terminate_scope( arm.m_code->span(), mv$(drop_scope) );
            // - Go to the next block
            builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
            
            builder.end_split_arm( arm.m_code->span(), match_scope, true );
        }

        arm_code.push_back( mv$(ac) );
    }
    
    // TODO: Sort columns of `arm_rules` to maximise effectiveness
    
    // TODO: Detect if a rule is ordering-dependent. In this case we currently have to fall back on the simple match code
    // - A way would be to search for `_` rules with non _ rules following. Would false-positive in some cases, but shouldn't false negative

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
// Dumb and Simple
// --------------------------------------------------------------------
int MIR_LowerHIR_Match_Simple__GeneratePattern(MirBuilder& builder, const Span& sp, const PatternRule* rules, unsigned int num_rules, const ::HIR::TypeRef& ty, const ::MIR::LValue& match_val,  ::MIR::BasicBlockId fail_bb);

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
            const auto& pat_rule = arm_rules[rule_idx];
            bool is_last_pat = (i+1 == arm.m_patterns.size());
            auto next_pattern_bb = (!is_last_pat ? builder.new_bb_unlinked() : next_arm_bb);
            
            // 1. Check
            MIR_LowerHIR_Match_Simple__GeneratePattern(builder, arm.m_code->span(), pat_rule.m_rules.data(), pat_rule.m_rules.size(), node.m_value->m_res_type, match_val, next_pattern_bb);
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

int MIR_LowerHIR_Match_Simple__GeneratePattern(MirBuilder& builder, const Span& sp, const PatternRule* rules, unsigned int num_rules, const ::HIR::TypeRef& ty, const ::MIR::LValue& match_val,  ::MIR::BasicBlockId fail_bb)
{
    TRACE_FUNCTION_F("ty = " << ty);
    assert( num_rules > 0 );
    const auto& rule = *rules;
    TU_MATCHA( (ty.m_data), (te),
    (Infer,
        BUG(sp, "Hit _ in type - " << ty);
        ),
    (Diverge,
        BUG(sp, "Matching over !");
        ),
    (Primitive,
        if( !rule.is_Any() ) {
            switch(te)
            {
            case ::HIR::CoreType::Bool: {
                ASSERT_BUG(sp, rule.is_Bool(), "PatternRule for bool isn't _Bool");
                bool test_val = rule.as_Bool();
                
                auto succ_bb = builder.new_bb_unlinked();
                
                if( test_val ) {
                    builder.end_block( ::MIR::Terminator::make_If({ match_val.clone(), succ_bb, fail_bb }) );
                }
                else {
                    builder.end_block( ::MIR::Terminator::make_If({ match_val.clone(), fail_bb, succ_bb }) );
                }
                builder.set_cur_block(succ_bb);
                } break;
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::Usize:
                TU_MATCH_DEF( PatternRule, (rule), (re),
                (
                    BUG(sp, "PatternRule for integer is not Value or ValueRange");
                    ),
                (Value,
                    auto succ_bb = builder.new_bb_unlinked();
                    
                    auto test_lval = builder.lvalue_or_temp(sp, te, ::MIR::Constant(re.as_Uint()));
                    auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ match_val.clone(), ::MIR::eBinOp::EQ, mv$(test_lval) }));
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
            case ::HIR::CoreType::Isize:
                TU_MATCH_DEF( PatternRule, (rule), (re),
                (
                    BUG(sp, "PatternRule for integer is not Value or ValueRange");
                    ),
                (Value,
                    auto succ_bb = builder.new_bb_unlinked();
                    
                    auto test_lval = builder.lvalue_or_temp(sp, te, ::MIR::Constant(re.as_Int()));
                    auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ match_val.clone(), ::MIR::eBinOp::EQ, mv$(test_lval) }));
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
                    auto cmp_lval = builder.lvalue_or_temp(sp, ::HIR::CoreType::Bool, ::MIR::RValue::make_BinOp({ match_val.clone(), ::MIR::eBinOp::EQ, mv$(test_lval) }));
                    builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_lval), succ_bb, fail_bb }) );
                    builder.set_cur_block(succ_bb);
                    ),
                (ValueRange,
                    TODO(sp, "Simple match over char - ValueRange");
                    )
                )
                break;
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64:
                TODO(sp, "Simple match over float - " << ty);
                break;
            case ::HIR::CoreType::Str:
                BUG(sp, "Match on str shouldn't hit this code");
                break;
            }
        }
        return 1;
        ),
    (Path,
        TU_MATCHA( (te.binding), (pbe),
        (Unbound,
            BUG(sp, "Encounterd unbound path - " << te.path);
            ),
        (Opaque,
            if( !rule.is_Any() ) {
                BUG(sp, "Attempting to match over opaque type - " << ty);
            }
            return 1;
            ),
        (Struct,
            auto monomorph = [&](const auto& ty) { return monomorphise_type(sp, pbe->m_params, te.path.m_data.as_Generic().m_params, ty); };
            const auto& str_data = pbe->m_data;
            TU_MATCHA( (str_data), (sd),
            (Unit,
                if( !rule.is_Any() ) {
                    BUG(sp, "Attempting to match over unit type - " << ty);
                }
                return 1;
                ),
            (Tuple,
                if( !rule.is_Any() ) {
                    TODO(sp, "Match over tuple struct");
                }
                return 1;
                ),
            (Named,
                if( !rule.is_Any() ) {
                    unsigned int total = 0;
                    unsigned int i = 0;
                    for( const auto& fld : sd ) {
                        ::HIR::TypeRef  ent_ty_tmp;
                        const auto& ent_ty = (monomorphise_type_needed(fld.second.ent) ? ent_ty_tmp = monomorph(fld.second.ent) : fld.second.ent);
                        unsigned int cnt = MIR_LowerHIR_Match_Simple__GeneratePattern(
                            builder, sp,
                            rules, num_rules, ent_ty,
                            ::MIR::LValue::make_Field({ box$(match_val.clone()), i }),
                            fail_bb
                            );
                        total += cnt;
                        rules += cnt;
                        num_rules -= cnt;
                        i += 1;
                    }
                    return total;
                }
                else {
                    return 1;
                }
                )
            )
            ),
        (Enum,
            auto monomorph = [&](const auto& ty) { return monomorphise_type(sp, pbe->m_params, te.path.m_data.as_Generic().m_params, ty); };
            if( !rule.is_Any() ) {
                ASSERT_BUG(sp, rule.is_Variant(), "Rule for enum isn't Any or Variant");
                const auto& re = rule.as_Variant();
                unsigned int var_idx = re.idx;
                
                auto next_bb = builder.new_bb_unlinked();
                auto var_count = pbe->m_variants.size();
                
                // Generate a switch with only one option different.
                ::std::vector< ::MIR::BasicBlockId> arms(var_count, fail_bb);
                arms[var_idx] = next_bb;
                builder.end_block( ::MIR::Terminator::make_Switch({ match_val.clone(), mv$(arms) }) );
                
                builder.set_cur_block(next_bb);
                
                const auto& var_data = pbe->m_variants.at(re.idx).second;
                TU_MATCHA( (var_data), (ve),
                (Unit,
                    // Nothing to recurse
                    ),
                (Value,
                    // Nothing to recurse
                    ),
                (Tuple,
                    auto lval_var = ::MIR::LValue::make_Downcast({ box$(match_val.clone()), var_idx });
                    const auto* subrules = re.sub_rules.data();
                    unsigned int subrule_count = re.sub_rules.size();
                    
                    for(unsigned int i = 0; i < ve.size(); i ++)
                    {
                        ::HIR::TypeRef  ent_ty_tmp;
                        const auto& ent_ty = (monomorphise_type_needed(ve[i].ent) ? ent_ty_tmp = monomorph(ve[i].ent) : ve[i].ent);
                        unsigned int cnt = MIR_LowerHIR_Match_Simple__GeneratePattern(
                            builder, sp,
                            subrules, subrule_count, ent_ty,
                            ::MIR::LValue::make_Field({ box$(lval_var.clone()), i }),
                            fail_bb
                            );
                        subrules += cnt;
                        subrule_count -= cnt;
                    }
                    ),
                (Struct,
                    auto lval_var = ::MIR::LValue::make_Downcast({ box$(match_val.clone()), var_idx });
                    const auto* subrules = re.sub_rules.data();
                    unsigned int subrule_count = re.sub_rules.size();
                    
                    for(unsigned int i = 0; i < ve.size(); i ++)
                    {
                        const auto& tpl_ty = ve[i].second.ent;
                        ::HIR::TypeRef  ent_ty_tmp;
                        const auto& ent_ty = (monomorphise_type_needed(tpl_ty) ? ent_ty_tmp = monomorph(tpl_ty) : tpl_ty);
                        unsigned int cnt = MIR_LowerHIR_Match_Simple__GeneratePattern(
                            builder, sp,
                            subrules, subrule_count, ent_ty,
                            ::MIR::LValue::make_Field({ box$(lval_var.clone()), i }),
                            fail_bb
                            );
                        subrules += cnt;
                        subrule_count -= cnt;
                    }
                    )
                )
                // NOTE: All enum variant patterns take one slot
                return 1;
            }
            else {
                return 1;
            }
            )
        )
        ),
    (Generic,
        // Nothing needed
        if( !rule.is_Any() ) {
            BUG(sp, "Attempting to match a generic");
        }
        return 1;
        ),
    (TraitObject,
        if( !rule.is_Any() ) {
            BUG(sp, "Attempting to match a trait object");
        }
        return 1;
        ),
    (Array,
        if( !rule.is_Any() ) {
            TODO(sp, "Match over Array");
        }
        return 1;
        ),
    (Slice,
        if( !rule.is_Any() ) {
            TODO(sp, "Match over Slice");
        }
        return 1;
        ),
    (Tuple,
        if( !rule.is_Any() ) {
            unsigned int total = 0;
            for( unsigned int i = 0; i < te.size(); i ++ ) {
                unsigned int cnt = MIR_LowerHIR_Match_Simple__GeneratePattern(
                    builder, sp,
                    rules, num_rules, te[i],
                    ::MIR::LValue::make_Field({ box$(match_val.clone()), i }),
                    fail_bb
                    );
                total += cnt;
                rules += cnt;
                num_rules -= cnt;
                if( num_rules == 0 )
                    return total;
            }
            return total;
        }
        return 1;
        ),
    (Borrow,
        if( !rule.is_Any() ) {
            return MIR_LowerHIR_Match_Simple__GeneratePattern(builder, sp, rules, num_rules, *te.inner, match_val, fail_bb);
        }
        return 1;
        ),
    (Pointer,
        if( !rule.is_Any() ) {
            BUG(sp, "Attempting to match a pointer");
        }
        return 1;
        ),
    (Function,
        if( !rule.is_Any() ) {
            BUG(sp, "Attempting to match a function pointer");
        }
        return 1;
        ),
    (Closure,
        if( !rule.is_Any() ) {
            BUG(sp, "Attempting to match a closure");
        }
        return 1;
        )
    )
    
    throw "";
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
        
        bool contains(const T& x) const {
            return (start <= x && x <= end);
        }
        bool overlaps(const Range<T>& x) const {
            return (x.start <= start && start <= x.end) || (x.start <= end && end <= x.end);
        }
    };
    
    TAGGED_UNION( Values, Unset,
        (Unset, struct {}),
        (Bool, struct { Branch false_branch, true_branch; }),
        (Variant, ::std::vector< ::std::pair<unsigned int, Branch> >),
        (Unsigned, ::std::vector< ::std::pair< Range<uint64_t>, Branch> >),
        (Signed, ::std::vector< ::std::pair< Range<int64_t>, Branch> >),
        (String, ::std::vector< ::std::pair< ::std::string, Branch> >)
        );
    
    // TODO: Arm specialisation
    bool is_specialisation;
    PatternRule::field_path_t   m_field_path;
    Values  m_branches;
    Branch  m_default;
    
    DecisionTreeNode( PatternRule::field_path_t field_path ):
        is_specialisation(false)/*,
        m_field_path( mv$(field_path) ) // */
    {}
    
    static Branch new_branch_subtree(PatternRule::field_path_t path) {
        return Branch( box$(DecisionTreeNode( mv$(path) )) );
    }
    
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
                //if( e == arm_index )    // Wut? How would this happen? Exact duplicate pattern
                //    return ;
                BUG(sp, "Duplicate terminal - Existing goes to arm " << e << ", new goes to arm " << e );
                )
            )
            branch = Branch::make_Terminal(arm_index);
            });
    }
    // `and_then` - Closure called after processing the final rule
    void populate_tree_from_rule(const Span& sp, const PatternRule* first_rule, unsigned int rule_count, ::std::function<void(Branch&)> and_then);
    
    /// Simplifies the tree by eliminating nodes with just a default
    void simplify();
    /// Propagate the m_default arm's contents to value arms, and vice-versa
    void propagate_default();
    /// HELPER: Unfies the rules from the provided branch with this node
    void unify_from(const Branch& b);
    
    ::MIR::LValue get_field(const ::MIR::LValue& base, unsigned int base_depth) const {
        ::MIR::LValue   cur = base.clone();
        for(unsigned int i = base_depth; i < m_field_path.size(); i ++ ) {
            const auto idx = m_field_path[i];
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
        generate_tree_code(sp, node, ty, 0, val, 0,0, [](const auto& n){ DEBUG("final node = " << n); });
    }
    void generate_tree_code(
        const Span& sp,
        const DecisionTreeNode& node,
        const ::HIR::TypeRef& ty, unsigned int ty_ofs,
        const ::MIR::LValue& base_val, unsigned int depth, unsigned int base_depth,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
    
    void generate_branch(const DecisionTreeNode::Branch& branch, ::std::function<void(const DecisionTreeNode&)> cb);
    
    void generate_branches_Unsigned(
        const Span& sp,
        const DecisionTreeNode::Branch& default_branch,
        const DecisionTreeNode::Values::Data_Unsigned& branches,
        const ::HIR::TypeRef& ty,/* unsigned int _ty_ofs,*/
        const ::MIR::LValue& val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
    void generate_branches_Char(
        const Span& sp,
        const DecisionTreeNode::Branch& default_branch,
        const DecisionTreeNode::Values::Data_Unsigned& branches,
        const ::HIR::TypeRef& ty,/* unsigned int _ty_ofs,*/
        const ::MIR::LValue& val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
    
    void generate_branches_Enum(
        const Span& sp,
        const DecisionTreeNode::Branch& default_branch,
        const DecisionTreeNode::Values::Data_Variant& branches,
        const ::HIR::TypeRef& ty,/* unsigned int _ty_ofs,*/
        const ::MIR::LValue& val, unsigned int depth,
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
    for( const auto& arm_rule : arm_rules )
    {
        auto arm_idx = arm_rule.arm_idx;
        DEBUG("(" << arm_idx << ", " << arm_rule.pat_idx << "): " << arm_rule.m_rules);
        root_node.populate_tree_from_rule( node.m_arms[arm_idx].m_code->span(), arm_idx, arm_rule.m_rules.data(), arm_rule.m_rules.size() );
    }
    DEBUG("root_node = " << root_node);
    root_node.simplify();
    root_node.propagate_default();
    DEBUG("root_node = " << root_node);
    
    // - Convert the above decision tree into MIR
    DEBUG("- Emitting decision tree");
    DecisionTreeGen gen { builder, rule_blocks };
    builder.set_cur_block( first_cmp_block );
    gen.generate_tree_code( node.span(), root_node, node.m_value->m_res_type, mv$(match_val) );
    ASSERT_BUG(node.span(), !builder.block_active(), "Decision tree didn't terminate the final block");
}

::std::ostream& operator<<(::std::ostream& os, const PatternRule& x)
{
    os <<"{";
    for(const auto idx : x.field_path)
        os << "." << static_cast<unsigned int>(idx);
    os << "}=";
    TU_MATCHA( (x), (e),
    (Any,
        os << "_";
        ),
    // Enum variant
    (Variant,
        os << e.idx << " [" << e.sub_rules << "]";
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

void PatternRulesetBuilder::append_from(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::TypeRef& ty)
{
    TRACE_FUNCTION_F("pat="<<pat<<", ty="<<ty<<",   m_field_path.size()=" <<m_field_path.size() << " " << (m_field_path.empty() ? 0 : m_field_path.back()) );
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
    };
    
    TU_MATCHA( (ty.m_data), (e),
    (Infer,   BUG(sp, "Ivar for in match type"); ),
    (Diverge, BUG(sp, "Diverge in match type");  ),
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
                TODO(sp, "Match over float, is it valid?");
                } break;
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::Usize: {
                uint64_t start = H::get_pattern_value_int(sp, pat, pe.start);
                uint64_t end   = H::get_pattern_value_int(sp, pat, pe.end  );
                this->push_rule( PatternRule::make_ValueRange( {::MIR::Constant(start), ::MIR::Constant(end)} ) );
                } break;
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
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
                TODO(sp, "Match over float, is it valid?");
                //double val = pe.val.as_Float().value;
                //this->push_rule( PatternRule::make_Value( ::MIR::Constant(val) ) );
                } break;
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::Usize: {
                uint64_t val = H::get_pattern_value_int(sp, pat, pe.val);
                this->push_rule( PatternRule::make_Value( ::MIR::Constant(val) ) );
                } break;
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
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
            auto monomorph = [&](const auto& ty) { return monomorphise_type(sp, pbe->m_params, e.path.m_data.as_Generic().m_params, ty); };
            const auto& str_data = pbe->m_data;
            TU_MATCHA( (str_data), (sd),
            (Unit,
                TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
                ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
                (Any,
                    // Nothing.
                    this->push_rule( PatternRule::make_Any({}) );
                    ),
                (Value,
                    TODO(sp, "Match over struct - Unit + Value");
                    )
                )
                ),
            (Tuple,
                TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
                ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
                (Any,
                    TODO(sp, "Match over struct - Tuple + Any");
                    ),
                (StructTuple,
                    TODO(sp, "Match over struct - Tuple + StructTuple");
                    )
                )
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
                    TODO(sp, "Match over struct - Named + Struct");
                    )
                )
                )
            )
            ),
        (Enum,
            auto monomorph = [&](const auto& ty) { return monomorphise_type(sp,  pbe->m_params, e.path.m_data.as_Generic().m_params,  ty); };
            TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
            ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
            (Any,
                this->push_rule( PatternRule::make_Any({}) );
                ),
            (EnumValue,
                this->push_rule( PatternRule::make_Variant( {pe.binding_idx, {} } ) );
                ),
            (EnumTuple,
                const auto& var_def = pe.binding_ptr->m_variants.at(pe.binding_idx);
                
                const auto& fields_def = var_def.second.as_Tuple();
                PatternRulesetBuilder   sub_builder;
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
                PatternRulesetBuilder   sub_builder;
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
    (Array,
        // TODO: Slice patterns, sequential comparison/sub-match
        TODO(sp, "Match over array");
        ),
    (Slice,
        if( pat.m_data.is_Any() ) {
        }
        else {
            BUG(sp, "Hit match over `[T]` - must be `&[T]`");
        }
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
            if( pe.val.is_String() ) {
                const auto& s = pe.val.as_String();
                this->push_rule( PatternRule::make_Value(s) );
            }
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
    (String,
        Values::Data_String rv;
        rv.reserve(e.size());
        for(const auto& v : e)
            rv.push_back( ::std::make_pair(v.first, clone(v.second)) );
        return Values( mv$(rv) );
        )
    )
    throw "";
}
DecisionTreeNode DecisionTreeNode::clone() const {
    DecisionTreeNode    rv(m_field_path);
    rv.is_specialisation = is_specialisation;
    rv.m_branches = clone(m_branches);
    rv.m_default = clone(m_default);
    return rv;
}
void DecisionTreeNode::populate_tree_from_rule(const Span& sp, const PatternRule* first_rule, unsigned int rule_count, ::std::function<void(Branch&)> and_then)
{
    assert( rule_count > 0 );
    const auto& rule = *first_rule;
    
    if( m_field_path.size() == 0 ) {
        m_field_path = rule.field_path;
    }
    else {
        assert( m_field_path == rule.field_path );
    }
    
    TU_MATCHA( (rule), (e),
    (Any, {
        if( m_default.is_Unset() ) {
            if( rule_count == 1 ) {
                and_then(m_default);
            }
            else {
                m_default = new_branch_subtree(rule.field_path);
                m_default.as_Subtree()->populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
            }
        }
        else TU_IFLET( Branch, m_default, Subtree, be,
            if( rule_count == 1 ) {
                and_then( m_default );
            }
            else {
                be->populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
            }
        )
        else {
            // NOTE: All lists processed as part of the same tree should be the same length
            BUG(sp, "Duplicate terminal rule");
        }
        }),
    (Variant, {
        if( m_branches.is_Unset() ) {
            m_branches = Values::make_Variant({});
        }
        if( !m_branches.is_Variant() ) {
            BUG(sp, "Mismatched rules");
        }
        auto& be = m_branches.as_Variant();
        
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
    (Bool,
        if( m_branches.is_Unset() ) {
            m_branches = Values::make_Bool({});
        }
        else if( !m_branches.is_Bool() ) {
            BUG(sp, "Mismatched rules");
        }
        auto& be = m_branches.as_Bool();
        
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
            TODO(sp, "Value patterns - Int");
            ),
        (Uint,
            if( m_branches.is_Unset() ) {
                m_branches = Values::make_Unsigned({});
            }
            else if( !m_branches.is_Unsigned() ) {
                BUG(sp, "Mismatched rules");
            }
            auto& be = m_branches.as_Unsigned();
            auto it = ::std::find_if(be.begin(), be.end(), [&](const auto& v){ return v.first.end >= ve; });
            if( it == be.end() || it->first.start > ve ) {
                it = be.insert( it, ::std::make_pair( Range<uint64_t> { ve,ve }, new_branch_subtree(rule.field_path) ) );
            }
            else if( it->first.start == ve && it->first.end == ve ) {
                // Equal, continue and add sub-pat
            }
            else {
                // Collide or overlap!
                TODO(sp, "Value patterns - Uint - Overlapping - " << it->first.start << " <= " << ve << " <= " << it->first.end);
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
        (Float,
            TODO(sp, "Value patterns - Float");
            ),
        (Bool,
            throw "";
            ),
        (Bytes,
            TODO(sp, "Value patterns - Bytes");
            ),
        (StaticString,
            if( m_branches.is_Unset() ) {
                m_branches = Values::make_String({});
            }
            else if( !m_branches.is_String() ) {
                BUG(sp, "Mismatched rules");
            }
            auto& be = m_branches.as_String();
            
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
            TODO(sp, "ValueRange patterns - Int");
            ),
        (Uint,
            if( m_branches.is_Unset() ) {
                m_branches = Values::make_Unsigned({});
            }
            else if( !m_branches.is_Unsigned() ) {
                BUG(sp, "Mismatched rules");
            }
            auto& be = m_branches.as_Unsigned();
            // Find the first entry that sorts after this range
            auto it = ::std::find_if(be.begin(), be.end(), [&](const auto& v){ return v.first >= ve_start || v.first.contains(ve_end); });
            // If the end of the list was reached, OR the located entry sorts after the end of this range
            if( it == be.end() || it->first >= ve_end ) {
                it = be.insert( it, ::std::make_pair( Range<uint64_t> { ve_start,ve_end }, new_branch_subtree(rule.field_path) ) );
            }
            else if( it->first.start == ve_start && it->first.end == ve_end ) {
                // Equal, add sub-pattern
            }
            else {
                // Collide or overlap!
                // - Overlap should split around the collision and merge into it (if this is less-specific).
                ASSERT_BUG(sp, ve_start < ve_end, "Range pattern with one value");
                ASSERT_BUG(sp, it->first.start == it->first.end, "Attempting to override a range pattern with another range");
                
                // TODO: This breaks miserably if the new range overlaps with two other ranges
                
                // Insert half of ve before, and the other half after? OR Create a sub-pattern specializing.
                if( ve_start == it->first.start ) {
                    // Add single range after
                    it ++;
                    it = be.insert(it, ::std::make_pair( Range<uint64_t> { ve_start + 1, ve_end }, new_branch_subtree(rule.field_path) ) );
                }
                else if( ve_end == it->first.start ) {
                    // Add single range before
                    it = be.insert(it, ::std::make_pair( Range<uint64_t> { ve_start, ve_end - 1 }, new_branch_subtree(rule.field_path) ) );
                }
                else {
                    // Add two ranges
                    auto end1 = it->first.start - 1;
                    auto start2 = it->first.end + 1;
                    it = be.insert(it, ::std::make_pair( Range<uint64_t> { ve_start, end1 }, new_branch_subtree(rule.field_path) ) );
                    auto& branch_1 = it->second;
                    it ++;
                    it ++;   // Skip the original entry
                    it = be.insert(it, ::std::make_pair( Range<uint64_t> { start2, ve_end }, new_branch_subtree(rule.field_path) ) );
                    auto& branch_2 = it->second;
                    
                    if( rule_count > 1 )
                    {
                        branch_1.as_Subtree()->populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
                        branch_2.as_Subtree()->populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
                    }
                    else
                    {
                        and_then(branch_1);
                        and_then(branch_2);
                    }
                    return ;
                }
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
        (Float,
            TODO(sp, "ValueRange patterns - Float");
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
    (String,
        for(auto& branch : e) {
            H::simplify_branch(branch.second);
        }
        )
    )
    
    H::simplify_branch(m_default);
}

void DecisionTreeNode::propagate_default()
{
    struct H {
        static void handle_branch(Branch& b, const Branch& def) {
            TU_IFLET(Branch, b, Subtree, be,
                be->propagate_default();
                if( be->m_default.is_Unset() ) {
                    be->unify_from(def);
                }
                be->propagate_default();
            )
        }
    };
    
    TU_MATCHA( (m_branches), (e),
    (Unset,
        ),
    (Bool,
        H::handle_branch(e.false_branch, m_default);
        H::handle_branch(e.true_branch, m_default);
        ),
    (Variant,
        for(auto& branch : e) {
            H::handle_branch(branch.second, m_default);
        }
        ),
    (Unsigned,
        for(auto& branch : e) {
            H::handle_branch(branch.second, m_default);
        }
        ),
    (Signed,
        for(auto& branch : e) {
            H::handle_branch(branch.second, m_default);
        }
        ),
    (String,
        for(auto& branch : e) {
            H::handle_branch(branch.second, m_default);
        }
        )
    )
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
            (String,
                for(auto& branch : e) {
                    be->unify_from(branch.second);
                }
                )
            )
        }
    )
}
void DecisionTreeNode::unify_from(const Branch& b)
{
    TU_MATCHA( (b), (be),
    (Unset,
        ),
    (Terminal,
        if( m_default.is_Unset() ) {
            m_default = Branch(be);
        }
        ),
    (Subtree,
        assert( be->m_branches.tag() == m_branches.tag() );
        TU_MATCHA( (be->m_branches, m_branches), (src, dst),
        (Unset,
            ),
        (Bool,
            if( dst.false_branch.is_Unset() ) {
                dst.false_branch = clone(src.false_branch);
            }
            if( dst.true_branch.is_Unset() ) {
                dst.true_branch = clone(src.false_branch);
            }
            ),
        (Variant,
            // Insert items not already present
            for(const auto& srcv : src)
            {
                auto it = ::std::find_if( dst.begin(), dst.end(), [&](const auto& x){ return x.first >= srcv.first; });
                // Not found? Insert a new branch
                if( it == dst.end() || it->first != srcv.first ) {
                    it = dst.insert(it, ::std::make_pair(srcv.first, clone(srcv.second)));
                }
            }
            ),
        (Unsigned,
            for(const auto& srcv : src)
            {
                // Find the first entry with an end greater than or equal to the start of this entry
                auto it = ::std::find_if( dst.begin(), dst.end(), [&](const auto& x){ return x.first.end >= srcv.first.start; });
                // Not found? Insert a new branch
                if( it == dst.end() ) {
                    it = dst.insert(it, ::std::make_pair(srcv.first, clone(srcv.second)));
                }
                // If the found entry doesn't overlap (the start of `*it` is after the end of `srcv`)
                else if( it->first.start > srcv.first.end ) {
                    it = dst.insert(it, ::std::make_pair(srcv.first, clone(srcv.second)));
                }
                else {
                    // NOTE: Overlapping doesn't get handled here
                }
            }
            ),
        (Signed,
            for(const auto& srcv : src)
            {
                // Find the first entry with an end greater than or equal to the start of this entry
                auto it = ::std::find_if( dst.begin(), dst.end(), [&](const auto& x){ return x.first.end >= srcv.first.start; });
                // Not found? Insert a new branch
                if( it == dst.end() ) {
                    it = dst.insert(it, ::std::make_pair( srcv.first, clone(srcv.second) ));
                }
                // If the found entry doesn't overlap (the start of `*it` is after the end of `srcv`)
                else if( it->first.start > srcv.first.end ) {
                    it = dst.insert(it, ::std::make_pair( srcv.first, clone(srcv.second) ));
                }
                else {
                    // NOTE: Overlapping doesn't get handled here
                }
            }
            ),
        (String,
            for(const auto& srcv : src)
            {
                auto it = ::std::find_if( dst.begin(), dst.end(), [&](const auto& x){ return x.first >= srcv.first; });
                // Not found? Insert a new branch
                if( it == dst.end() ) {
                    it = dst.insert(it, ::std::make_pair( srcv.first, clone(srcv.second) ));
                }
                else {
                }
            }
            )
        )
        if( m_default.is_Unset() ) {
            m_default = clone(be->m_default);
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
    os << "DTN [";
    for(const auto idx : x.m_field_path)
        os << "." << static_cast<unsigned int>(idx);
    os << "] { ";
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
    (String,
        for(const auto& branch : e) {
            os << "\"" << branch.first << "\"" << " = " << branch.second << ", ";
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
    const ::HIR::TypeRef& ty, unsigned int ty_ofs,
    const ::MIR::LValue& base_val, unsigned int depth, unsigned int base_depth,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    TRACE_FUNCTION_F("ty=" << ty << ", ty_ofs=" << ty_ofs << ", depth="<<depth<<", node=" << node);
    
    #if 1
    if( depth > node.m_field_path.size() ) {
        and_then(node);
        return ;
    }
    #else
    assert( depth <= node.m_field_path.size() );
    #endif
    
    TU_MATCHA( (ty.m_data), (e),
    (Infer,   BUG(sp, "Ivar for in match type"); ),
    (Diverge, BUG(sp, "Diverge in match type");  ),
    (Primitive,
        switch(e)
        {
        case ::HIR::CoreType::Bool: {
            ASSERT_BUG(sp, node.m_branches.is_Bool(), "Tree for bool isn't a _Bool - node="<<node);
            //this->generate_branches_Bool(sp, node.m_branches.as_Bool(), ty, val, mv$(and_then));
            const auto& branches = node.m_branches.as_Bool();
            
            if( node.m_default.is_Unset() )
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
            m_builder.end_block( ::MIR::Terminator::make_If({ node.get_field(base_val, base_depth), bb_true, bb_false }) );
            
            // Recurse into sub-patterns
            const auto& branch_false = ( !branches.false_branch.is_Unset() ? branches.false_branch : node.m_default );
            const auto& branch_true  = ( !branches. true_branch.is_Unset() ? branches. true_branch : node.m_default );
            
            m_builder.set_cur_block(bb_true );
            this->generate_branch(branch_true , and_then);
            m_builder.set_cur_block(bb_false);
            this->generate_branch(branch_false, and_then);
            
            } break;
        case ::HIR::CoreType::U8:
        case ::HIR::CoreType::U16:
        case ::HIR::CoreType::U32:
        case ::HIR::CoreType::U64:
        case ::HIR::CoreType::Usize:
            ASSERT_BUG(sp, node.m_branches.is_Unsigned(), "Tree for unsigned isn't a _Unsigned - node="<<node);
            this->generate_branches_Unsigned(sp, node.m_default, node.m_branches.as_Unsigned(), ty, node.get_field(base_val, base_depth), mv$(and_then));
            break;
        case ::HIR::CoreType::Char:
            ASSERT_BUG(sp, node.m_branches.is_Unsigned(), "Tree for char isn't a _Unsigned - node="<<node);
            this->generate_branches_Char(sp, node.m_default, node.m_branches.as_Unsigned(), ty, node.get_field(base_val, base_depth), mv$(and_then));
            break;
        default:
            TODO(sp, "Primitive - " << ty);
            break;
        }
        ),
    (Tuple,
        // Tuple - Recurse on each sub-type (increasing the index)
        // - If complete, call completion callback
        if( ty_ofs == e.size() ) {
            and_then(node);
        }
        // - If the node is for this type, recurse
        else {
            assert( depth < node.m_field_path.size() );
            if( node.m_field_path[depth] == ty_ofs ) {
                generate_tree_code( sp, node,
                    e[ty_ofs], 0, base_val, depth+1, base_depth,
                    [&](auto& n){ this->generate_tree_code(sp, n, ty, ty_ofs+1, base_val, depth, base_depth, and_then); }
                    );
            }
            // - Otherwise, go to the next node
            else {
                this->generate_tree_code(sp, node, ty, ty_ofs+1, base_val, depth, base_depth, and_then);
            }
        }
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
            auto monomorph = [&](const auto& ty) { return monomorphise_type(sp, pbe->m_params, e.path.m_data.as_Generic().m_params, ty); };
            TU_MATCHA( (pbe->m_data), (fields),
            (Unit,
                and_then(node);
                ),
            (Tuple,
                if( ty_ofs == fields.size() ) {
                    and_then(node);
                }
                else {
                    assert( depth < node.m_field_path.size() );
                    if( node.m_field_path[depth] == ty_ofs ) {
                        const auto& fld = fields[ty_ofs];
                        ::HIR::TypeRef  tmp;
                        const auto& fld_ty = (monomorphise_type_needed(fld.ent) ? tmp = monomorph(fld.ent) : fld.ent);
                        generate_tree_code( sp, node,
                            fld_ty, 0, base_val, depth+1, base_depth,
                            [&](auto& n){ this->generate_tree_code(sp, n, ty, ty_ofs+1, base_val, depth, base_depth, and_then); }
                            );
                    }
                    else {
                        this->generate_tree_code(sp, node, ty, ty_ofs+1, base_val, depth, base_depth, and_then);
                    }
                }
                ),
            (Named,
                if( ty_ofs == fields.size() ) {
                    and_then(node);
                }
                else {
                    assert( depth < node.m_field_path.size() );
                    if( node.m_field_path[depth] == ty_ofs ) {
                        const auto& fld = fields[ty_ofs].second;
                        ::HIR::TypeRef  tmp;
                        const auto& fld_ty = (monomorphise_type_needed(fld.ent) ? tmp = monomorph(fld.ent) : fld.ent);
                        generate_tree_code( sp, node,
                            fld_ty, 0, base_val, depth+1, base_depth,
                            [&](auto& n){ this->generate_tree_code(sp, n, ty, ty_ofs+1, base_val, depth, base_depth, and_then); }
                            );
                    }
                    else {
                        this->generate_tree_code(sp, node, ty, ty_ofs+1, base_val, depth, base_depth, and_then);
                    }
                }
                )
            )
            ),
        (Enum,
            ASSERT_BUG(sp, node.m_branches.is_Variant(), "Tree for enum isn't a Variant - node="<<node);
            assert(pbe);
            this->generate_branches_Enum(sp, node.m_default, node.m_branches.as_Variant(), ty, node.get_field(base_val, base_depth), depth, mv$(and_then));
            )
        )
        ),
    (Generic,
        and_then(node);
        ),
    (TraitObject,
        ERROR(sp, E0000, "Attempting to match over a trait object");
        ),
    (Array,
        // TODO: Slice patterns, sequential comparison/sub-match
        TODO(sp, "Match over array");
        ),
    (Slice,
        BUG(sp, "Hit match over `[T]` - must be `&[T]`");
        ),
    (Borrow,
        if( *e.inner == ::HIR::TypeRef(::HIR::CoreType::Str) ) {
            // TODO: Chained comparisons with ordering.
            // - Would this just emit a eBinOp? That implies deep codegen support for strings.
            // - rustc emits calls to PartialEq::eq for this and for slices. mrustc could use PartialOrd and fall back to PartialEq if unavaliable?
            //  > Requires crate access here! - A memcmp call is probably better, probably via a binop
            
            // NOTE: The below implementation gets the final codegen to call memcmp on the strings by emitting eBinOp::{LT,GT}

            ASSERT_BUG(sp, node.m_branches.is_String(), "Tree for &str isn't a _String - node="<<node);
            const auto& branches = node.m_branches.as_String();
            
            auto default_bb = m_builder.new_bb_unlinked();
            
            // 
            assert( !branches.empty() );
            for(const auto& branch : branches)
            {
                auto have_val = node.get_field(base_val, base_depth);
                
                auto next_bb = (&branch == &branches.back() ? default_bb : m_builder.new_bb_unlinked());
                
                auto test_val = m_builder.lvalue_or_temp(sp, ::HIR::TypeRef(::HIR::CoreType::Str), ::MIR::Constant(branch.first) );
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
            this->generate_branch(node.m_default, and_then);
        }
        else if( e.inner->m_data.is_Slice() ) {
            TODO(sp, "Match over &[T]");
        }
        else {
            // TODO: Should this check if the index is -1? The assertion doesn't fire in general use, so looks good.
            ASSERT_BUG( sp,  node.m_field_path[depth] == FIELD_DEREF, "& not matching on deref " << depth << " node=" <<node );
            generate_tree_code( sp, node, *e.inner, 0, base_val, depth+1, base_depth,  and_then );
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

void DecisionTreeGen::generate_branches_Unsigned(
    const Span& sp,
    const DecisionTreeNode::Branch& default_branch,
    const DecisionTreeNode::Values::Data_Unsigned& branches,
    const ::HIR::TypeRef& ty,/* unsigned int _ty_ofs,*/
    const ::MIR::LValue& val,
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
void DecisionTreeGen::generate_branches_Char(
    const Span& sp,
    const DecisionTreeNode::Branch& default_branch,
    const DecisionTreeNode::Values::Data_Unsigned& branches,
    const ::HIR::TypeRef& ty,/* unsigned int _ty_ofs,*/
    const ::MIR::LValue& val,
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
void DecisionTreeGen::generate_branches_Enum(
    const Span& sp,
    const DecisionTreeNode::Branch& default_branch,
    const DecisionTreeNode::Values::Data_Variant& branches,
    const ::HIR::TypeRef& ty,/* unsigned int _ty_ofs,*/
    const ::MIR::LValue& val, unsigned int depth,
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
        assert( variant_blocks.size() < variant_count );
        assert( has_any );
        variant_blocks.resize( variant_count, any_block );
    }
    
    m_builder.end_block( ::MIR::Terminator::make_Switch({
        val.clone(), variant_blocks // NOTE: Copies the list, so it can be used lower down
        }) );
    
    // Emit sub-patterns, looping over variants
    for( const auto& branch : branches )
    {
        auto bb = variant_blocks[branch.first];
        const auto& var = variants[branch.first];
        DEBUG(branch.first << " " << var.first << " = " << branch.second);
        m_builder.set_cur_block( bb );
        this->generate_branch(branch.second, [&](auto& subnode) {
            TU_MATCHA( (var.second), (e),
            (Unit,
                and_then( subnode );
                ),
            (Value,
                and_then( subnode );
                ),
            (Tuple,
                if( depth < subnode.m_field_path.size() )
                {
                    // Make a fake tuple
                    ::std::vector< ::HIR::TypeRef>  ents;
                    for( const auto& fld : e )
                    {
                        ents.push_back( monomorphise_type(sp,  enum_ref.m_params, enum_path.m_params,  fld.ent) );
                    }
                    ::HIR::TypeRef  fake_ty { mv$(ents) };
                    // NOTE: Depth is increased by the tuple code
                    this->generate_tree_code(sp, subnode, fake_ty, 0, ::MIR::LValue::make_Downcast({ box$(val.clone()), branch.first }), depth, depth, and_then);
                }
                else {
                    and_then( subnode );
                }
                ),
            (Struct,
                if( depth < subnode.m_field_path.size() )
                {
                    ::std::vector< ::HIR::TypeRef>  ents;
                    for( const auto& fld : e )
                    {
                        ents.push_back( monomorphise_type(sp,  enum_ref.m_params, enum_path.m_params,  fld.second.ent) );
                    }
                    ::HIR::TypeRef  fake_ty { mv$(ents) };
                    // NOTE: Depth is increased by the tuple code
                    this->generate_tree_code(sp, subnode, fake_ty, 0, ::MIR::LValue::make_Downcast({ box$(val.clone()), branch.first }), depth, depth, and_then);
                }
                else {
                    and_then( subnode );
                }
                )
            )
            });
    }
    
    DEBUG("_ = " << default_branch);
    if( !default_branch.is_Unset() )
    {
        m_builder.set_cur_block(any_block);
        this->generate_branch(default_branch, and_then);
    }
}
