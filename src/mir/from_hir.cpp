/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/from_hir.cpp
 * - Construction of MIR from the HIR expression tree
 */
#include <type_traits>  // for TU_MATCHA
#include <algorithm>
#include "mir.hpp"
#include "mir_ptr.hpp"
#include <hir/expr.hpp>
#include <hir/hir.hpp>
#include <hir/visitor.hpp>
#include <hir_typeck/helpers.hpp>   // monomorphise_type
#include "main_bindings.hpp"

namespace {
    class MirBuilder
    {
        ::MIR::Function&    m_output;
        
        unsigned int    m_current_block;
        bool    m_block_active;
        
        ::MIR::RValue   m_result;
        bool    m_result_valid;
    public:
        MirBuilder(::MIR::Function& output):
            m_output(output),
            m_block_active(false),
            m_result_valid(false)
        {
            set_cur_block( new_bb_unlinked() );
        }
        
        ::MIR::LValue new_temporary(const ::HIR::TypeRef& ty)
        {
            unsigned int rv = m_output.temporaries.size();
            m_output.temporaries.push_back( ty.clone() );
            return ::MIR::LValue::make_Temporary({rv});
        }
        ::MIR::LValue lvalue_or_temp(const ::HIR::TypeRef& ty, ::MIR::RValue val)
        {
            TU_IFLET(::MIR::RValue, val, Use, e,
                return mv$(e);
            )
            else {
                auto temp = new_temporary(ty);
                push_stmt_assign( ::MIR::LValue(temp.as_Temporary()), mv$(val) );
                return temp;
            }
        }
        
        ::MIR::RValue get_result(const Span& sp)
        {
            if(!m_result_valid) {
                BUG(sp, "No value avaliable");
            }
            auto rv = mv$(m_result);
            m_result_valid = false;
            return rv;
        }
        ::MIR::LValue get_result_lvalue(const Span& sp)
        {
            auto rv = get_result(sp);
            TU_IFLET(::MIR::RValue, rv, Use, e,
                return mv$(e);
            )
            else {
                BUG(sp, "LValue expected, got RValue");
            }
        }
        void set_result(const Span& sp, ::MIR::RValue val) {
            if(m_result_valid) {
                BUG(sp, "Pushing a result over an existing result");
            }
            m_result = mv$(val);
            m_result_valid = true;
        }
        bool has_result() const {
            return m_result_valid;
        }
        
        void push_stmt_assign(::MIR::LValue dst, ::MIR::RValue val)
        {
            ASSERT_BUG(Span(), m_block_active, "Pushing statement with no active block");
            m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Assign({ mv$(dst), mv$(val) }) );
        }
        void push_stmt_drop(::MIR::LValue val)
        {
            ASSERT_BUG(Span(), m_block_active, "Pushing statement with no active block");
            m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Drop({ ::MIR::eDropKind::DEEP, mv$(val) }) );
        }
        
        bool block_active() const {
            return m_block_active;
        }
        void end_block(::MIR::Terminator term)
        {
            if( !m_block_active ) {
                BUG(Span(), "Terminating block when none active");
            }
            m_output.blocks.at(m_current_block).terminator = mv$(term);
            m_block_active = false;
            m_current_block = 0;
        }
        void set_cur_block(unsigned int new_block)
        {
            if( m_block_active ) {
                BUG(Span(), "Updating block when previous is active");
            }
            m_current_block = new_block;
            m_block_active = true;
        }
        ::MIR::BasicBlockId new_bb_linked()
        {
            auto rv = new_bb_unlinked();
            end_block( ::MIR::Terminator::make_Goto(rv) );
            set_cur_block(rv);
            return rv;
        }
        ::MIR::BasicBlockId new_bb_unlinked()
        {
            auto rv = m_output.blocks.size();
            m_output.blocks.push_back({});
            return rv;
        }
    };
    
    class ExprVisitor_Conv:
        public ::HIR::ExprVisitor
    {
        MirBuilder  m_builder;
        
        struct LoopDesc {
            ::std::string   label;
            unsigned int    cur;
            unsigned int    next;
        };
        ::std::vector<LoopDesc> m_loop_stack;
        struct BlockDesc {
            ::std::vector<unsigned int> bindings;
        };
        ::std::vector<BlockDesc>    m_block_stack;
        
    public:
        ExprVisitor_Conv(::MIR::Function& output):
            m_builder(output)
        {
        }
        
        
        void destructure_from(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval)
        {
            if( pat.m_binding.is_valid() ) {
                ASSERT_BUG(sp, pat.m_data.is_Any(), "Destructure patterns can't bind and match");
                
                m_builder.push_stmt_assign( ::MIR::LValue::make_Variable(pat.m_binding.m_slot), mv$(lval) );
                return ;
            }
            // TODO: Destructure
            TODO(sp, "Destructure using " << pat);
        }
        
        // -- ExprVisitor
        void visit(::HIR::ExprNode_Block& node) override
        {
            TRACE_FUNCTION_F("_Block");
            // NOTE: This doesn't create a BB, as BBs are not needed for scoping
            if( node.m_nodes.size() > 0 )
            {
                m_block_stack.push_back( {} );
                for(unsigned int i = 0; i < node.m_nodes.size()-1; i ++)
                {
                    auto& subnode = node.m_nodes[i];
                    const Span& sp = subnode->span();
                    this->visit_node_ptr(subnode);
                    m_builder.push_stmt_drop( m_builder.lvalue_or_temp(subnode->m_res_type, m_builder.get_result(sp)) );
                }
                
                this->visit_node_ptr(node.m_nodes.back());
                auto ret = m_builder.get_result(node.m_nodes.back()->span());
                
                auto bd = mv$( m_block_stack.back() );
                m_block_stack.pop_back();
                
                // Drop all bindings introduced during this block.
                for( auto& var_idx : bd.bindings ) {
                    m_builder.push_stmt_drop( ::MIR::LValue::make_Variable(var_idx) );
                }
                
                m_builder.set_result(node.span(), mv$(ret));
            }
            else
            {
                TODO(node.span(), "Lower empty blocks");
            }
        }
        void visit(::HIR::ExprNode_Return& node) override
        {
            TRACE_FUNCTION_F("_Return");
            this->visit_node_ptr(node.m_value);
            
            m_builder.push_stmt_assign( ::MIR::LValue::make_Return({}),  m_builder.get_result(node.span()) );
            m_builder.end_block( ::MIR::Terminator::make_Return({}) );
        }
        void visit(::HIR::ExprNode_Let& node) override
        {
            TRACE_FUNCTION_F("_Let");
            if( node.m_value )
            {
                this->visit_node_ptr(node.m_value);
                
                this->destructure_from(node.span(), node.m_pattern, m_builder.lvalue_or_temp(node.m_type, m_builder.get_result(node.span()) ));
            }
            m_builder.set_result(node.span(), ::MIR::RValue::make_Tuple({}));
        }
        void visit(::HIR::ExprNode_Loop& node) override
        {
            TRACE_FUNCTION_F("_Loop");
            auto loop_block = m_builder.new_bb_linked();
            auto loop_next = m_builder.new_bb_unlinked();
            
            m_loop_stack.push_back( LoopDesc { node.m_label, loop_block, loop_next } );
            this->visit_node_ptr(node.m_code);
            m_loop_stack.pop_back();
            
            m_builder.end_block( ::MIR::Terminator::make_Goto(loop_block) );
            m_builder.set_cur_block(loop_next);
        }
        void visit(::HIR::ExprNode_LoopControl& node) override
        {
            TRACE_FUNCTION_F("_LoopControl");
            if( m_loop_stack.size() == 0 ) {
                BUG(node.span(), "Loop control outside of a loop");
            }
            
            const auto* target_block = &m_loop_stack.back();
            if( node.m_label != "" ) {
                auto it = ::std::find_if(m_loop_stack.rbegin(), m_loop_stack.rend(), [&](const auto& x){ return x.label == node.m_label; });
                if( it == m_loop_stack.rend() ) {
                    BUG(node.span(), "Named loop '" << node.m_label << " doesn't exist");
                }
                target_block = &*it;
            }
            
            if( node.m_continue ) {
                m_builder.end_block( ::MIR::Terminator::make_Goto(target_block->cur) );
            }
            else {
                m_builder.end_block( ::MIR::Terminator::make_Goto(target_block->next) );
            }
        }
        
        void visit(::HIR::ExprNode_Match& node) override
        {
            TRACE_FUNCTION_F("_Match");
            this->visit_node_ptr(node.m_value);
            auto match_val = m_builder.lvalue_or_temp(node.m_value->m_res_type, m_builder.get_result(node.m_value->span()));
            
            if( node.m_arms.size() == 0 ) {
                // Nothing
                TODO(node.span(), "Handle zero-arm match");
            }
            else if( node.m_arms.size() == 1 && node.m_arms[0].m_patterns.size() == 1 && ! node.m_arms[0].m_cond ) {
                // - Shortcut: Single-arm match
                // TODO: Drop scope
                this->destructure_from(node.span(), node.m_arms[0].m_patterns[0], mv$(match_val));
                this->visit_node_ptr(node.m_arms[0].m_code);
            }
            else {
                // TODO: Convert patterns into sequence of switches and comparisons.
                
                // Build up a sorted vector of MIR pattern rules
                TAGGED_UNION(PatternRule, Any,
                (Any, struct {}),
                (Variant, struct { unsigned int idx; ::std::vector<PatternRule> sub_rules; }),
                (Value, ::MIR::Constant)
                );
                struct PatternRuleset {
                    ::std::vector<PatternRule>  m_rules;
                    
                    static ::Ordering rule_is_before(const PatternRule& l, const PatternRule& r)
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
                        (Value,
                            TODO(Span(), "Order PatternRule::Value");
                            )
                        )
                        throw "";
                    }
                    bool is_before(const PatternRuleset& other) const
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
                };
                struct PatternRulesetBuilder {
                    ::std::vector<PatternRule>  m_rules;
                    void append_from(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::TypeRef& ty) {
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
                                case ::HIR::CoreType::F64:
                                    TODO(sp, "Match value float");
                                    break;
                                case ::HIR::CoreType::U8:
                                case ::HIR::CoreType::U16:
                                case ::HIR::CoreType::U32:
                                case ::HIR::CoreType::U64:
                                case ::HIR::CoreType::Usize:
                                    TODO(sp, "Match value unsigned");
                                    break;
                                case ::HIR::CoreType::I8:
                                case ::HIR::CoreType::I16:
                                case ::HIR::CoreType::I32:
                                case ::HIR::CoreType::I64:
                                case ::HIR::CoreType::Isize:
                                    TODO(sp, "Match value signed");
                                    break;
                                case ::HIR::CoreType::Bool:
                                    TODO(sp, "Match value bool");
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
                                TODO(sp, "Match over struct - " << e.path);
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
                    PatternRuleset into_ruleset() {
                        return PatternRuleset { mv$(this->m_rules) };
                    }
                };
                
                // Map of arm index to ruleset
                ::std::vector< ::std::pair< unsigned int, PatternRuleset > >   arm_rules;
                for(unsigned int idx = 0; idx < node.m_arms.size(); idx ++)
                {
                    const auto& arm = node.m_arms[idx];
                    for( const auto& pat : arm.m_patterns )
                    {
                        auto builder = PatternRulesetBuilder {};
                        builder.append_from(node.span(), pat, node.m_value->m_res_type);
                        arm_rules.push_back( ::std::make_pair(idx, builder.into_ruleset()) );
                    }
                    
                    if( arm.m_cond ) {
                        // - TODO: What to do with contidionals?
                        TODO(node.span(), "Handle conditional match arms (ordering matters)");
                    }
                }
                
                // Sort ruleset such that wildcards go last (if there's no conditionals)
                // NOTE: Conditionals will break this, as you can have `x if x.is_foo() => {}, Enum::Foo ...`
                ::std::sort( arm_rules.begin(), arm_rules.end(), [](const auto& l, const auto& r){ return l.second.is_before( r.second ); });
                
                // Generate arm code
                // - End the current block with a jump to the descision code (TODO: Can this be avoided while still being defensive?)
                auto descision_block = m_builder.new_bb_unlinked();
                m_builder.end_block( ::MIR::Terminator::make_Goto(descision_block) );
                
                // - Create a result and next block
                auto result_val = m_builder.new_temporary(node.m_res_type);
                auto next_block = m_builder.new_bb_unlinked();
                
                // - Generate code for arms.
                ::std::vector< ::MIR::BasicBlockId> arm_blocks;
                arm_blocks.reserve( arm_rules.size() );
                for(auto& arm : node.m_arms)
                {
                    // TODO: Register introduced bindings to be dropped on return/diverge
                    arm_blocks.push_back( m_builder.new_bb_unlinked() );
                    m_builder.set_cur_block(arm_blocks.back());
                    
                    this->visit_node_ptr(arm.m_code);
                    
                    if( !m_builder.block_active() && !m_builder.has_result() ) {
                        // Nothing need be done, as the block diverged.
                    }
                    else {
                        m_builder.push_stmt_assign( result_val.clone(), m_builder.get_result(arm.m_code->span()) );
                        m_builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
                    }
                }
                
                ::std::vector< ::MIR::BasicBlockId> rule_blocks;
                for(const auto& rule : arm_rules)
                {
                    rule_blocks.push_back( m_builder.new_bb_unlinked() );
                    m_builder.set_cur_block(arm_blocks.back());
                    
                    const auto& arm = node.m_arms[ rule.first ];
                    ASSERT_BUG(node.span(), arm.m_patterns.size() == 1, "TODO: Handle multiple patterns on one arm");
                    const auto& pat = arm.m_patterns[0 /*rule.first.second*/];
                    
                    // Assign bindings (drop registration happens in previous loop)
                    this->destructure_from( arm.m_code->span(), pat, match_val.clone() );
                }
                
                // ## Create descision tree in-memory based off the ruleset
                // > Tree contains an lvalue and a set of possibilities (PatternRule) connected to another tree or to a branch index
                struct DecisionTreeNode {
                    TAGGED_UNION( Branch, Unset,
                    (Unset, struct{}),
                    (Subtree, ::std::unique_ptr<DecisionTreeNode>),
                    (Terminal, unsigned int)
                    );
                    
                    TAGGED_UNION( Values, Unset,
                    (Unset, struct {}),
                    (Variant, ::std::vector< ::std::pair<unsigned int, Branch> >)/*,
                    (Unsigned, struct { branchset_t<uint64_t[2]>    branches; }),
                    (String, struct { branchset_t< ::std::string>   branches; })*/
                    );
                    
                    bool is_specialisation;
                    ::MIR::LValue   value;
                    Values  m_branches;
                    Branch  m_default;
                    
                    DecisionTreeNode():
                        is_specialisation(false)
                    {}
                    
                    void populate_tree_from_rule(const Span& sp, unsigned int arm_index, const PatternRule* first_rule, unsigned int rule_count)
                    {
                        populate_tree_from_rule(sp, first_rule, rule_count, [rule_count](auto& branch){ branch = Branch::make_Terminal(rule_count); });
                    }
                    // `and_then` - Closure called after processing the final rule
                    void populate_tree_from_rule(const Span& sp, const PatternRule* first_rule, unsigned int rule_count, ::std::function<void(Branch&)> and_then)
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
                            // Not found? Insert a new branch
                            if( it == be.end() || it->first != e.idx ) {
                                it = be.insert(it, ::std::make_pair(e.idx, Branch( box$(DecisionTreeNode()) )));
                                auto& subtree = *it->second.as_Subtree();
                                
                                if( e.sub_rules.size() > 0 && rule_count > 1 )
                                {
                                    subtree.populate_tree_from_rule(sp, e.sub_rules.data(), e.sub_rules.size(), [&](auto& branch){
                                        ASSERT_BUG(sp, branch.is_Unset(), "Duplicate terminator");
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
                            }
                            else {
                                assert( !it->second.is_Unset() );
                                TU_IFLET(Branch, it->second, Subtree, subtree_ptr,
                                    assert(subtree_ptr);
                                    subtree_ptr->populate_tree_from_rule(sp, first_rule+1, rule_count-1, and_then);
                                )
                                else {
                                    BUG(sp, "Duplicate terminal rule - " << it->second.as_Terminal());
                                }
                            }
                            }),
                        (Value,
                            TODO(sp, "Value patterns");
                            )
                        )
                    }
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
                        populate_tree_vals(sp, node, ty, 0, val, [](const auto&){});
                    }
                    void populate_tree_vals(
                        const Span& sp,
                        const DecisionTreeNode& node,
                        const ::HIR::TypeRef& ty, unsigned int ty_ofs, const ::MIR::LValue& val,
                        ::std::function<void(const DecisionTreeNode&)> and_then
                        )
                    {
                        TU_MATCHA( (ty.m_data), (e),
                        (Infer,   BUG(sp, "Ivar for in match type"); ),
                        (Diverge, BUG(sp, "Diverge in match type");  ),
                        (Primitive,
                            TODO(sp, "Primitive");
                            ),
                        (Tuple,
                            // Tuple - Recurse on each sub-type (increasing the index)
                            if( ty_ofs == e.size() ) {
                                and_then(node);
                            }
                            else {
                                populate_tree_vals( sp, node,
                                    e[ty_ofs], 0, ::MIR::LValue::make_Field({ box$(val.clone()), ty_ofs}),
                                    [&](auto& n){ this->populate_tree_vals(sp, node, ty, ty_ofs+1, val, and_then); }
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
                                ),
                            (Struct,
                                TODO(sp, "Match over struct - " << e.path);
                                ),
                            (Enum,
                                const auto& branches = node.m_branches.as_Variant();
                                const auto& variants = pbe->m_variants;
                                bool has_any = ! node.m_default.is_Unset();
                                
                                auto variant_count = pbe->m_variants.size();
                                if( branches.size() < variant_count && ! has_any ) {
                                    ERROR(sp, E0000, "Non-exhaustive match");
                                }
                                if( branches.size() == variant_count && has_any ) {
                                    ERROR(sp, E0000, "Unreachable _ arm");
                                }
                                
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
                                    m_builder.set_cur_block(bb);
                                    TU_MATCHA( (var.second), (e),
                                    (Unit,
                                        assert( branch.second.is_Terminal() );
                                        m_builder.end_block( ::MIR::Terminator::make_Goto( this->get_block_for_rule( branch.second.as_Terminal() ) ) );
                                        ),
                                    (Value,
                                        assert( branch.second.is_Terminal() );
                                        m_builder.end_block( ::MIR::Terminator::make_Goto( this->get_block_for_rule( branch.second.as_Terminal() ) ) );
                                        ),
                                    (Tuple,
                                        TODO(sp, "Enum pattern - tuple");
                                        ),
                                    (Struct,
                                        TODO(sp, "Enum pattern - struct");
                                        )
                                    )
                                }
                                )
                            )
                            ),
                        (Generic,
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
                };
                
                // - Build tree by running each arm's pattern across it
                DecisionTreeNode    root_node;
                for( const auto& arm_rule : arm_rules )
                {
                    root_node.populate_tree_from_rule( node.m_arms[arm_rule.first].m_code->span(), arm_rule.first, arm_rule.second.m_rules.data(), arm_rule.second.m_rules.size() );
                }
                
                // TODO: Convert the above decision tree into MIR
                TODO(node.span(), "Turn decision tree into MIR");
                
                m_builder.set_cur_block(next_block);
                m_builder.set_result( node.span(), mv$(result_val) );
            }
        } // ExprNode_Match
        
        void visit(::HIR::ExprNode_If& node) override
        {
            TRACE_FUNCTION_F("_If");
            
            this->visit_node_ptr(node.m_cond);
            auto decision_val = m_builder.lvalue_or_temp(node.m_cond->m_res_type, m_builder.get_result(node.m_cond->span()) );
            
            auto true_branch = m_builder.new_bb_unlinked();
            auto false_branch = m_builder.new_bb_unlinked();
            auto next_block = m_builder.new_bb_unlinked();
            m_builder.end_block( ::MIR::Terminator::make_If({ mv$(decision_val), true_branch, false_branch }) );
            
            auto result_val = m_builder.new_temporary(node.m_res_type);
            
            m_builder.set_cur_block(true_branch);
            this->visit_node_ptr(node.m_true);
            m_builder.push_stmt_assign( result_val.clone(), m_builder.get_result(node.m_true->span()) );
            m_builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
            
            m_builder.set_cur_block(false_branch);
            if( node.m_false )
            {
                this->visit_node_ptr(node.m_false);
                m_builder.push_stmt_assign( result_val.clone(), m_builder.get_result(node.m_false->span()) );
                m_builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
            }
            else
            {
                // Assign `()` to the result
                m_builder.push_stmt_assign( result_val.clone(), ::MIR::RValue::make_Tuple({}) );
                m_builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
            }
            m_builder.set_cur_block(next_block);
            
            m_builder.set_result( node.span(), mv$(result_val) );
        }
        
        void visit(::HIR::ExprNode_Assign& node) override
        {
            TRACE_FUNCTION_F("_Assign");
            const auto& sp = node.span();
            
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.get_result(sp);
            
            this->visit_node_ptr(node.m_slot);
            auto dst = m_builder.get_result_lvalue(sp);
            
            if( node.m_op != ::HIR::ExprNode_Assign::Op::None )
            {
                // TODO: What about += on primitives?
                ASSERT_BUG(sp, node.m_op == ::HIR::ExprNode_Assign::Op::None, "Operator overload assignments should already be eliminated");
            }
            else
            {
                m_builder.push_stmt_assign(mv$(dst), mv$(val));
            }
        }
        
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            TRACE_FUNCTION_F("_BinOp");
            this->visit_node_ptr(node.m_left);
            auto left = m_builder.lvalue_or_temp( node.m_left->m_res_type, m_builder.get_result(node.m_left->span()) );
            
            this->visit_node_ptr(node.m_right);
            auto right = m_builder.lvalue_or_temp( node.m_right->m_res_type, m_builder.get_result(node.m_right->span()) );
            
            auto res = m_builder.new_temporary(node.m_res_type);
            ::MIR::eBinOp   op;
            switch(node.m_op)
            {
            case ::HIR::ExprNode_BinOp::Op::CmpEqu: op = ::MIR::eBinOp::EQ; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpNEqu:op = ::MIR::eBinOp::NE; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpLt:  op = ::MIR::eBinOp::LT; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpLtE: op = ::MIR::eBinOp::LE; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpGt:  op = ::MIR::eBinOp::GT; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpGtE: op = ::MIR::eBinOp::GE;
                m_builder.push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_BinOp({ mv$(left), op, mv$(right) }));
                break;
            
            case ::HIR::ExprNode_BinOp::Op::Xor: op = ::MIR::eBinOp::BIT_XOR; if(0)
            case ::HIR::ExprNode_BinOp::Op::Or : op = ::MIR::eBinOp::BIT_OR ; if(0)
            case ::HIR::ExprNode_BinOp::Op::And: op = ::MIR::eBinOp::BIT_AND;
                m_builder.push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_BinOp({ mv$(left), op, mv$(right) }));
                break;
            
            case ::HIR::ExprNode_BinOp::Op::Shr: op = ::MIR::eBinOp::BIT_SHR; if(0)
            case ::HIR::ExprNode_BinOp::Op::Shl: op = ::MIR::eBinOp::BIT_SHL;
                // TODO: Overflow checks
                m_builder.push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_BinOp({ mv$(left), op, mv$(right) }));
                break;
            
            case ::HIR::ExprNode_BinOp::Op::BoolAnd:
                TODO(node.span(), "&&");
                break;
            case ::HIR::ExprNode_BinOp::Op::BoolOr:
                TODO(node.span(), "||");
                break;
            
            case ::HIR::ExprNode_BinOp::Op::Add:    op = ::MIR::eBinOp::ADD; if(0)
            case ::HIR::ExprNode_BinOp::Op::Sub:    op = ::MIR::eBinOp::SUB; if(0)
            case ::HIR::ExprNode_BinOp::Op::Mul:    op = ::MIR::eBinOp::MUL; if(0)
            case ::HIR::ExprNode_BinOp::Op::Div:    op = ::MIR::eBinOp::DIV; if(0)
            case ::HIR::ExprNode_BinOp::Op::Mod:    op = ::MIR::eBinOp::MOD;
                // TODO: Overflow checks
                m_builder.push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_BinOp({ mv$(left), op, mv$(right) }));
                break;
            }
            m_builder.set_result( node.span(), mv$(res) );
        }
        
        void visit(::HIR::ExprNode_UniOp& node) override
        {
            TRACE_FUNCTION_F("_UniOp");
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.lvalue_or_temp( node.m_value->m_res_type, m_builder.get_result(node.m_value->span()) );
            
            auto res = m_builder.new_temporary(node.m_res_type);
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Ref:
                m_builder.push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_Borrow({ 0, ::HIR::BorrowType::Shared, mv$(val) }));
                break;
            case ::HIR::ExprNode_UniOp::Op::RefMut:
                m_builder.push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_Borrow({ 0, ::HIR::BorrowType::Unique, mv$(val) }));
                break;
            case ::HIR::ExprNode_UniOp::Op::Invert:
                m_builder.push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_UniOp({ mv$(val), ::MIR::eUniOp::INV }));
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                m_builder.push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_UniOp({ mv$(val), ::MIR::eUniOp::NEG }));
                break;
            }
            m_builder.set_result( node.span(), mv$(res) );
        }
        void visit(::HIR::ExprNode_Cast& node) override
        {
            TRACE_FUNCTION_F("_Cast");
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.lvalue_or_temp( node.m_value->m_res_type, m_builder.get_result(node.m_value->span()) );
            
            #if 0
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (node.m_res_type->m_data), (de),
            (
                ),
            (Primitive,
                switch(de)
                {
                
                }
                )
            )
            #endif
            auto res = m_builder.new_temporary(node.m_res_type);
            m_builder.push_stmt_assign(res.clone(), ::MIR::RValue::make_Cast({ mv$(val), node.m_res_type.clone() }));
            m_builder.set_result( node.span(), mv$(res) );
        }
        void visit(::HIR::ExprNode_Unsize& node) override
        {
            TRACE_FUNCTION_F("_Unsize");
            this->visit_node_ptr(node.m_value);
            TODO(node.span(), "MIR _Unsize to " << node.m_res_type);
        }
        void visit(::HIR::ExprNode_Index& node) override
        {
            TRACE_FUNCTION_F("_Index");
            this->visit_node_ptr(node.m_index);
            auto index = m_builder.lvalue_or_temp( node.m_index->m_res_type, m_builder.get_result(node.m_index->span()) );
            
            this->visit_node_ptr(node.m_value);
            auto value = m_builder.lvalue_or_temp( node.m_value->m_res_type, m_builder.get_result(node.m_value->span()) );
            
            if( false )
            {
                ::MIR::RValue   limit_val;
                TU_MATCH_DEF(::HIR::TypeRef::Data, (node.m_value->m_res_type.m_data), (e),
                (
                    BUG(node.span(), "Indexing unsupported type " << node.m_value->m_res_type);
                    ),
                (Array,
                    limit_val = ::MIR::Constant( e.size_val );
                    ),
                (Slice,
                    limit_val = ::MIR::RValue::make_DstMeta({ value.clone() });
                    )
                )
                
                auto limit_lval = m_builder.lvalue_or_temp(node.m_index->m_res_type, mv$(limit_val));
                
                auto cmp_res = m_builder.new_temporary( ::HIR::CoreType::Bool );
                m_builder.push_stmt_assign(cmp_res.clone(), ::MIR::RValue::make_BinOp({ index.clone(), ::MIR::eBinOp::GE, mv$(limit_lval) }));
                auto arm_panic = m_builder.new_bb_unlinked();
                auto arm_continue = m_builder.new_bb_unlinked();
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_res), arm_panic, arm_continue }) );
                
                m_builder.set_cur_block( arm_panic );
                // TODO: Call an "index fail" method which always panics.
                //m_builder.end_block( ::MIR::Terminator::make_Panic({}) );
                m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
                
                m_builder.set_cur_block( arm_continue );
            }
            
            m_builder.set_result( node.span(), ::MIR::LValue::make_Index({ box$(index), box$(value) }) );
        }
        
        void visit(::HIR::ExprNode_Deref& node) override
        {
            TRACE_FUNCTION_F("_Deref");
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.lvalue_or_temp( node.m_value->m_res_type, m_builder.get_result(node.m_value->span()) );
            
            m_builder.set_result( node.span(), ::MIR::LValue::make_Deref({ box$(val) }) );
        }
        
        void visit(::HIR::ExprNode_TupleVariant& node) override
        {
            TRACE_FUNCTION_F("_TupleVariant");
            ::std::vector< ::MIR::LValue>   values;
            values.reserve( node.m_args.size() );
            for(auto& arg : node.m_args)
            {
                this->visit_node_ptr(arg);
                values.push_back( m_builder.lvalue_or_temp( arg->m_res_type, m_builder.get_result(arg->span()) ) );
            }
            
            m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                node.m_path.clone(),
                mv$(values)
                }) );
        }
        
        void visit(::HIR::ExprNode_CallPath& node) override
        {
            TRACE_FUNCTION_F("_CallPath " << node.m_path);
            ::std::vector< ::MIR::LValue>   values;
            values.reserve( node.m_args.size() );
            for(auto& arg : node.m_args)
            {
                this->visit_node_ptr(arg);
                values.push_back( m_builder.lvalue_or_temp( arg->m_res_type, m_builder.get_result(arg->span()) ) );
            }
            
            // TODO: Obtain function type for this function
            auto fcn_ty_data = ::HIR::FunctionType {
                false,
                "",
                box$( node.m_cache.m_arg_types.back().clone() ),
                {}
                };
            for(unsigned int i = 0; i < node.m_cache.m_arg_types.size() - 1; i ++)
            {
                fcn_ty_data.m_arg_types.push_back( node.m_cache.m_arg_types[i].clone() );
            }
            auto fcn_val = m_builder.new_temporary( ::HIR::TypeRef(mv$(fcn_ty_data)) );
            m_builder.push_stmt_assign( fcn_val.clone(), ::MIR::RValue::make_Constant( ::MIR::Constant(node.m_path.clone()) ) );
            
            auto panic_block = m_builder.new_bb_unlinked();
            auto next_block = m_builder.new_bb_unlinked();
            auto res = m_builder.new_temporary( node.m_res_type );
            m_builder.end_block(::MIR::Terminator::make_Call({
                next_block, panic_block,
                res.clone(), mv$(fcn_val),
                mv$(values)
                }));
            
            m_builder.set_cur_block(panic_block);
            // TODO: Proper panic handling
            m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
            
            m_builder.set_cur_block( next_block );
            m_builder.set_result( node.span(), mv$(res) );
        }
        
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            BUG(node.span(), "Leftover _CallValue");
        }
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            BUG(node.span(), "Leftover _CallMethod");
        }
        void visit(::HIR::ExprNode_Field& node) override
        {
            TRACE_FUNCTION_F("_Field");
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.get_result_lvalue(node.m_value->span());
            
            unsigned int idx;
            if( '0' <= node.m_field[0] && node.m_field[0] <= '9' ) {
                ::std::stringstream(node.m_field) >> idx;
            }
            else {
                const auto& str = *node.m_value->m_res_type.m_data.as_Path().binding.as_Struct();
                const auto& fields = str.m_data.as_Named();
                idx = ::std::find_if( fields.begin(), fields.end(), [&](const auto& x){ return x.first == node.m_field; } ) - fields.begin();
            }
            m_builder.set_result( node.span(), ::MIR::LValue::make_Field({ box$(val), idx }) );
        }
        void visit(::HIR::ExprNode_Literal& node) override
        {
            TRACE_FUNCTION_F("_Literal");
            TU_MATCHA( (node.m_data), (e),
            (Integer,
                switch(node.m_res_type.m_data.as_Primitive())
                {
                case ::HIR::CoreType::U8:
                case ::HIR::CoreType::U16:
                case ::HIR::CoreType::U32:
                case ::HIR::CoreType::U64:
                case ::HIR::CoreType::Usize:
                    m_builder.set_result(node.span(), ::MIR::RValue( ::MIR::Constant(e.m_value) ));
                    break;
                case ::HIR::CoreType::I8:
                case ::HIR::CoreType::I16:
                case ::HIR::CoreType::I32:
                case ::HIR::CoreType::I64:
                case ::HIR::CoreType::Isize:
                    m_builder.set_result(node.span(), ::MIR::RValue( ::MIR::Constant( static_cast<int64_t>(e.m_value) ) ));
                    break;
                default:
                    BUG(node.span(), "Integer literal with unexpected type - " << node.m_res_type);
                }
                ),
            (Float,
                m_builder.set_result(node.span(), ::MIR::RValue::make_Constant( ::MIR::Constant(e.m_value) ));
                ),
            (Boolean,
                m_builder.set_result(node.span(), ::MIR::RValue::make_Constant( ::MIR::Constant(e) ));
                ),
            (String,
                m_builder.set_result(node.span(), ::MIR::RValue::make_Constant( ::MIR::Constant(e) ));
                ),
            (ByteString,
                auto v = mv$( *reinterpret_cast< ::std::vector<uint8_t>*>( &e) );
                m_builder.set_result(node.span(), ::MIR::RValue::make_Constant( ::MIR::Constant(mv$(v)) ));
                )
            )
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override
        {
            TRACE_FUNCTION_F("_UnitVariant");
            m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                node.m_path.clone(),
                {}
                }) );
        }
        void visit(::HIR::ExprNode_PathValue& node) override
        {
            TRACE_FUNCTION_F("_PathValue - " << node.m_path);
            m_builder.set_result( node.span(), ::MIR::LValue::make_Static(node.m_path.clone()) );
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
            TRACE_FUNCTION_F("_Variable - " << node.m_name << " #" << node.m_slot);
            m_builder.set_result( node.span(), ::MIR::LValue::make_Variable(node.m_slot) );
        }
        
        void visit(::HIR::ExprNode_StructLiteral& node) override
        {
            TRACE_FUNCTION_F("_StructLiteral");
            ::MIR::LValue   base_val;
            if( node.m_base_value )
            {
                this->visit_node_ptr(node.m_base_value);
                base_val = m_builder.get_result_lvalue(node.m_base_value->span());
            }
            
            const ::HIR::t_struct_fields* fields_ptr = nullptr;
            TU_MATCH(::HIR::TypeRef::TypePathBinding, (node.m_res_type.m_data.as_Path().binding), (e),
            (Unbound, ),
            (Opaque, ),
            (Enum,
                const auto& var_name = node.m_path.m_path.m_components.back();
                const auto& enm = *e;
                auto it = ::std::find_if(enm.m_variants.begin(), enm.m_variants.end(), [&](const auto&v)->auto{ return v.first == var_name; });
                assert(it != enm.m_variants.end());
                fields_ptr = &it->second.as_Struct();
                ),
            (Struct,
                fields_ptr = &e->m_data.as_Named();
                )
            )
            assert(fields_ptr);
            const ::HIR::t_struct_fields& fields = *fields_ptr;
            
            ::std::vector<bool> values_set;
            ::std::vector< ::MIR::LValue>   values;
            values.resize( fields.size() );
            values_set.resize( fields.size() );
            
            for(auto& ent : node.m_values)
            {
                auto idx = ::std::find_if(fields.begin(), fields.end(), [&](const auto&x){ return x.first == ent.first; }) - fields.begin();
                assert( !values_set[idx] );
                values_set[idx] = true;
                this->visit_node_ptr(ent.second);
                values.at(idx) = m_builder.lvalue_or_temp( ent.second->m_res_type, m_builder.get_result(ent.second->span()) );
            }
            for(unsigned int i = 0; i < values.size(); i ++)
            {
                if( !values_set[i] ) {
                    if( !node.m_base_value) {
                        ERROR(node.span(), E0000, "Field '" << fields[i].first << "' not specified");
                    }
                    values[i] = ::MIR::LValue::make_Field({ box$( base_val.clone() ), i });
                }
                else {
                    // Drop unused part of the base
                    if( node.m_base_value) {
                        m_builder.push_stmt_drop( ::MIR::LValue::make_Field({ box$( base_val.clone() ), i }) );
                    }
                }
            }
            
            m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                node.m_path.clone(),
                mv$(values)
                }) );
        }
        
        void visit(::HIR::ExprNode_Tuple& node) override
        {
            TRACE_FUNCTION_F("_Tuple");
            ::std::vector< ::MIR::LValue>   values;
            values.reserve( node.m_vals.size() );
            for(auto& arg : node.m_vals)
            {
                this->visit_node_ptr(arg);
                values.push_back( m_builder.lvalue_or_temp( arg->m_res_type, m_builder.get_result(arg->span()) ) );
            }
            
            m_builder.set_result( node.span(), ::MIR::RValue::make_Tuple({
                mv$(values)
                }) );
        }
        
        void visit(::HIR::ExprNode_ArrayList& node) override
        {
            TRACE_FUNCTION_F("_ArrayList");
            ::std::vector< ::MIR::LValue>   values;
            values.reserve( node.m_vals.size() );
            for(auto& arg : node.m_vals)
            {
                this->visit_node_ptr(arg);
                values.push_back( m_builder.lvalue_or_temp( arg->m_res_type, m_builder.get_result(arg->span()) ) );
            }
            
            m_builder.set_result( node.span(), ::MIR::RValue::make_Array({
                mv$(values)
                }) );
        }
        
        void visit(::HIR::ExprNode_ArraySized& node) override
        {
            TRACE_FUNCTION_F("_ArraySized");
            this->visit_node_ptr( node.m_val );
            auto value = m_builder.lvalue_or_temp( node.m_val->m_res_type, m_builder.get_result(node.m_val->span()) );
            
            m_builder.set_result( node.span(), ::MIR::RValue::make_SizedArray({
                mv$(value),
                static_cast<unsigned int>(node.m_size_val)
                }) );
        }
        
        void visit(::HIR::ExprNode_Closure& node) override
        {
            TRACE_FUNCTION_F("_Closure");
            TODO(node.span(), "_Closure");
        }
    };
}


::MIR::FunctionPointer LowerMIR(const ::HIR::ExprPtr& ptr, const ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >& args)
{
    ::MIR::Function fcn;
    
    ExprVisitor_Conv    ev { fcn };
    
    // 1. Apply destructuring to arguments
    unsigned int i = 0;
    for( const auto& arg : args )
    {
        ev.destructure_from(ptr->span(), arg.first, ::MIR::LValue::make_Argument({i}));
    }
    
    // 2. Destructure code
    ::HIR::ExprNode& root_node = const_cast<::HIR::ExprNode&>(*ptr);
    root_node.visit( ev );
    
    return ::MIR::FunctionPointer(new ::MIR::Function(mv$(fcn)));
}

namespace {
    class OuterVisitor:
        public ::HIR::Visitor
    {
    public:
        OuterVisitor(const ::HIR::Crate& crate)
        {}
        
        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) {
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                DEBUG("Array size " << ty);
                if( e.size ) {
                    auto fcn = LowerMIR(e.size, {});
                    e.size.m_mir = mv$(fcn);
                }
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }

        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            if( item.m_code )
            {
                DEBUG("Function code " << p);
                item.m_code.m_mir = LowerMIR(item.m_code, item.m_args);
            }
            else
            {
                DEBUG("Function code " << p << " (none)");
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            if( item.m_value )
            {
                DEBUG("`static` value " << p);
                item.m_value.m_mir = LowerMIR(item.m_value, {});
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                DEBUG("`const` value " << p);
                item.m_value.m_mir = LowerMIR(item.m_value, {});
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            //auto enum_type = ::HIR::TypeRef(::HIR::CoreType::Isize);
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    //DEBUG("Enum value " << p << " - " << var.first);
                    //::std::vector< ::HIR::TypeRef>  tmp;
                    //ExprVisitor_Extract    ev(m_resolve, tmp, m_new_trait_impls);
                    //ev.visit_root(*e);
                )
            }
        }
    };
}

void HIR_GenerateMIR(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

