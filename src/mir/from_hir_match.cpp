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
    ::std::vector<PatternRule>  m_rules;
    
    static ::Ordering rule_is_before(const PatternRule& l, const PatternRule& r);
    
    bool is_before(const PatternRuleset& other) const;
};

typedef ::std::vector< ::std::pair< ::std::pair<unsigned int,unsigned int>, PatternRuleset > >  t_arm_rules;

void MIR_LowerHIR_Match_Simple( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules );
void MIR_LowerHIR_Match_DecisionTree( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules );
/// Helper to construct rules from a passed pattern
struct PatternRulesetBuilder
{
    ::std::vector<PatternRule>  m_rules;
    void append_from(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::TypeRef& ty);
    
    PatternRuleset into_ruleset() {
        return PatternRuleset { mv$(this->m_rules) };
    }
};

// --------------------------------------------------------------------
// CODE
// --------------------------------------------------------------------

// Handles lowering non-trivial matches to MIR
// - Non-trivial means that there's more than one pattern
void MIR_LowerHIR_Match( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val )
{
    bool fall_back_on_simple = false;
    
    // 1. Build up a sorted vector of MIR pattern rules
    // Map of arm index to ruleset
    t_arm_rules arm_rules;
    for(unsigned int arm_idx = 0; arm_idx < node.m_arms.size(); arm_idx ++)
    {
        const auto& arm = node.m_arms[arm_idx];
        unsigned int pat_idx = 0;
        for( const auto& pat : arm.m_patterns )
        {
            auto builder = PatternRulesetBuilder {};
            builder.append_from(node.span(), pat, node.m_value->m_res_type);
            arm_rules.push_back( ::std::make_pair( ::std::make_pair(arm_idx, pat_idx), builder.into_ruleset()) );
            pat_idx += 1;
        }
        
        if( arm.m_cond ) {
            // TODO: What to do with contidionals?
            // > Could fall back on a sequential comparison model.
            // > OR split the match on each conditional.
            fall_back_on_simple = true;
        }
    }
    // TODO: Detect if a rule is ordering-dependent. In this case we currently have to fall back on the simple match code

    if( fall_back_on_simple ) {
        MIR_LowerHIR_Match_Simple( builder, conv, node, mv$(match_val), mv$(arm_rules) );
    }
    else {
        MIR_LowerHIR_Match_DecisionTree( builder, conv, node, mv$(match_val), mv$(arm_rules) );
    }
}

// --------------------------------------------------------------------
// Dumb and Simple
// --------------------------------------------------------------------
void MIR_LowerHIR_Match_Simple__GeneratePattern( MirBuilder& builder, const Span& sp, const ::HIR::Pattern& pat, const ::HIR::TypeRef& ty, const ::MIR::LValue& match_val,  ::MIR::BasicBlockId fail_tgt);

void MIR_LowerHIR_Match_Simple( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules )
{
    auto result_val = builder.new_temporary( node.m_res_type );
    auto next_block = builder.new_bb_unlinked();
    
    ::std::vector< ::MIR::BasicBlockId> arm_cmp_blocks;
    arm_cmp_blocks.push_back( builder.new_bb_unlinked() );
    builder.end_block( ::MIR::Terminator::make_Goto( arm_cmp_blocks.back() ) );
    
    // 1. Generate code with conditions
    ::std::vector< ::MIR::BasicBlockId> arm_code_blocks;
    for( /*const*/ auto& arm : node.m_arms )
    {
        //auto cur_arm_block = arm_cmp_blocks.back();
        arm_cmp_blocks.push_back( builder.new_bb_unlinked() );
        auto next_arm_block = arm_cmp_blocks.back();
        
        // TODO: Register introduced bindings to be dropped on return/diverge within this scope
        
        arm_code_blocks.push_back( builder.new_bb_unlinked() );
        builder.set_cur_block( arm_code_blocks.back() );
        
        if(arm.m_cond) {
            auto code_block = builder.new_bb_unlinked();
            
            conv.visit_node_ptr( arm.m_cond );
            auto cnd_lval = builder.lvalue_or_temp( ::HIR::TypeRef(::HIR::CoreType::Bool), builder.get_result(arm.m_cond->span()) );
            
            builder.end_block( ::MIR::Terminator::make_If({ mv$(cnd_lval), code_block, next_arm_block }) );
            builder.set_cur_block( code_block );
        }
        
        conv.visit_node_ptr( arm.m_code );
        
        if( !builder.block_active() && !builder.has_result() ) {
            // Nothing need be done, as the block diverged.
        }
        else {
            builder.push_stmt_assign( result_val.clone(), builder.get_result(arm.m_code->span()) );
            builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
        }
    }
    
    // 2. Generate pattern matches
    builder.set_cur_block( arm_cmp_blocks[0] );
    for( unsigned int arm_idx = 0; arm_idx < node.m_arms.size(); arm_idx ++ )
    {
        const auto& arm = node.m_arms[arm_idx];
        auto code_bb = arm_code_blocks[arm_idx];
        
        for( unsigned int i = 0; i < arm.m_patterns.size(); i ++ )
        {
            const auto& pat = arm.m_patterns[i];
            auto next_pattern_bb = (i+1 < arm.m_patterns.size() ? builder.new_bb_unlinked() : arm_cmp_blocks[1+arm_idx]);
            
            // 1. Check
            MIR_LowerHIR_Match_Simple__GeneratePattern(builder, arm.m_code->span(), pat, node.m_value->m_res_type, match_val, next_pattern_bb);
            // 2. Destructure
            conv.destructure_from( arm.m_code->span(), pat, match_val.clone(), true );
            // - Go to code
            builder.end_block( ::MIR::Terminator::make_Goto(code_bb) );
            
            builder.set_cur_block( next_pattern_bb );
        }
    }
    // - Kill the final pattern block (which is dead code)
    builder.end_block( ::MIR::Terminator::make_Diverge({}) );
    
    builder.set_cur_block( next_block );
    builder.set_result( node.span(), mv$(result_val) );
}
void MIR_LowerHIR_Match_Simple__GeneratePattern( MirBuilder& builder, const Span& sp, const ::HIR::Pattern& pat, const ::HIR::TypeRef& ty, const ::MIR::LValue& match_val,  ::MIR::BasicBlockId fail_bb)
{
    TU_MATCHA( (pat.m_data), (pe),
    (Any,
        ),
    (Box,
        TODO(sp, "box patterns");
        ),
    (Ref,
        auto lval = ::MIR::LValue::make_Deref({ box$(match_val.clone()) });
        MIR_LowerHIR_Match_Simple__GeneratePattern(builder, sp, *pe.sub, *ty.m_data.as_Borrow().inner, mv$(lval), fail_bb);
        ),
    (Tuple,
        const auto& te = ty.m_data.as_Tuple();
        assert( te.size() == pe.sub_patterns.size() );
        for(unsigned int idx = 0; idx < pe.sub_patterns.size(); idx ++ )
        {
            auto lval = ::MIR::LValue::make_Field({ box$(match_val.clone()), idx });
            MIR_LowerHIR_Match_Simple__GeneratePattern(builder, sp, pe.sub_patterns[idx], te[idx], mv$(lval), fail_bb);
        }
        ),
    (StructTuple,
        TODO(sp, "StructTuple");
        ),
    (StructTupleWildcard,
        TODO(sp, "StructTupleWildcard");
        ),
    (Struct,
        TODO(sp, "Struct");
        ),
    (Value,
        TODO(sp, "Value");
        ),
    (Range,
        TODO(sp, "Range");
        ),
    (EnumValue,
        TODO(sp, "EnumValue");
        ),
    (EnumTuple,
        // Switch (or if) on the variant, then recurse
        auto next_bb = builder.new_bb_unlinked();
        auto var_count = pe.binding_ptr->m_variants.size();
        auto var_idx = pe.binding_idx;
        ::std::vector< ::MIR::BasicBlockId> arms(var_count, fail_bb);
        arms[var_idx] = next_bb;
        builder.end_block( ::MIR::Terminator::make_Switch({ match_val.clone(), mv$(arms) }) );
        
        builder.set_cur_block(next_bb);
        TU_MATCHA( (pe.binding_ptr->m_variants[var_idx].second), (var),
        (Unit,
            BUG(sp, "EnumTuple pattern with Unit enum variant");
            ),
        (Value,
            BUG(sp, "EnumTuple pattern with Value enum variant");
            ),
        (Tuple,
            auto lval_var = ::MIR::LValue::make_Downcast({ box$(match_val.clone()), var_idx });
            assert( pe.sub_patterns.size() == var.size() );
            for(unsigned int i = 0; i < var.size(); i ++)
            {
                auto lval = ::MIR::LValue::make_Field({ box$(lval_var.clone()), i });
                const auto& ty = var[i].ent;
                const auto& pat = pe.sub_patterns[i];
                if( monomorphise_type_needed(ty) ) {
                    auto ty_mono = monomorphise_type(sp, pe.binding_ptr->m_params, pe.path.m_params, ty);
                    MIR_LowerHIR_Match_Simple__GeneratePattern(builder, sp, pat, ty_mono, mv$(lval), fail_bb);
                }
                else {
                    MIR_LowerHIR_Match_Simple__GeneratePattern(builder, sp, pat, ty, mv$(lval), fail_bb);
                }
            }
            ),
        (Struct,
            BUG(sp, "EnumTuple pattern with Named enum variant");
            )
        )
        ),
    (EnumTupleWildcard,
        TODO(sp, "EnumTupleWildcard");
        ),
    (EnumStruct,
        TODO(sp, "EnumStruct");
        ),
    (Slice,
        TODO(sp, "Slice");
        ),
    (SplitSlice,
        TODO(sp, "SplitSlice");
        )
    )
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
        const DecisionTreeNode::Values::Data_Unsigned& branches,
        const ::HIR::TypeRef& ty,/* unsigned int _ty_ofs,*/
        const ::MIR::LValue& val,
        ::std::function<void(const DecisionTreeNode&)> and_then
        );
};

void MIR_LowerHIR_Match_DecisionTree( MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val, t_arm_rules arm_rules )
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
    
    
    // Generate arm code
    DEBUG("- Generating arm code");
    // - End the current block with a jump to the descision code
    //  > TODO: Can this goto be avoided while still being defensive? (Avoiding incomplete blocks)
    auto descision_block = builder.new_bb_unlinked();
    builder.end_block( ::MIR::Terminator::make_Goto(descision_block) );
    
    // - Create a result and next block
    auto result_val = builder.new_temporary(node.m_res_type);
    auto next_block = builder.new_bb_unlinked();
    
    // - Generate code for arms.
    ::std::vector< ::MIR::BasicBlockId> arm_blocks;
    arm_blocks.reserve( arm_rules.size() );
    for(auto& arm : node.m_arms)
    {
        // TODO: Register introduced bindings to be dropped on return/diverge
        arm_blocks.push_back( builder.new_bb_unlinked() );
        builder.set_cur_block(arm_blocks.back());
        
        conv.visit_node_ptr(arm.m_code);
        
        if( !builder.block_active() && !builder.has_result() ) {
            // Nothing need be done, as the block diverged.
        }
        else {
            builder.push_stmt_assign( result_val.clone(), builder.get_result(arm.m_code->span()) );
            builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
        }
    }
    
    DEBUG("- Generating rule bindings");
    ::std::vector< ::MIR::BasicBlockId> rule_blocks;
    for(const auto& rule : arm_rules)
    {
        rule_blocks.push_back( builder.new_bb_unlinked() );
        builder.set_cur_block(arm_blocks.back());
        
        const auto& arm = node.m_arms[ rule.first.first ];
        const auto& pat = arm.m_patterns[rule.first.second];
        
        // Assign bindings (drop registration happens in previous loop) - Allow refutable patterns
        conv.destructure_from( arm.m_code->span(), pat, match_val.clone(), 1 );
        
        builder.end_block( ::MIR::Terminator::make_Goto( arm_blocks[rule.first.first] ) );
    }
    
    
    // - Build tree by running each arm's pattern across it
    DEBUG("- Building decision tree");
    DecisionTreeNode    root_node;
    for( const auto& arm_rule : arm_rules )
    {
        auto arm_idx = arm_rule.first.first;
        root_node.populate_tree_from_rule( node.m_arms[arm_idx].m_code->span(), arm_idx, arm_rule.second.m_rules.data(), arm_rule.second.m_rules.size() );
    }
    DEBUG("root_node = " << root_node);
    root_node.simplify();
    root_node.propagate_default();
    DEBUG("root_node = " << root_node);
    
    // - Convert the above decision tree into MIR
    DEBUG("- Emitting decision tree");
    DecisionTreeGen gen { builder, rule_blocks };
    builder.set_cur_block( descision_block );
    gen.populate_tree_vals( node.span(), root_node, node.m_value->m_res_type, mv$(match_val) );
    
    builder.set_cur_block(next_block);
    builder.set_result( node.span(), mv$(result_val) );
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
    TU_MATCHA( (ty.m_data), (e),
    (Infer,   BUG(sp, "Ivar for in match type"); ),
    (Diverge, BUG(sp, "Diverge in match type");  ),
    (Primitive,
        TU_MATCH_DEF(::HIR::Pattern::Data, (pat.m_data), (pe),
        ( throw ""; ),
        (Any,
            m_rules.push_back( PatternRule::make_Any({}) );
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
                uint64_t val = pe.val.as_Integer().value;
                m_rules.push_back( PatternRule::make_Value( ::MIR::Constant(val) ) );
                } break;
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::Isize: {
                int64_t val = pe.val.as_Integer().value;
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
        ( throw ""; ),
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
            ( throw ""; ),
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
        ( throw ""; ),
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
        TODO(sp, "ValueRange patterns");
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
            this->generate_branches_Unsigned(sp, node.m_branches.as_Unsigned(), ty, val, mv$(and_then));
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
            const auto& enum_path = e.path.m_data.as_Generic();
            ASSERT_BUG(sp, node.m_branches.is_Variant(), "Tree for enum isn't a Variant - node="<<node);
            const auto& branches = node.m_branches.as_Variant();
            const auto& variants = pbe->m_variants;
            auto variant_count = pbe->m_variants.size();
            bool has_any = ! node.m_default.is_Unset();
            
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
                            ents.push_back( monomorphise_type(sp,  pbe->m_params, enum_path.m_params,  fld.ent) );
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
            TU_MATCHA( (node.m_default), (be),
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
        auto next_block = m_builder.new_bb_unlinked();
        
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
}
