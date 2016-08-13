/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/from_hir_match.cpp
 * - Conversion of `match` blocks into MIR
 */
#include "from_hir.hpp"
#include <hir_typeck/helpers.hpp>   // monomorphise_type
#include <algorithm>

void MIR_LowerHIR_Match( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val );

TAGGED_UNION(PatternRule, Any,
    // _ pattern
    (Any, struct {}),
    // Enum variant
    (Variant, struct { unsigned int idx; ::std::vector<PatternRule> sub_rules; }),
    // Boolean (different to Constant because of how restricted it is)
    (Bool, bool),
    // General value
    (Value, ::MIR::Constant),
    (ValueRange, struct { ::MIR::Constant first, last; })
    );
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
    ::MIR::BasicBlockId   code;
    bool has_condition;
    ::MIR::BasicBlockId   cond_code; // NOTE: Incomplete, requires terminating If
    ::MIR::LValue   cond_lval;
};

typedef ::std::vector<PatternRuleset>  t_arm_rules;

void MIR_LowerHIR_Match_Simple( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arm_code, ::MIR::BasicBlockId first_cmp_block);
void MIR_LowerHIR_Match_DecisionTree( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arm_code , ::MIR::BasicBlockId first_cmp_block);
/// Helper to construct rules from a passed pattern
struct PatternRulesetBuilder
{
    ::std::vector<PatternRule>  m_rules;
    void append_from(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::TypeRef& ty);
};

// --------------------------------------------------------------------
// CODE
// --------------------------------------------------------------------

// Handles lowering non-trivial matches to MIR
// - Non-trivial means that there's more than one pattern
void MIR_LowerHIR_Match( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val )
{
    bool fall_back_on_simple = false;
    
    auto result_val = builder.new_temporary( node.m_res_type );
    auto next_block = builder.new_bb_unlinked();
    
    // 1. Stop the current block so we can generate code
    //  > TODO: Can this goto be avoided while still being defensive? (Avoiding incomplete blocks)
    auto first_cmp_block = builder.new_bb_unlinked();
    builder.end_block( ::MIR::Terminator::make_Goto(next_block) );

    // Map of arm index to ruleset
    ::std::vector< ArmCode> arm_code;
    t_arm_rules arm_rules;
    for(unsigned int arm_idx = 0; arm_idx < node.m_arms.size(); arm_idx ++)
    {
        ArmCode ac;
        
        /*const*/ auto& arm = node.m_arms[arm_idx];
        unsigned int pat_idx = 0;
        for( const auto& pat : arm.m_patterns )
        {
            auto pat_builder = PatternRulesetBuilder {};
            pat_builder.append_from(node.span(), pat, node.m_value->m_res_type);
            arm_rules.push_back( PatternRuleset { arm_idx, pat_idx, mv$(pat_builder.m_rules) } );
            pat_idx += 1;
        }
        
        // TODO: Register introduced bindings to be dropped on return/diverge within this scope
        //auto drop_scope = builder.new_drop_scope( arm.m_patterns[0] );

        // Code
        ac.code = builder.new_bb_unlinked();
        builder.set_cur_block( ac.code );
        conv.visit_node_ptr( arm.m_code );
        if( !builder.block_active() && !builder.has_result() ) {
            // Nothing need be done, as the block diverged.
        }
        else {
            builder.push_stmt_assign( result_val.clone(), builder.get_result(arm.m_code->span()) );
            builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
        }
        
        // Condition
        if(arm.m_cond)
        {
            ac.has_condition = true;
            ac.cond_code = builder.new_bb_unlinked();
            builder.set_cur_block( ac.cond_code );
            
            conv.visit_node_ptr( arm.m_cond );
            ac.cond_lval = builder.lvalue_or_temp( ::HIR::TypeRef(::HIR::CoreType::Bool), builder.get_result(arm.m_cond->span()) );
            builder.pause_cur_block();
            // NOTE: Paused so that later code (which knows what the false branch will be) can end it correctly
            
            // TODO: What to do with contidionals?
            // > Could fall back on a sequential comparison model.
            // > OR split the match on each conditional.
            fall_back_on_simple = true;
        }
        else
        {
            ac.has_condition = false;
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
}

// --------------------------------------------------------------------
// Dumb and Simple
// --------------------------------------------------------------------
int MIR_LowerHIR_Match_Simple__GeneratePattern(MirBuilder& builder, const Span& sp, const PatternRule* rules, unsigned int num_rules, const ::HIR::TypeRef& ty, const ::MIR::LValue& match_val,  ::MIR::BasicBlockId fail_bb);

void MIR_LowerHIR_Match_Simple( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arms_code, ::MIR::BasicBlockId first_cmp_block )
{
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
            const auto& pat = arm.m_patterns[i];
            bool is_last_pat = (i+1 == arm.m_patterns.size());
            auto next_pattern_bb = (!is_last_pat ? builder.new_bb_unlinked() : next_arm_bb);
            
            // 1. Check
            MIR_LowerHIR_Match_Simple__GeneratePattern(builder, arm.m_code->span(), pat_rule.m_rules.data(), pat_rule.m_rules.size(), node.m_value->m_res_type, match_val, next_pattern_bb);
            // 2. Destructure
            conv.destructure_from( arm.m_code->span(), pat, match_val.clone(), true );
            
            // - Go to code/condition check
            if( arm_code.has_condition )
            {
                builder.end_block( ::MIR::Terminator::make_Goto(arm_code.cond_code) );
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
            builder.set_cur_block( arm_code.cond_code );
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
            TODO(sp, "Simple match over primitive");
        }
        return 1;
        ),
    (Path,
        // TODO
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
    };
    
    TAGGED_UNION( Values, Unset,
        (Unset, struct {}),
        (Bool, struct { Branch false_branch, true_branch; }),
        (Variant, ::std::vector< ::std::pair<unsigned int, Branch> >),
        (Unsigned, ::std::vector< ::std::pair< Range<uint64_t>, Branch> >),
        (Signed, ::std::vector< ::std::pair< Range<int64_t>, Branch> >)
        //(String, struct { branchset_t< ::std::string>   branches; })
        );
    
    bool is_specialisation;
    Values  m_branches;
    Branch  m_default;
    
    DecisionTreeNode():
        is_specialisation(false)
    {}
    
    static Branch clone(const Branch& b);
    static Values clone(const Values& x);
    DecisionTreeNode clone() const;
    
    void populate_tree_from_rule(const Span& sp, unsigned int arm_index, const PatternRule* first_rule, unsigned int rule_count) {
        populate_tree_from_rule(sp, first_rule, rule_count, [arm_index](auto& branch){ branch = Branch::make_Terminal(arm_index); });
    }
    // `and_then` - Closure called after processing the final rule
    void populate_tree_from_rule(const Span& sp, const PatternRule* first_rule, unsigned int rule_count, ::std::function<void(Branch&)> and_then);
    
    /// Simplifies the tree by eliminating nodes with just a default
    void simplify();
    /// Propagate the m_default arm's contents to value arms, and vice-versa
    void propagate_default();
    /// HELPER: Unfies the rules from the provided branch with this node
    void unify_from(const Branch& b);
    
    
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
    
    void populate_tree_vals(const Span& sp, const DecisionTreeNode& node, const ::HIR::TypeRef& ty, const ::MIR::LValue& val) {
        populate_tree_vals(sp, node, ty, 0, val, [](const auto& n){ DEBUG("final node = " << n); });
    }
    void populate_tree_vals(
        const Span& sp,
        const DecisionTreeNode& node,
        const ::HIR::TypeRef& ty, unsigned int ty_ofs, const ::MIR::LValue& val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
    
    void generate_branch(const DecisionTreeNode::Branch& branch, ::MIR::BasicBlockId bb, ::std::function<void(const DecisionTreeNode&)> cb);
    
    void generate_branches_Unsigned(
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
        const ::MIR::LValue& val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
};

void MIR_LowerHIR_Match_DecisionTree( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules, ::std::vector<ArmCode> arms_code, ::MIR::BasicBlockId first_cmp_block )
{
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
        rule_blocks.push_back( builder.new_bb_unlinked() );
        builder.set_cur_block(rule_blocks.back());
        
        const auto& arm = node.m_arms[ rule.arm_idx ];
        const auto& pat = arm.m_patterns[rule.pat_idx];
        
        // Assign bindings (drop registration happens in previous loop) - Allow refutable patterns
        conv.destructure_from( arm.m_code->span(), pat, match_val.clone(), true );
        
        ASSERT_BUG(node.span(), ! arms_code[rule.arm_idx].has_condition, "Decision tree doesn't (yet) support conditionals");
        builder.end_block( ::MIR::Terminator::make_Goto( arms_code[rule.arm_idx].code ) );
    }
    
    
    // - Build tree by running each arm's pattern across it
    DEBUG("- Building decision tree");
    DecisionTreeNode    root_node;
    for( const auto& arm_rule : arm_rules )
    {
        auto arm_idx = arm_rule.arm_idx;
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
    gen.populate_tree_vals( node.span(), root_node, node.m_value->m_res_type, mv$(match_val) );
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


void PatternRulesetBuilder::append_from(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::TypeRef& ty)
{
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
            m_rules.push_back( PatternRule::make_Any({}) );
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
                m_rules.push_back( PatternRule::make_ValueRange( {::MIR::Constant(start), ::MIR::Constant(end)} ) );
                } break;
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::Isize: {
                int64_t start = H::get_pattern_value_int(sp, pat, pe.start);
                int64_t end   = H::get_pattern_value_int(sp, pat, pe.end  );
                m_rules.push_back( PatternRule::make_ValueRange( {::MIR::Constant(start), ::MIR::Constant(end)} ) );
                } break;
            case ::HIR::CoreType::Bool:
                BUG(sp, "Can't range match on Bool");
                break;
            case ::HIR::CoreType::Char:
                TODO(sp, "Match value char");
                break;
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
                //m_rules.push_back( PatternRule::make_Value( ::MIR::Constant(val) ) );
                } break;
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::Usize: {
                uint64_t val = H::get_pattern_value_int(sp, pat, pe.val);
                m_rules.push_back( PatternRule::make_Value( ::MIR::Constant(val) ) );
                } break;
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::Isize: {
                int64_t val = H::get_pattern_value_int(sp, pat, pe.val);
                m_rules.push_back( PatternRule::make_Value( ::MIR::Constant(val) ) );
                } break;
            case ::HIR::CoreType::Bool:
                // TODO: Support values from `const` too
                m_rules.push_back( PatternRule::make_Bool( pe.val.as_Integer().value != 0 ) );
                break;
            case ::HIR::CoreType::Char:
                TODO(sp, "Match value char");
                break;
            case ::HIR::CoreType::Str:
                BUG(sp, "Hit match over `str` - must be `&str`");
                break;
            }
            )
        )
        ),
    (Tuple,
        TU_MATCH_DEF(::HIR::Pattern::Data, (pat.m_data), (pe),
        ( BUG(sp, "Matching tuple with invalid pattern - " << pat); ),
        (Any,
            for(const auto& sty : e)
                this->append_from(sp, pat, sty);
            ),
        (Tuple,
            assert(e.size() == pe.sub_patterns.size());
            for(unsigned int i = 0; i < e.size(); i ++)
                this->append_from(sp, pe.sub_patterns[i], e[i]);
            )
        )
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
                m_rules.push_back( PatternRule::make_Any({}) );
                )
            )
            ),
        (Struct,
            //auto monomorph_cb = [&](const auto& ty)->const auto& {
            //    const auto& ge = ty.m_data.as_Generic();
            //    if( ge.
            //    };
            auto monomorph = [&](const auto& ty) { return monomorphise_type(sp, pbe->m_params, e.path.m_data.as_Generic().m_params, ty); };
            const auto& str_data = pbe->m_data;
            TU_MATCHA( (str_data), (sd),
            (Unit,
                TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
                ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
                (Any,
                    // Nothing.
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
                    ),
                (StructTupleWildcard,
                    TODO(sp, "Match over struct - Tuple + StructTupleWildcard");
                    )
                )
                ),
            (Named,
                // TODO: Avoid needing to clone everything.
                ::std::vector< ::HIR::TypeRef>  types;
                types.reserve( sd.size() );
                for( const auto& fld : sd ) {
                    types.push_back( monomorph(fld.second.ent) );
                }
                
                TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
                ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
                (Any,
                    for(const auto& sty : types)
                        this->append_from(sp, pat, sty);
                    ),
                (Struct,
                    TODO(sp, "Match over struct - Named + Struct");
                    )
                )
                )
            )
            ),
        (Enum,
            TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
            ( BUG(sp, "Match not allowed, " << ty <<  " with " << pat); ),
            (Any,
                m_rules.push_back( PatternRule::make_Any({}) );
                ),
            (EnumValue,
                m_rules.push_back( PatternRule::make_Variant( {pe.binding_idx, {} } ) );
                ),
            (EnumTuple,
                const auto& fields_def = pe.binding_ptr->m_variants[pe.binding_idx].second.as_Tuple();
                PatternRulesetBuilder   sub_builder;
                for( unsigned int i = 0; i < pe.sub_patterns.size(); i ++ )
                {
                    const auto& subpat = pe.sub_patterns[i];
                    auto subty = monomorphise_type(sp,  pe.binding_ptr->m_params, e.path.m_data.as_Generic().m_params,  fields_def[i].ent);
                    sub_builder.append_from( sp, subpat, subty );
                }
                m_rules.push_back( PatternRule::make_Variant({ pe.binding_idx, mv$(sub_builder.m_rules) }) );
                ),
            (EnumTupleWildcard,
                m_rules.push_back( PatternRule::make_Variant({ pe.binding_idx, {} }) );
                ),
            (EnumStruct,
                PatternRulesetBuilder   sub_builder;
                TODO(sp, "Convert EnumStruct patterns");
                m_rules.push_back( PatternRule::make_Variant({ pe.binding_idx, mv$(sub_builder.m_rules) }) );
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
            m_rules.push_back( PatternRule::make_Any({}) );
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
        TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (pe),
        ( BUG(sp, "Matching borrow invalid pattern - " << pat); ),
        (Any,
            this->append_from( sp, pat, *e.inner );
            ),
        (Ref,
            this->append_from( sp, *pe.sub, *e.inner );
            )
        )
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
        )
    )
    throw "";
}
DecisionTreeNode DecisionTreeNode::clone() const {
    DecisionTreeNode    rv;
    rv.is_specialisation = is_specialisation;
    rv.m_branches = clone(m_branches);
    rv.m_default = clone(m_default);
    return rv;
}
void DecisionTreeNode::populate_tree_from_rule(const Span& sp, const PatternRule* first_rule, unsigned int rule_count, ::std::function<void(Branch&)> and_then)
{
    assert( rule_count > 0 );
    const auto& rule = *first_rule;
    
    TU_MATCHA( (rule), (e),
    (Any, {
        if( m_default.is_Unset() ) {
            if( rule_count == 1 ) {
                and_then(m_default);
            }
            else {
                auto be = box$(DecisionTreeNode());
                be->populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
                m_default = Branch(mv$(be));
            }
        }
        else TU_IFLET( Branch, m_default, Subtree, be,
            assert( be );
            assert(rule_count > 1);
            be->populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
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
            it = be.insert(it, ::std::make_pair(e.idx, Branch( box$(DecisionTreeNode()) )));
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
                ASSERT_BUG(sp, branch.is_Unset(), "Duplicate terminator");
                branch = Branch( box$(DecisionTreeNode()) );
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
            branch = Branch( box$( DecisionTreeNode() ) );
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
            auto it = ::std::find_if(be.begin(), be.end(), [&](const auto& v){ return v.first.start <= ve; });
            if( it == be.end() || it->first.end < ve ) {
                it = be.insert( it, ::std::make_pair( Range<uint64_t> { ve,ve }, Branch( box$(DecisionTreeNode()) ) ) );
            }
            else {
                // Collide or overlap!
                TODO(sp, "Value patterns - Uint - Overlapping");
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
            TODO(sp, "Value patterns - StaticString");
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
            auto it = ::std::find_if(be.begin(), be.end(), [&](const auto& v){ return v.first.start <= ve_end; });
            if( it == be.end() || it->first.end < ve_start ) {
                it = be.insert( it, ::std::make_pair( Range<uint64_t> { ve_start,ve_end }, Branch( box$(DecisionTreeNode()) ) ) );
            }
            else {
                // Collide or overlap!
                TODO(sp, "ValueRange patterns - Uint - Overlapping");
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
            TODO(sp, "Value patterns - StaticString");
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
    os << "DTN { ";
    TU_MATCHA( (x.m_branches), (e),
    (Unset,
        os << "!, ";
        ),
    (Bool,
        os << "false = " << e.false_branch << ", true = " << e.true_branch << ", ";
        ),
    (Variant,
        for(const auto& branch : e) {
            os << branch.first << " = " << branch.second << ", ";
        }
        ),
    (Unsigned,
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
        )
    )
    
    os << "* = " << x.m_default;
    os << " }";
    return os;
}


// ----------------------------
// DecisionTreeGen
// ----------------------------
void DecisionTreeGen::populate_tree_vals(
    const Span& sp,
    const DecisionTreeNode& node,
    const ::HIR::TypeRef& ty, unsigned int ty_ofs, const ::MIR::LValue& val,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    struct H {
    };
    
    TRACE_FUNCTION_F("ty=" << ty << ", ty_ofs=" << ty_ofs << ", node=" << node);
    
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
            m_builder.end_block( ::MIR::Terminator::make_If({ val.clone(), bb_true, bb_false }) );
            
            // Recurse into sub-patterns
            const auto& branch_false = ( !branches.false_branch.is_Unset() ? branches.false_branch : node.m_default );
            const auto& branch_true  = ( !branches. true_branch.is_Unset() ? branches. true_branch : node.m_default );
            
            this->generate_branch(branch_true , bb_true , and_then);
            this->generate_branch(branch_false, bb_false, and_then);
            
            } break;
        case ::HIR::CoreType::U8:
        case ::HIR::CoreType::U16:
        case ::HIR::CoreType::U32:
        case ::HIR::CoreType::U64:
        case ::HIR::CoreType::Usize:
            ASSERT_BUG(sp, node.m_branches.is_Unsigned(), "Tree for unsigned isn't a _Unsigned - node="<<node);
            this->generate_branches_Unsigned(sp, node.m_default, node.m_branches.as_Unsigned(), ty, val, mv$(and_then));
            break;
        default:
            TODO(sp, "Primitive - " << ty);
            break;
        }
        ),
    (Tuple,
        // Tuple - Recurse on each sub-type (increasing the index)
        if( ty_ofs == e.size() ) {
            and_then(node);
        }
        else {
            populate_tree_vals( sp, node,
                e[ty_ofs], 0, ::MIR::LValue::make_Field({ box$(val.clone()), ty_ofs}),
                [&](auto& n){ this->populate_tree_vals(sp, n, ty, ty_ofs+1, val, and_then); }
                );
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
            TODO(sp, "Match over struct - " << e.path);
            ),
        (Enum,
            ASSERT_BUG(sp, node.m_branches.is_Variant(), "Tree for enum isn't a Variant - node="<<node);
            assert(pbe);
            this->generate_branches_Enum(sp, node.m_default, node.m_branches.as_Variant(), ty, val, mv$(and_then));
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
        populate_tree_vals( sp, node,
            *e.inner, 0, ::MIR::LValue::make_Deref({ box$(val.clone()) }),
            and_then
            );
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

void DecisionTreeGen::generate_branch(const DecisionTreeNode::Branch& branch, ::MIR::BasicBlockId bb, ::std::function<void(const DecisionTreeNode&)> cb)
{
    this->m_builder.set_cur_block(bb);
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
        
        auto val_start = m_builder.lvalue_or_temp(ty, ::MIR::Constant(branch.first.start));
        auto val_end = (branch.first.end == branch.first.start ? val_start.clone() : m_builder.lvalue_or_temp(ty, ::MIR::Constant(branch.first.end)));
        
        auto cmp_gt_block = m_builder.new_bb_unlinked();
        auto val_cmp_lt = m_builder.lvalue_or_temp( ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
            val.clone(), ::MIR::eBinOp::LT, mv$(val_start)
            }) );
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_lt), default_block, cmp_gt_block }) );
        m_builder.set_cur_block( cmp_gt_block );
        auto success_block = m_builder.new_bb_unlinked();
        auto val_cmp_gt = m_builder.lvalue_or_temp( ::HIR::TypeRef(::HIR::CoreType::Bool), ::MIR::RValue::make_BinOp({
            val.clone(), ::MIR::eBinOp::GT, mv$(val_end)
            }) );
        m_builder.end_block( ::MIR::Terminator::make_If({ mv$(val_cmp_gt), next_block, success_block }) );
        
        this->generate_branch(branch.second, success_block, and_then);
        
        m_builder.set_cur_block( next_block );
    }
    assert( m_builder.block_active() );
    
    // TODO: default_branch
    TU_MATCHA( (default_branch), (be),
    (Unset,
        m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
        ),
    (Terminal,
        m_builder.end_block( ::MIR::Terminator::make_Goto( this->get_block_for_rule( be ) ) );
        ),
    (Subtree,
        and_then( *be );
        )
    )
}
void DecisionTreeGen::generate_branches_Enum(
    const Span& sp,
    const DecisionTreeNode::Branch& default_branch,
    const DecisionTreeNode::Values::Data_Variant& branches,
    const ::HIR::TypeRef& ty,/* unsigned int _ty_ofs,*/
    const ::MIR::LValue& val,
    ::std::function<void(const DecisionTreeNode&)> and_then
    )
{
    const auto& enum_ref = *ty.m_data.as_Path().binding.as_Enum();
    const auto& enum_path = ty.m_data.as_Path().path.m_data.as_Generic();
    const auto& variants = enum_ref.m_variants;
    auto variant_count = variants.size();
    bool has_any = ! default_branch.is_Unset();
    
    if( branches.size() < variant_count && ! has_any ) {
        ERROR(sp, E0000, "Non-exhaustive match");
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
        DEBUG(branch.first << " " << var.first << " = " << branch);
        this->generate_branch(branch.second, bb, [&](auto& subnode) {
            TU_MATCHA( (var.second), (e),
            (Unit,
                and_then( subnode );
                ),
            (Value,
                and_then( subnode );
                ),
            (Tuple,
                // Make a fake tuple
                ::std::vector< ::HIR::TypeRef>  ents;
                for( const auto& fld : e )
                {
                    ents.push_back( monomorphise_type(sp,  enum_ref.m_params, enum_path.m_params,  fld.ent) );
                }
                ::HIR::TypeRef  fake_ty { mv$(ents) };
                this->populate_tree_vals(sp, subnode, fake_ty, 0, ::MIR::LValue::make_Downcast({ box$(val.clone()), branch.first }), and_then);
                ),
            (Struct,
                TODO(sp, "Enum pattern - struct");
                )
            )
            });
    }
    
    DEBUG("_");
    TU_MATCHA( (default_branch), (be),
    (Unset, ),
    (Terminal,
        m_builder.set_cur_block(any_block);
        m_builder.end_block( ::MIR::Terminator::make_Goto( this->get_block_for_rule( be ) ) );
        ),
    (Subtree,
        m_builder.set_cur_block(any_block);
        and_then( *be );
        )
    )
}
