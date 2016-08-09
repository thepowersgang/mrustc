/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/from_hir.hpp
 * - Construction of MIR from the HIR expression tree
 */
#include <type_traits>  // for TU_MATCHA
#include "mir.hpp"
#include "mir_ptr.hpp"
#include <hir/expr.hpp>
#include <hir/hir.hpp>
#include <algorithm>

namespace {
    class ExprVisitor_Conv:
        public ::HIR::ExprVisitor
    {
        ::MIR::Function&    m_output;
        
        unsigned int    m_current_block;
        bool    m_block_active;
        
        ::MIR::RValue   m_result;
        bool    m_result_valid;
        
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
            m_output(output)
        {}
        
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
                auto temp = this->new_temporary(ty);
                this->push_stmt_assign( ::MIR::LValue(temp.as_Temporary()), mv$(val) );
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
                BUG(Span(), "Pushing a result over an existing result");
            }
            m_result = mv$(val);
            m_result_valid = true;
        }
        
        void push_stmt_assign(::MIR::LValue dst, ::MIR::RValue val)
        {
            assert(m_block_active);
            m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Assign({ mv$(dst), mv$(val) }) );
        }
        void push_stmt_drop(::MIR::LValue val)
        {
            assert(m_block_active);
            m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Drop({ ::MIR::eDropKind::DEEP, mv$(val) }) );
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
            this->end_block( ::MIR::Terminator::make_Goto(rv) );
            this->set_cur_block(rv);
            return rv;
        }
        ::MIR::BasicBlockId new_bb_unlinked()
        {
            auto rv = m_output.blocks.size();
            m_output.blocks.push_back({});
            return rv;
        }
        
        void destructure_from(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval)
        {
            // TODO: Destructure
            TODO(sp, "Destructure using " << pat);
        }
        
        // -- ExprVisitor
        void visit(::HIR::ExprNode_Block& node) override
        {
            // NOTE: This doesn't create a BB, as BBs are not needed for scoping
            if( node.m_nodes.size() > 0 )
            {
                m_block_stack.push_back( {} );
                for(unsigned int i = 0; i < node.m_nodes.size()-1; i ++)
                {
                    const Span& sp = node.m_nodes[i]->span();
                    this->visit_node_ptr(node.m_nodes[i]);
                    this->push_stmt_drop( this->get_result_lvalue(sp) );
                }
                
                this->visit_node_ptr(node.m_nodes.back());
                auto ret = this->get_result(node.m_nodes.back()->span());
                
                auto bd = mv$( m_block_stack.back() );
                m_block_stack.pop_back();
                
                // Drop all bindings introduced during this block.
                for( auto& var_idx : bd.bindings ) {
                    this->push_stmt_drop( ::MIR::LValue::make_Variable(var_idx) );
                }
                
                this->set_result(node.span(), mv$(ret));
            }
            else
            {
                TODO(node.span(), "Lower empty blocks");
            }
        }
        void visit(::HIR::ExprNode_Return& node) override
        {
            this->visit_node_ptr(node.m_value);
            
            this->push_stmt_assign( ::MIR::LValue::make_Return({}),  this->get_result(node.span()) );
            this->end_block( ::MIR::Terminator::make_Return({}) );
        }
        void visit(::HIR::ExprNode_Let& node) override
        {
            if( node.m_value )
            {
                this->visit_node_ptr(node.m_value);
                
                this->destructure_from(node.span(), node.m_pattern, this->lvalue_or_temp(node.m_type, this->get_result(node.span()) ));
            }
        }
        void visit(::HIR::ExprNode_Loop& node) override
        {
            auto loop_block = this->new_bb_linked();
            auto loop_next = this->new_bb_unlinked();
            
            m_loop_stack.push_back( LoopDesc { node.m_label, loop_block, loop_next } );
            this->visit_node_ptr(node.m_code);
            m_loop_stack.pop_back();
            
            this->end_block( ::MIR::Terminator::make_Goto(loop_block) );
            this->set_cur_block(loop_next);
        }
        void visit(::HIR::ExprNode_LoopControl& node) override
        {
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
                this->end_block( ::MIR::Terminator::make_Goto(target_block->cur) );
            }
            else {
                this->end_block( ::MIR::Terminator::make_Goto(target_block->next) );
            }
        }
        
        void visit(::HIR::ExprNode_Match& node) override
        {
            this->visit_node_ptr(node.m_value);
            //auto match_val = this->get_result();
            
            if( node.m_arms.size() == 0 ) {
                // Nothing
                TODO(node.span(), "Handle zero-arm match");
            }
            else if( node.m_arms.size() == 1 ) {
                // - Shortcut: Single-arm match
                TODO(node.span(), "Convert single-arm match");
            }
            else {
                // TODO: Convert patterns into sequence of switches and comparisons.
                
                // Build up a sorted vector of MIR pattern rules
                TAGGED_UNION(PatternRule, Any,
                (Any, struct {}),
                (EnumVariant, unsigned int),
                (Value, ::MIR::Constant)
                );
                struct PatternRuleset {
                    ::std::vector<PatternRule>  m_rules;
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
                            // TODO: This is either a destructure or an enum
                            TODO(sp, "Match over path");
                            ),
                        (Generic,
                            // TODO: Is this possible? - Single arm has already been handled
                            ERROR(sp, E0000, "Attempting to match over a generic");
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
                            // TODO: Sub-match
                            TODO(sp, "Match over borrow");
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
                ::std::vector< ::std::pair< unsigned int, PatternRuleset > >   m_arm_rules;
                for(const auto& arm : node.m_arms) {
                    auto idx = m_arm_rules.size();
                    for( const auto& pat : arm.m_patterns )
                    {
                        auto builder = PatternRulesetBuilder {};
                        builder.append_from(node.span(), pat, node.m_value->m_res_type);
                        m_arm_rules.push_back( ::std::make_pair(idx, builder.into_ruleset()) );
                    }
                    
                    if( arm.m_cond ) {
                        // - TODO: What to do with contidionals?
                        TODO(node.span(), "Handle conditional match arms (ordering matters)");
                    }
                }
                
                // TODO: Sort ruleset such that wildcards go last (if there's no conditionals)

                // TODO: Generate decision tree based on ruleset
                
                TODO(node.span(), "Convert match into MIR using ruleset");
            }
        } // ExprNode_Match
        
        void visit(::HIR::ExprNode_If& node) override
        {
            this->visit_node_ptr(node.m_cond);
            auto decision_val = this->lvalue_or_temp(node.m_cond->m_res_type, this->get_result(node.m_cond->span()) );
            
            auto true_branch = this->new_bb_unlinked();
            auto false_branch = this->new_bb_unlinked();
            auto next_block = this->new_bb_unlinked();
            this->end_block( ::MIR::Terminator::make_If({ mv$(decision_val), true_branch, false_branch }) );
            
            auto result_val = this->new_temporary(node.m_res_type);
            
            this->set_cur_block(true_branch);
            this->visit_node_ptr(node.m_true);
            this->end_block( ::MIR::Terminator::make_Goto(next_block) );
            this->push_stmt_assign( ::MIR::LValue::make_Temporary({result_val.as_Temporary().idx}), this->get_result(node.m_true->span()) );
            
            this->set_cur_block(false_branch);
            if( node.m_false )
            {
                this->visit_node_ptr(node.m_false);
                this->end_block( ::MIR::Terminator::make_Goto(next_block) );
                this->push_stmt_assign( ::MIR::LValue::make_Temporary({result_val.as_Temporary().idx}), this->get_result(node.m_false->span()) );
            }
            else
            {
                // TODO: Assign `()` to the result
            }
            
            this->set_cur_block(next_block);
            this->set_result( node.span(), mv$(result_val) );
        }
        
        void visit(::HIR::ExprNode_Assign& node) override
        {
            const auto& sp = node.span();
            
            this->visit_node_ptr(node.m_value);
            auto val = this->get_result(sp);
            
            this->visit_node_ptr(node.m_slot);
            auto dst = this->get_result_lvalue(sp);
            
            if( node.m_op != ::HIR::ExprNode_Assign::Op::None )
            {
                // TODO: What about += on primitives?
                ASSERT_BUG(sp, node.m_op == ::HIR::ExprNode_Assign::Op::None, "Operator overload assignments should already be eliminated");
            }
            else
            {
                this->push_stmt_assign(mv$(dst), mv$(val));
            }
        }
        
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            this->visit_node_ptr(node.m_left);
            auto left = this->lvalue_or_temp( node.m_left->m_res_type, this->get_result(node.m_left->span()) );
            
            this->visit_node_ptr(node.m_right);
            auto right = this->lvalue_or_temp( node.m_right->m_res_type, this->get_result(node.m_right->span()) );
            
            auto res = this->new_temporary(node.m_res_type);
            ::MIR::eBinOp   op;
            switch(node.m_op)
            {
            case ::HIR::ExprNode_BinOp::Op::CmpEqu: op = ::MIR::eBinOp::EQ; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpNEqu:op = ::MIR::eBinOp::NE; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpLt:  op = ::MIR::eBinOp::LT; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpLtE: op = ::MIR::eBinOp::LE; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpGt:  op = ::MIR::eBinOp::GT; if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpGtE: op = ::MIR::eBinOp::GE;
                this->push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_BinOp({ mv$(left), op, mv$(right) }));
                break;
            
            case ::HIR::ExprNode_BinOp::Op::Xor: op = ::MIR::eBinOp::BIT_XOR; if(0)
            case ::HIR::ExprNode_BinOp::Op::Or : op = ::MIR::eBinOp::BIT_OR ; if(0)
            case ::HIR::ExprNode_BinOp::Op::And: op = ::MIR::eBinOp::BIT_AND;
                this->push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_BinOp({ mv$(left), op, mv$(right) }));
                break;
            
            case ::HIR::ExprNode_BinOp::Op::Shr: op = ::MIR::eBinOp::BIT_SHR; if(0)
            case ::HIR::ExprNode_BinOp::Op::Shl: op = ::MIR::eBinOp::BIT_SHL;
                // TODO: Overflow checks
                this->push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_BinOp({ mv$(left), op, mv$(right) }));
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
                this->push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_BinOp({ mv$(left), op, mv$(right) }));
                break;
            }
            this->set_result( node.span(), mv$(res) );
        }
        
        void visit(::HIR::ExprNode_UniOp& node) override
        {
            this->visit_node_ptr(node.m_value);
            auto val = this->lvalue_or_temp( node.m_value->m_res_type, this->get_result(node.m_value->span()) );
            
            auto res = this->new_temporary(node.m_res_type);
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Ref:
                this->push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_Borrow({ 0, ::HIR::BorrowType::Shared, mv$(val) }));
                break;
            case ::HIR::ExprNode_UniOp::Op::RefMut:
                this->push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_Borrow({ 0, ::HIR::BorrowType::Unique, mv$(val) }));
                break;
            case ::HIR::ExprNode_UniOp::Op::Invert:
                this->push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_UniOp({ mv$(val), ::MIR::eUniOp::INV }));
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                this->push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_UniOp({ mv$(val), ::MIR::eUniOp::NEG }));
                break;
            }
            this->set_result( node.span(), mv$(res) );
        }
        void visit(::HIR::ExprNode_Cast& node) override
        {
            this->visit_node_ptr(node.m_value);
            TODO(node.span(), "MIR _Cast");
        }
        void visit(::HIR::ExprNode_Unsize& node) override
        {
            this->visit_node_ptr(node.m_value);
            TODO(node.span(), "MIR _Unsize");
        }
        void visit(::HIR::ExprNode_Index& node) override
        {
            this->visit_node_ptr(node.m_index);
            auto index = this->lvalue_or_temp( node.m_index->m_res_type, this->get_result(node.m_index->span()) );
            
            this->visit_node_ptr(node.m_value);
            auto value = this->lvalue_or_temp( node.m_value->m_res_type, this->get_result(node.m_value->span()) );
            
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
                
                auto limit_lval = this->lvalue_or_temp(node.m_index->m_res_type, mv$(limit_val));
                
                auto cmp_res = this->new_temporary( ::HIR::CoreType::Bool );
                this->push_stmt_assign(cmp_res.clone(), ::MIR::RValue::make_BinOp({ index.clone(), ::MIR::eBinOp::GE, mv$(limit_lval) }));
                auto arm_panic = this->new_bb_unlinked();
                auto arm_continue = this->new_bb_unlinked();
                this->end_block( ::MIR::Terminator::make_If({ mv$(cmp_res), arm_panic, arm_continue }) );
                
                this->set_cur_block( arm_panic );
                // TODO: Call an "index fail" method which always panics.
                //this->end_block( ::MIR::Terminator::make_Panic({}) );
                this->end_block( ::MIR::Terminator::make_Diverge({}) );
                
                this->set_cur_block( arm_continue );
            }
            
            this->set_result( node.span(), ::MIR::LValue::make_Index({ box$(index), box$(value) }) );
        }
        
        void visit(::HIR::ExprNode_Deref& node) override
        {
            this->visit_node_ptr(node.m_value);
            auto val = this->lvalue_or_temp( node.m_value->m_res_type, this->get_result(node.m_value->span()) );
            
            this->set_result( node.span(), ::MIR::LValue::make_Deref({ box$(val) }) );
        }
        
        void visit(::HIR::ExprNode_TupleVariant& node) override
        {
            ::std::vector< ::MIR::LValue>   values;
            values.reserve( node.m_args.size() );
            for(auto& arg : node.m_args)
            {
                this->visit_node_ptr(arg);
                values.push_back( this->lvalue_or_temp( arg->m_res_type, this->get_result(arg->span()) ) );
            }
            
            this->set_result( node.span(), ::MIR::RValue::make_Struct({
                node.m_path.clone(),
                mv$(values)
                }) );
        }
        
        void visit(::HIR::ExprNode_CallPath& node) override
        {
            ::std::vector< ::MIR::LValue>   values;
            values.reserve( node.m_args.size() );
            for(auto& arg : node.m_args)
            {
                this->visit_node_ptr(arg);
                values.push_back( this->lvalue_or_temp( arg->m_res_type, this->get_result(arg->span()) ) );
            }
            
            // TODO: Obtain function type for this function
            auto fcn_val = this->new_temporary( ::HIR::TypeRef(::HIR::FunctionType {
                } ) );
            
            auto panic_block = this->new_bb_unlinked();
            auto next_block = this->new_bb_unlinked();
            auto res = this->new_temporary( node.m_res_type );
            this->end_block(::MIR::Terminator::make_Call({
                next_block, panic_block,
                res.clone(), mv$(fcn_val),
                mv$(values)
                }));
            
            this->set_cur_block(panic_block);
            // TODO: Proper panic handling
            this->end_block( ::MIR::Terminator::make_Diverge({}) );
            
            this->set_cur_block( next_block );
            this->set_result( node.span(), mv$(res) );
        }
        
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            BUG(node.span(), "Leftover _CallValue");
        }
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            BUG(node.span(), "Leftover _CallValue");
        }
        void visit(::HIR::ExprNode_Field& node) override
        {
            this->visit_node_ptr(node.m_value);
            auto val = this->get_result_lvalue(node.m_value->span());
            
            unsigned int idx;
            if( '0' <= node.m_field[0] && node.m_field[0] <= '9' ) {
                ::std::stringstream(node.m_field) >> idx;
            }
            else {
                const auto& str = *node.m_value->m_res_type.m_data.as_Path().binding.as_Struct();
                const auto& fields = str.m_data.as_Named();
                idx = ::std::find_if( fields.begin(), fields.end(), [&](const auto& x){ return x.first == node.m_field; } ) - fields.begin();
            }
            this->set_result( node.span(), ::MIR::LValue::make_Field({ box$(val), idx }) );
        }
        void visit(::HIR::ExprNode_Literal& node) override
        {
            TODO(node.span(), "Primitive literals");
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override
        {
            this->set_result( node.span(), ::MIR::RValue::make_Struct({
                node.m_path.clone(),
                {}
                }) );
        }
        void visit(::HIR::ExprNode_PathValue& node) override
        {
            this->set_result( node.span(), ::MIR::LValue::make_Static(node.m_path.clone()) );
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
            this->set_result( node.span(), ::MIR::LValue::make_Variable(node.m_slot) );
        }
        
        void visit(::HIR::ExprNode_StructLiteral& node) override
        {
            ::MIR::LValue   base_val;
            if( node.m_base_value )
            {
                this->visit_node_ptr(node.m_base_value);
                base_val = this->get_result_lvalue(node.m_base_value->span());
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
                values.at(idx) = this->lvalue_or_temp( ent.second->m_res_type, this->get_result(ent.second->span()) );
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
                        this->push_stmt_drop( ::MIR::LValue::make_Field({ box$( base_val.clone() ), i }) );
                    }
                }
            }
            
            this->set_result( node.span(), ::MIR::RValue::make_Struct({
                node.m_path.clone(),
                mv$(values)
                }) );
        }
        
        void visit(::HIR::ExprNode_Tuple& node) override
        {
            ::std::vector< ::MIR::LValue>   values;
            values.reserve( node.m_vals.size() );
            for(auto& arg : node.m_vals)
            {
                this->visit_node_ptr(arg);
                values.push_back( this->lvalue_or_temp( arg->m_res_type, this->get_result(arg->span()) ) );
            }
            
            this->set_result( node.span(), ::MIR::RValue::make_Tuple({
                mv$(values)
                }) );
        }
        
        void visit(::HIR::ExprNode_ArrayList& node) override
        {
            ::std::vector< ::MIR::LValue>   values;
            values.reserve( node.m_vals.size() );
            for(auto& arg : node.m_vals)
            {
                this->visit_node_ptr(arg);
                values.push_back( this->lvalue_or_temp( arg->m_res_type, this->get_result(arg->span()) ) );
            }
            
            this->set_result( node.span(), ::MIR::RValue::make_Array({
                mv$(values)
                }) );
        }
        
        void visit(::HIR::ExprNode_ArraySized& node) override
        {
            this->visit_node_ptr( node.m_val );
            auto value = this->lvalue_or_temp( node.m_val->m_res_type, this->get_result(node.m_val->span()) );
            
            this->set_result( node.span(), ::MIR::RValue::make_SizedArray({
                mv$(value),
                static_cast<unsigned int>(node.m_size_val)
                }) );
        }
        
        void visit(::HIR::ExprNode_Closure& node) override
        {
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

