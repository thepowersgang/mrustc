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
#include "from_hir.hpp"


namespace {
    
    class ExprVisitor_Conv:
        public MirConverter
    {
        MirBuilder  m_builder;
        const ::std::vector< ::HIR::TypeRef>&  m_variable_types;
        
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
        ExprVisitor_Conv(::MIR::Function& output, const ::std::vector< ::HIR::TypeRef>& var_types):
            m_builder(output),
            m_variable_types(var_types)
        {
        }
        
        void destructure_from(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval, bool allow_refutable=false) override
        {
            destructure_from_ex(sp, pat, mv$(lval), (allow_refutable ? 1 : 0));
        }
        
        void destructure_from_ex(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval, int allow_refutable=0) // 1 : yes, 2 : disallow binding
        {
            if( allow_refutable != 3 && pat.m_binding.is_valid() ) {
                if( allow_refutable == 2 ) {
                    BUG(sp, "Binding when not expected");
                }
                else if( allow_refutable == 0 ) {
                    ASSERT_BUG(sp, pat.m_data.is_Any(), "Destructure patterns can't bind and match");
                    
                    m_builder.push_stmt_assign( ::MIR::LValue::make_Variable(pat.m_binding.m_slot), mv$(lval) );
                }
                else {
                    // Refutable and binding allowed
                    m_builder.push_stmt_assign( ::MIR::LValue::make_Variable(pat.m_binding.m_slot), lval.clone() );
                    
                    destructure_from_ex(sp, pat, mv$(lval), 3);
                }
                return;
            }
            if( allow_refutable == 3 ) {
                allow_refutable = 2;
            }
            
            TU_MATCHA( (pat.m_data), (e),
            (Any,
                ),
            (Box,
                TODO(sp, "Destructure using " << pat);
                ),
            (Ref,
                destructure_from_ex(sp, *e.sub, ::MIR::LValue::make_Deref({ box$( mv$(lval) ) }), allow_refutable);
                ),
            (Tuple,
                for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                {
                    destructure_from_ex(sp, e.sub_patterns[i], ::MIR::LValue::make_Field({ box$( lval.clone() ), i}), allow_refutable);
                }
                ),
            (StructValue,
                // Nothing.
                ),
            (StructTuple,
                for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                {
                    destructure_from_ex(sp, e.sub_patterns[i], ::MIR::LValue::make_Field({ box$( lval.clone() ), i}), allow_refutable);
                }
                ),
            (StructTupleWildcard,
                // Nothing.
                ),
            (Struct,
                const auto& str = *e.binding;
                const auto& fields = str.m_data.as_Named();
                for(const auto& fld_pat : e.sub_patterns)
                {
                    unsigned idx = ::std::find_if( fields.begin(), fields.end(), [&](const auto&x){ return x.first == fld_pat.first; } ) - fields.begin();
                    destructure_from_ex(sp, fld_pat.second, ::MIR::LValue::make_Field({ box$( lval.clone() ), idx}), allow_refutable);
                }
                ),
            // Refutable
            (Value,
                ASSERT_BUG(sp, allow_refutable, "Refutable pattern not expected - " << pat);
                ),
            (Range,
                ASSERT_BUG(sp, allow_refutable, "Refutable pattern not expected - " << pat);
                ),
            (EnumValue,
                ASSERT_BUG(sp, allow_refutable, "Refutable pattern not expected - " << pat);
                ),
            (EnumTuple,
                ASSERT_BUG(sp, allow_refutable, "Refutable pattern not expected - " << pat);
                auto lval_var = ::MIR::LValue::make_Downcast({ box$(mv$(lval)), e.binding_idx });
                for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                {
                    destructure_from_ex(sp, e.sub_patterns[i], ::MIR::LValue::make_Field({ box$( lval_var.clone() ), i}), allow_refutable);
                }
                ),
            (EnumTupleWildcard,
                ASSERT_BUG(sp, allow_refutable, "Refutable pattern not expected - " << pat);
                ),
            (EnumStruct,
                ASSERT_BUG(sp, allow_refutable, "Refutable pattern not expected - " << pat);
                const auto& enm = *e.binding_ptr;
                const auto& fields = enm.m_variants[e.binding_idx].second.as_Struct();
                auto lval_var = ::MIR::LValue::make_Downcast({ box$(mv$(lval)), e.binding_idx });
                for(const auto& fld_pat : e.sub_patterns)
                {
                    unsigned idx = ::std::find_if( fields.begin(), fields.end(), [&](const auto&x){ return x.first == fld_pat.first; } ) - fields.begin();
                    destructure_from_ex(sp, fld_pat.second, ::MIR::LValue::make_Field({ box$( lval_var.clone() ), idx}), allow_refutable);
                }
                ),
            (Slice,
                ASSERT_BUG(sp, allow_refutable, "Refutable pattern not expected - " << pat);
                TODO(sp, "Destructure using " << pat);
                ),
            (SplitSlice,
                ASSERT_BUG(sp, allow_refutable, "Refutable pattern not expected - " << pat);
                TODO(sp, "Destructure using " << pat);
                )
            )
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
                    if( m_builder.block_active() || m_builder.has_result() )
                    {
                        m_builder.push_stmt_drop( m_builder.lvalue_or_temp(subnode->m_res_type, m_builder.get_result(sp)) );
                    }
                }
                
                this->visit_node_ptr(node.m_nodes.back());
                
                auto bd = mv$( m_block_stack.back() );
                m_block_stack.pop_back();
                
                // Drop all bindings introduced during this block.
                // TODO: This should be done in a more generic manner, allowing for drops on panic/return
                if( m_builder.block_active() )
                {
                    for( auto& var_idx : bd.bindings ) {
                        m_builder.push_stmt_drop( ::MIR::LValue::make_Variable(var_idx) );
                    }
                }
                
                // Result maintained from last node
            }
            else
            {
                m_builder.set_result(node.span(), ::MIR::RValue::make_Tuple({}));
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
            
            // If there's a stray result, drop i
            if( m_builder.has_result() ) {
                assert( m_builder.block_active() );
                //::MIR::RValue res = m_builder.get_result(node.span());
                //if( res.is_Use() ) {
                //    m_builder.push_stmt_drop( mv$(res.as_Use()) );
                //}
            }
            // Terminate block with a jump back to the start
            if( m_builder.block_active() )
            {
                m_builder.end_block( ::MIR::Terminator::make_Goto(loop_block) );
            }
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
                MIR_LowerHIR_Match(m_builder, *this, node, mv$(match_val));
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
            if( m_builder.block_active() )
            {
                m_builder.push_stmt_assign( result_val.clone(), m_builder.get_result(node.m_true->span()) );
                m_builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
            }
            
            m_builder.set_cur_block(false_branch);
            if( node.m_false )
            {
                this->visit_node_ptr(node.m_false);
                if( m_builder.block_active() )
                {
                    m_builder.push_stmt_assign( result_val.clone(), m_builder.get_result(node.m_false->span()) );
                    m_builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
                }
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
        
        void generate_checked_binop(const Span& sp, ::MIR::LValue res_slot, ::MIR::eBinOp op, ::MIR::LValue val_l, const ::HIR::TypeRef& ty_l, ::MIR::LValue val_r, const ::HIR::TypeRef& ty_r)
        {
            switch(op)
            {
            case ::MIR::eBinOp::EQ: case ::MIR::eBinOp::NE:
            case ::MIR::eBinOp::LT: case ::MIR::eBinOp::LE:
            case ::MIR::eBinOp::GT: case ::MIR::eBinOp::GE:
                ASSERT_BUG(sp, ty_l == ty_r, "Types in comparison operators must be equal - " << ty_l << " != " << ty_r);
                // Defensive assert that the type is a valid MIR comparison
                TU_MATCH_DEF(::HIR::TypeRef::Data, (ty_l.m_data), (e),
                (
                    BUG(sp, "Invalid type in comparison - " << ty_l);
                    ),
                (Pointer,
                    // Valid
                    ),
                // TODO: Should straight comparisons on &str be supported here?
                (Primitive,
                    if( e == ::HIR::CoreType::Str ) {
                        BUG(sp, "Invalid type in comparison - " << ty_l);
                    }
                    )
                )
                m_builder.push_stmt_assign(mv$(res_slot), ::MIR::RValue::make_BinOp({ mv$(val_l), op, mv$(val_r) }));
                break;
            // Bitwise masking operations: Require equal integer types or bool
            case ::MIR::eBinOp::BIT_XOR:
            case ::MIR::eBinOp::BIT_OR :
            case ::MIR::eBinOp::BIT_AND:
                ASSERT_BUG(sp, ty_l == ty_r, "Types in bitwise operators must be equal - " << ty_l << " != " << ty_r);
                ASSERT_BUG(sp, ty_l.m_data.is_Primitive(), "Only primitives allowed in bitwise operators");
                switch(ty_l.m_data.as_Primitive())
                {
                case ::HIR::CoreType::Str:
                case ::HIR::CoreType::Char:
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    BUG(sp, "Invalid type for bitwise operator - " << ty_l);
                default:
                    break;
                }
                m_builder.push_stmt_assign(mv$(res_slot), ::MIR::RValue::make_BinOp({ mv$(val_l), op, mv$(val_r) }));
                break;
            case ::MIR::eBinOp::ADD:    case ::MIR::eBinOp::ADD_OV:
            case ::MIR::eBinOp::SUB:    case ::MIR::eBinOp::SUB_OV:
            case ::MIR::eBinOp::MUL:    case ::MIR::eBinOp::MUL_OV:
            case ::MIR::eBinOp::DIV:    case ::MIR::eBinOp::DIV_OV:
            case ::MIR::eBinOp::MOD:
                ASSERT_BUG(sp, ty_l == ty_r, "Types in arithmatic operators must be equal - " << ty_l << " != " << ty_r);
                ASSERT_BUG(sp, ty_l.m_data.is_Primitive(), "Only primitives allowed in arithmatic operators");
                switch(ty_l.m_data.as_Primitive())
                {
                case ::HIR::CoreType::Str:
                case ::HIR::CoreType::Char:
                case ::HIR::CoreType::Bool:
                    BUG(sp, "Invalid type for arithmatic operator - " << ty_l);
                default:
                    break;
                }
                // TODO: Overflow checks (none for eBinOp::MOD)
                m_builder.push_stmt_assign(mv$(res_slot), ::MIR::RValue::make_BinOp({ mv$(val_l), op, mv$(val_r) }));
                break;
            case ::MIR::eBinOp::BIT_SHL:
            case ::MIR::eBinOp::BIT_SHR:
                ;
                ASSERT_BUG(sp, ty_l.m_data.is_Primitive(), "Only primitives allowed in arithmatic operators");
                ASSERT_BUG(sp, ty_r.m_data.is_Primitive(), "Only primitives allowed in arithmatic operators");
                switch(ty_l.m_data.as_Primitive())
                {
                case ::HIR::CoreType::Str:
                case ::HIR::CoreType::Char:
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    BUG(sp, "Invalid type for shift op-assignment - " << ty_l);
                default:
                    break;
                }
                switch(ty_r.m_data.as_Primitive())
                {
                case ::HIR::CoreType::Str:
                case ::HIR::CoreType::Char:
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    BUG(sp, "Invalid type for shift op-assignment - " << ty_r);
                default:
                    break;
                }
                // TODO: Overflow check
                m_builder.push_stmt_assign(mv$(res_slot), ::MIR::RValue::make_BinOp({ mv$(val_l), op, mv$(val_r) }));
                break;
            }
        }
        
        void visit(::HIR::ExprNode_Assign& node) override
        {
            TRACE_FUNCTION_F("_Assign");
            const auto& sp = node.span();
            
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.get_result(sp);
            
            this->visit_node_ptr(node.m_slot);
            auto dst = m_builder.get_result_lvalue(sp);
            
            const auto& ty_slot = node.m_slot->m_res_type;
            const auto& ty_val  = node.m_value->m_res_type;
            
            if( node.m_op != ::HIR::ExprNode_Assign::Op::None )
            {
                auto dst_clone = dst.clone();
                auto val_lv = m_builder.lvalue_or_temp( ty_val, mv$(val) );
                
                ASSERT_BUG(sp, ty_slot.m_data.is_Primitive(), "Assignment operator overloads are only valid on primitives - ty_slot="<<ty_slot);
                ASSERT_BUG(sp, ty_val.m_data.is_Primitive(), "Assignment operator overloads are only valid on primitives - ty_val="<<ty_val);
                
                ::MIR::RValue res;
                #define _(v)    ::HIR::ExprNode_Assign::Op::v
                ::MIR::eBinOp   op;
                switch(node.m_op)
                {
                case _(None):  throw "";
                case _(Add): op = ::MIR::eBinOp::ADD; if(0)
                case _(Sub): op = ::MIR::eBinOp::SUB; if(0)
                case _(Mul): op = ::MIR::eBinOp::MUL; if(0)
                case _(Div): op = ::MIR::eBinOp::DIV; if(0)
                case _(Mod): op = ::MIR::eBinOp::MOD; if(0)
                    ;
                    this->generate_checked_binop(sp, mv$(dst), op, mv$(dst_clone), ty_slot,  mv$(val_lv), ty_val);
                    break;
                case _(Xor): op = ::MIR::eBinOp::BIT_XOR; if(0)
                case _(Or ): op = ::MIR::eBinOp::BIT_OR ; if(0)
                case _(And): op = ::MIR::eBinOp::BIT_AND; if(0)
                    ;
                    this->generate_checked_binop(sp, mv$(dst), op, mv$(dst_clone), ty_slot,  mv$(val_lv), ty_val);
                    break;
                case _(Shl): op = ::MIR::eBinOp::BIT_SHL; if(0)
                case _(Shr): op = ::MIR::eBinOp::BIT_SHR; if(0)
                    ;
                    this->generate_checked_binop(sp, mv$(dst), op, mv$(dst_clone), ty_slot,  mv$(val_lv), ty_val);
                    break;
                }
                #undef _
                
                m_builder.push_stmt_assign(mv$(dst), mv$(res));
            }
            else
            {
                ASSERT_BUG(sp, ty_slot == ty_val, "Types must match for assignment - " << ty_slot << " != " << ty_val);
                m_builder.push_stmt_assign(mv$(dst), mv$(val));
            }
            m_builder.set_result(node.span(), ::MIR::RValue::make_Tuple({}));
        }
        
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            const auto& sp = node.span();
            TRACE_FUNCTION_F("_BinOp");
            
            const auto& ty_l = node.m_left->m_res_type;
            this->visit_node_ptr(node.m_left);
            auto left = m_builder.lvalue_or_temp( ty_l, m_builder.get_result(node.m_left->span()) );
            
            const auto& ty_r = node.m_right->m_res_type;
            this->visit_node_ptr(node.m_right);
            auto right = m_builder.lvalue_or_temp( ty_r, m_builder.get_result(node.m_right->span()) );
            
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
                this->generate_checked_binop(sp, res.clone(), op, mv$(left), ty_l, mv$(right), ty_r);
                break;
            
            case ::HIR::ExprNode_BinOp::Op::Xor: op = ::MIR::eBinOp::BIT_XOR; if(0)
            case ::HIR::ExprNode_BinOp::Op::Or : op = ::MIR::eBinOp::BIT_OR ; if(0)
            case ::HIR::ExprNode_BinOp::Op::And: op = ::MIR::eBinOp::BIT_AND;
                this->generate_checked_binop(sp, res.clone(), op, mv$(left), ty_l, mv$(right), ty_r);
                break;
            
            case ::HIR::ExprNode_BinOp::Op::Shr: op = ::MIR::eBinOp::BIT_SHR; if(0)
            case ::HIR::ExprNode_BinOp::Op::Shl: op = ::MIR::eBinOp::BIT_SHL;
                this->generate_checked_binop(sp, res.clone(), op, mv$(left), ty_l, mv$(right), ty_r);
                break;
            
            case ::HIR::ExprNode_BinOp::Op::Add:    op = ::MIR::eBinOp::ADD; if(0)
            case ::HIR::ExprNode_BinOp::Op::Sub:    op = ::MIR::eBinOp::SUB; if(0)
            case ::HIR::ExprNode_BinOp::Op::Mul:    op = ::MIR::eBinOp::MUL; if(0)
            case ::HIR::ExprNode_BinOp::Op::Div:    op = ::MIR::eBinOp::DIV; if(0)
            case ::HIR::ExprNode_BinOp::Op::Mod:    op = ::MIR::eBinOp::MOD;
                this->generate_checked_binop(sp, res.clone(), op, mv$(left), ty_l, mv$(right), ty_r);
                break;
            
            case ::HIR::ExprNode_BinOp::Op::BoolAnd: {
                auto bb_next = m_builder.new_bb_unlinked();
                auto bb_true = m_builder.new_bb_unlinked();
                auto bb_false = m_builder.new_bb_unlinked();
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(left), bb_true, bb_false }) );
                // If left is false, assign result false and return
                m_builder.set_cur_block( bb_false );
                m_builder.push_stmt_assign(res.clone(), ::MIR::RValue( ::MIR::Constant::make_Bool(false) ));
                m_builder.end_block( ::MIR::Terminator::make_Goto(bb_next) );
                
                // If left is true, assign result to right
                m_builder.set_cur_block( bb_true );
                m_builder.push_stmt_assign(res.clone(), mv$(right));    // TODO: Right doens't need to be an LValue here.
                m_builder.end_block( ::MIR::Terminator::make_Goto(bb_next) );
                
                m_builder.set_cur_block( bb_next );
                } break;
            case ::HIR::ExprNode_BinOp::Op::BoolOr: {
                auto bb_next = m_builder.new_bb_unlinked();
                auto bb_true = m_builder.new_bb_unlinked();
                auto bb_false = m_builder.new_bb_unlinked();
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(left), bb_true, bb_false }) );
                // If left is true, assign result true and return
                m_builder.set_cur_block( bb_true );
                m_builder.push_stmt_assign(res.clone(), ::MIR::RValue( ::MIR::Constant::make_Bool(true) ));
                m_builder.end_block( ::MIR::Terminator::make_Goto(bb_next) );
                
                // If left is false, assign result to right
                m_builder.set_cur_block( bb_false );
                m_builder.push_stmt_assign(res.clone(), mv$(right));    // TODO: Right doens't need to be an LValue here.
                m_builder.end_block( ::MIR::Terminator::make_Goto(bb_next) );
                
                m_builder.set_cur_block( bb_next );
                } break;
            }
            m_builder.set_result( node.span(), mv$(res) );
        }
        
        void visit(::HIR::ExprNode_UniOp& node) override
        {
            TRACE_FUNCTION_F("_UniOp");
            
            const auto& ty_val = node.m_value->m_res_type;
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.lvalue_or_temp( ty_val, m_builder.get_result(node.m_value->span()) );
            
            auto res = m_builder.new_temporary(node.m_res_type);
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Invert:
                if( ty_val.m_data.is_Primitive() ) {
                    switch( ty_val.m_data.as_Primitive() )
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        BUG(node.span(), "`!` operator on invalid type - " << ty_val);
                        break;
                    default:
                        break;
                    }
                }
                else {
                    BUG(node.span(), "`!` operator on invalid type - " << ty_val);
                }
                m_builder.push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_UniOp({ mv$(val), ::MIR::eUniOp::INV }));
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                if( ty_val.m_data.is_Primitive() ) {
                    switch( ty_val.m_data.as_Primitive() )
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Bool:
                        BUG(node.span(), "`-` operator on invalid type - " << ty_val);
                        break;
                    case ::HIR::CoreType::U8:
                    case ::HIR::CoreType::U16:
                    case ::HIR::CoreType::U32:
                    case ::HIR::CoreType::U64:
                    case ::HIR::CoreType::Usize:
                        BUG(node.span(), "`-` operator on unsigned integer - " << ty_val);
                        break;
                    default:
                        break;
                    }
                }
                else {
                    BUG(node.span(), "`!` operator on invalid type - " << ty_val);
                }
                m_builder.push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_UniOp({ mv$(val), ::MIR::eUniOp::NEG }));
                break;
            }
            m_builder.set_result( node.span(), mv$(res) );
        }
        void visit(::HIR::ExprNode_Borrow& node) override
        {
            TRACE_FUNCTION_F("_Borrow");
            
            const auto& ty_val = node.m_value->m_res_type;
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.lvalue_or_temp( ty_val, m_builder.get_result(node.m_value->span()) );
            
            auto res = m_builder.new_temporary(node.m_res_type);
            m_builder.push_stmt_assign(res.as_Temporary(), ::MIR::RValue::make_Borrow({ 0, node.m_type, mv$(val) }));
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
            auto ptr_lval = m_builder.lvalue_or_temp( node.m_value->m_res_type, m_builder.get_result(node.span()) );
            
            const auto& ty_out = node.m_res_type;
            const auto& ty_in = node.m_value->m_res_type;
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty_out.m_data), (e),
            (
                TODO(node.span(), "MIR _Unsize to " << ty_out);
                ),
            // TODO: Unsize custom types containing a ?Size generic - See the Unsize trait
            //(Path,
            //    ),
            //(Generic,
            //    ),
            (Slice,
                if( ty_in.m_data.is_Array() )
                {
                    const auto& in_array = ty_in.m_data.as_Array();
                    auto size_lval = m_builder.lvalue_or_temp( ::HIR::TypeRef(::HIR::CoreType::Usize), ::MIR::Constant( static_cast<uint64_t>(in_array.size_val) ) );
                    m_builder.set_result( node.span(), ::MIR::RValue::make_MakeDst({ mv$(ptr_lval), mv$(size_lval) }) );
                }
                else if( ty_in.m_data.is_Generic() )
                {
                    // HACK: FixedSizeArray uses `A: Unsize<[T]>` which will lead to the above code not working (as the size isn't known).
                    // - Maybe _Meta on the `&A` would work as a stopgap (since A: Sized, it won't collide with &[T] or similar)
                    auto size_lval = m_builder.lvalue_or_temp( ::HIR::TypeRef(::HIR::CoreType::Usize), ::MIR::RValue::make_DstMeta({ ptr_lval.clone() }) );
                    m_builder.set_result( node.span(), ::MIR::RValue::make_MakeDst({ mv$(ptr_lval), mv$(size_lval) }) );
                }
                else
                {
                    ASSERT_BUG(node.span(), ty_in.m_data.is_Array(), "Unsize to slice from non-array - " << ty_in);
                }
                ),
            (TraitObject,
                // TODO: Obtain the vtable if the destination is a trait object
                // vtable exists as an unnamable associated type

                ::HIR::Path vtable { ty_in.clone(), e.m_trait.m_path.clone(), "#vtable" };
                ::HIR::TypeRef  vtable_type { {} };
                auto vtable_lval = m_builder.lvalue_or_temp(
                    ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, mv$(vtable_type)),
                    ::MIR::RValue( ::MIR::Constant::make_ItemAddr(mv$(vtable)) )
                    );
                
                m_builder.set_result( node.span(), ::MIR::RValue::make_MakeDst({ mv$(ptr_lval), mv$(vtable_lval) }) );
                )
            )
        }
        void visit(::HIR::ExprNode_Index& node) override
        {
            TRACE_FUNCTION_F("_Index");
            
            // NOTE: Calculate the index first (so if it borrows from the source, it's over by the time that's needed)
            const auto& ty_idx = node.m_index->m_res_type;
            this->visit_node_ptr(node.m_index);
            auto index = m_builder.lvalue_or_temp( ty_idx, m_builder.get_result(node.m_index->span()) );
            
            const auto& ty_val = node.m_value->m_res_type;
            this->visit_node_ptr(node.m_value);
            auto value = m_builder.lvalue_or_temp( ty_val, m_builder.get_result(node.m_value->span()) );
           
            ::MIR::RValue   limit_val;
            TU_MATCH_DEF(::HIR::TypeRef::Data, (ty_val.m_data), (e),
            (
                BUG(node.span(), "Indexing unsupported type " << ty_val);
                ),
            (Array,
                limit_val = ::MIR::Constant( e.size_val );
                ),
            (Slice,
                limit_val = ::MIR::RValue::make_DstMeta({ value.clone() });
                )
            )
            
            TU_MATCH_DEF(::HIR::TypeRef::Data, (ty_idx.m_data), (e),
            (
                BUG(node.span(), "Indexing using unsupported index type " << ty_idx);
                ),
            (Primitive,
                if( e != ::HIR::CoreType::Usize ) {
                    BUG(node.span(), "Indexing using unsupported index type " << ty_idx);
                }
                )
            )
            
            // Range checking (DISABLED)
            if( false )
            {
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
            const Span& sp = node.span();
            TRACE_FUNCTION_F("_Deref");
            
            const auto& ty_val = node.m_value->m_res_type;
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.lvalue_or_temp( ty_val, m_builder.get_result(node.m_value->span()) );
            
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty_val.m_data), (te),
            (
                BUG(sp, "Deref on unsupported type - " << ty_val);
                ),
            //(Array,
            //    ),
            (Pointer,
                ),
            (Borrow,
                )
            )
            
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
            TRACE_FUNCTION_F("_CallValue " << node.m_value->m_res_type);
            
            // _CallValue is ONLY valid on function pointers (all others must be desugared)
            ASSERT_BUG(node.span(), node.m_value->m_res_type.m_data.is_Function(), "Leftover _CallValue on a non-fn()");
            this->visit_node_ptr(node.m_value);
            auto fcn_val = m_builder.lvalue_or_temp( node.m_value->m_res_type, m_builder.get_result(node.m_value->span()) );
            
            ::std::vector< ::MIR::LValue>   values;
            values.reserve( node.m_args.size() );
            for(auto& arg : node.m_args)
            {
                this->visit_node_ptr(arg);
                values.push_back( m_builder.lvalue_or_temp( arg->m_res_type, m_builder.get_result(arg->span()) ) );
            }
            
            
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
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            // TODO: Allow use on trait objects.
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
                case ::HIR::CoreType::Char:
                    m_builder.set_result(node.span(), ::MIR::RValue( ::MIR::Constant( static_cast<uint64_t>(e.m_value) ) ));
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
            TRACE_FUNCTION_F("_Closure - " << node.m_obj_path);
            
            // Emit construction of the closure.
            ::std::vector< ::MIR::LValue>   vals;
            for( const auto cap_idx : node.m_var_captures )
            {
                ASSERT_BUG(node.span(), cap_idx < m_variable_types.size(), "Capture #" << cap_idx << " not in variable set (" << m_variable_types.size() << " total)");
                if( node.m_is_move ) {
                    vals.push_back( ::MIR::LValue::make_Variable(cap_idx) );
                }
                else {
                    auto borrow_ty = ::HIR::BorrowType::Shared;
                    auto lval = m_builder.lvalue_or_temp(
                        ::HIR::TypeRef::new_borrow(borrow_ty, m_variable_types[cap_idx].clone()),
                        ::MIR::RValue::make_Borrow({ 0, borrow_ty, ::MIR::LValue::make_Variable(cap_idx) })
                        );
                    vals.push_back( mv$(lval) );
                }
            }
            
            m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                node.m_obj_path.clone(),
                mv$(vals)
                }) );
        }
    };
}


::MIR::FunctionPointer LowerMIR(const ::HIR::ExprPtr& ptr, const ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >& args)
{
    ::MIR::Function fcn;
    
    ExprVisitor_Conv    ev { fcn, ptr.m_bindings };
    
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
        void visit_expr(::HIR::ExprPtr& exp) override {
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

// --------------------------------------------------------------------
// MirBuilder
// --------------------------------------------------------------------
::MIR::LValue MirBuilder::new_temporary(const ::HIR::TypeRef& ty)
{
    unsigned int rv = m_output.temporaries.size();
    m_output.temporaries.push_back( ty.clone() );
    return ::MIR::LValue::make_Temporary({rv});
}
::MIR::LValue MirBuilder::lvalue_or_temp(const ::HIR::TypeRef& ty, ::MIR::RValue val)
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

::MIR::RValue MirBuilder::get_result(const Span& sp)
{
    if(!m_result_valid) {
        BUG(sp, "No value avaliable");
    }
    auto rv = mv$(m_result);
    m_result_valid = false;
    return rv;
}

::MIR::LValue MirBuilder::get_result_lvalue(const Span& sp)
{
    auto rv = get_result(sp);
    TU_IFLET(::MIR::RValue, rv, Use, e,
        return mv$(e);
    )
    else {
        BUG(sp, "LValue expected, got RValue");
    }
}
void MirBuilder::set_result(const Span& sp, ::MIR::RValue val)
{
    if(m_result_valid) {
        BUG(sp, "Pushing a result over an existing result");
    }
    m_result = mv$(val);
    m_result_valid = true;
}

void MirBuilder::push_stmt_assign(::MIR::LValue dst, ::MIR::RValue val)
{
    ASSERT_BUG(Span(), m_block_active, "Pushing statement with no active block");
    m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Assign({ mv$(dst), mv$(val) }) );
}
void MirBuilder::push_stmt_drop(::MIR::LValue val)
{
    ASSERT_BUG(Span(), m_block_active, "Pushing statement with no active block");
    m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Drop({ ::MIR::eDropKind::DEEP, mv$(val) }) );
}

void MirBuilder::set_cur_block(unsigned int new_block)
{
    if( m_block_active ) {
        BUG(Span(), "Updating block when previous is active");
    }
    m_current_block = new_block;
    m_block_active = true;
}
void MirBuilder::end_block(::MIR::Terminator term)
{
    if( !m_block_active ) {
        BUG(Span(), "Terminating block when none active");
    }
    m_output.blocks.at(m_current_block).terminator = mv$(term);
    m_block_active = false;
    m_current_block = 0;
}
void MirBuilder::pause_cur_block()
{
    if( !m_block_active ) {
        BUG(Span(), "Pausing block when none active");
    }
    m_block_active = false;
    m_current_block = 0;
}
::MIR::BasicBlockId MirBuilder::new_bb_linked()
{
    auto rv = new_bb_unlinked();
    end_block( ::MIR::Terminator::make_Goto(rv) );
    set_cur_block(rv);
    return rv;
}
::MIR::BasicBlockId MirBuilder::new_bb_unlinked()
{
    auto rv = m_output.blocks.size();
    m_output.blocks.push_back({});
    return rv;
}

// --------------------------------------------------------------------

void HIR_GenerateMIR(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

