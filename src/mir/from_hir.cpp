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
#include <hir_typeck/common.hpp>   // monomorphise_type
#include "main_bindings.hpp"
#include "from_hir.hpp"


namespace {
    
    class ExprVisitor_Conv:
        public MirConverter
    {
        MirBuilder& m_builder;
        
        const ::std::vector< ::HIR::TypeRef>&  m_variable_types;
        
        struct LoopDesc {
            ScopeHandle scope;
            ::std::string   label;
            unsigned int    cur;
            unsigned int    next;
        };
        ::std::vector<LoopDesc> m_loop_stack;
        
    public:
        ExprVisitor_Conv(MirBuilder& builder, const ::std::vector< ::HIR::TypeRef>& var_types):
            m_builder(builder),
            m_variable_types(var_types)
        {
        }
        
        void destructure_from(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval, bool allow_refutable=false) override
        {
            destructure_from_ex(sp, pat, mv$(lval), (allow_refutable ? 1 : 0));
        }
        
        // Brings variables defined in `pat` into scope
        void define_vars_from(const Span& sp, const ::HIR::Pattern& pat) override
        {
            if( pat.m_binding.is_valid() ) {
                m_builder.define_variable( pat.m_binding.m_slot );
            }
            
            TU_MATCHA( (pat.m_data), (e),
            (Any,
                ),
            (Box,
                define_vars_from(sp, *e.sub);
                ),
            (Ref,
                define_vars_from(sp, *e.sub);
                ),
            (Tuple,
                for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                {
                    define_vars_from(sp, e.sub_patterns[i]);
                }
                ),
            (SplitTuple,
                BUG(sp, "Tuple .. should be eliminated");
                ),
            (StructValue,
                // Nothing.
                ),
            (StructTuple,
                for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                {
                    define_vars_from(sp, e.sub_patterns[i]);
                }
                ),
            (Struct,
                for(const auto& fld_pat : e.sub_patterns)
                {
                    define_vars_from(sp, fld_pat.second);
                }
                ),
            // Refutable
            (Value,
                ),
            (Range,
                ),
            (EnumValue,
                ),
            
            (EnumTuple,
                for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                {
                    define_vars_from(sp, e.sub_patterns[i]);
                }
                ),
            (EnumStruct,
                for(const auto& fld_pat : e.sub_patterns)
                {
                    define_vars_from(sp, fld_pat.second);
                }
                ),
            (Slice,
                for(const auto& subpat : e.sub_patterns)
                {
                    define_vars_from(sp, subpat);
                }
                ),
            (SplitSlice,
                for(const auto& subpat : e.leading)
                {
                    define_vars_from(sp, subpat);
                }
                if( e.extra_bind.is_valid() ) {
                    m_builder.define_variable( e.extra_bind.m_slot );
                }
                for(const auto& subpat : e.trailing)
                {
                    define_vars_from(sp, subpat);
                }
                )
            )
        }
        
        void destructure_from_ex(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval, int allow_refutable=0) // 1 : yes, 2 : disallow binding
        {
            if( allow_refutable != 3 && pat.m_binding.is_valid() ) {
                if( allow_refutable == 2 ) {
                    BUG(sp, "Binding when not expected");
                }
                else if( allow_refutable == 0 ) {
                    ASSERT_BUG(sp, pat.m_data.is_Any(), "Destructure patterns can't bind and match");
                }
                else {
                    // Refutable and binding allowed
                    destructure_from_ex(sp, pat, lval.clone(), 3);
                }
                
                switch( pat.m_binding.m_type )
                {
                case ::HIR::PatternBinding::Type::Move:
                    m_builder.push_stmt_assign( sp, ::MIR::LValue::make_Variable(pat.m_binding.m_slot), mv$(lval) );
                    break;
                case ::HIR::PatternBinding::Type::Ref:
                    m_builder.push_stmt_assign( sp, ::MIR::LValue::make_Variable(pat.m_binding.m_slot), ::MIR::RValue::make_Borrow({
                        0, ::HIR::BorrowType::Shared, mv$(lval)
                        }) );
                    break;
                case ::HIR::PatternBinding::Type::MutRef:
                    m_builder.push_stmt_assign( sp, ::MIR::LValue::make_Variable(pat.m_binding.m_slot), ::MIR::RValue::make_Borrow({
                        0, ::HIR::BorrowType::Unique, mv$(lval)
                        }) );
                    break;
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
                destructure_from_ex(sp, *e.sub, ::MIR::LValue::make_Deref({ box$( mv$(lval) ) }), allow_refutable);
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
            (SplitTuple,
                BUG(sp, "Tuple .. should be eliminated");
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
                // These are only refutable if T is [T]
                bool ty_is_array = false;
                m_builder.with_val_type(sp, lval, [&ty_is_array](const auto& ty){
                    ty_is_array = ty.m_data.is_Array();
                    });
                if( ty_is_array )
                {
                    // TODO: Assert array size
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++)
                    {
                        const auto& subpat = e.sub_patterns[i];
                        destructure_from_ex(sp, subpat, ::MIR::LValue::make_Field({ box$(lval.clone()), i }), allow_refutable );
                    }
                }
                else
                {
                    ASSERT_BUG(sp, allow_refutable, "Refutable pattern not expected - " << pat);
                    
                    // TODO: Emit code to triple-check the size? Or just assume that match did that correctly.
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++)
                    {
                        const auto& subpat = e.sub_patterns[i];
                        destructure_from_ex(sp, subpat, ::MIR::LValue::make_Field({ box$(lval.clone()), i }), allow_refutable );
                    }
                }
                ),
            (SplitSlice,
                // These are only refutable if T is [T]
                bool ty_is_array = false;
                m_builder.with_val_type(sp, lval, [&ty_is_array](const auto& ty){
                    ty_is_array = ty.m_data.is_Array();
                    });
                if( ty_is_array )
                {
                    // TODO: Assert array size
                    //for(unsigned int i = 0; i < e.leading.size(); i ++)
                    //{
                    //    auto idx = 0 + i;
                    //    destructure_from_ex(sp, e.leading[i], ::MIR::LValue::make_Index({ box$( lval.clone() ), box$(lval_idx) }), allow_refutable );
                    //}
                    //for(unsigned int i = 0; i < e.trailing.size(); i ++)
                    //{
                    //    auto idx = 0 + i;
                    //    destructure_from_ex(sp, e.leading[i], ::MIR::LValue::make_Index({ box$( lval.clone() ), box$(lval_idx) }), allow_refutable );
                    //}
                    TODO(sp, "Destructure array using SplitSlice - " << pat);
                }
                else
                {
                    ASSERT_BUG(sp, allow_refutable, "Refutable pattern not expected - " << pat);
                    TODO(sp, "Destructure slice using SplitSlice - " << pat);
                }
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
                bool res_valid;
                ::MIR::RValue   res;
                
                auto scope = m_builder.new_scope_var(node.span());
                
                for(unsigned int i = 0; i < node.m_nodes.size()-1; i ++)
                {
                    auto& subnode = node.m_nodes[i];
                    const Span& sp = subnode->span();
                    
                    auto stmt_scope = m_builder.new_scope_temp(sp);
                    this->visit_node_ptr(subnode);
                    if( m_builder.has_result() ) {
                        m_builder.get_result(sp);
                    }
                    
                    if( m_builder.block_active() ) {
                        m_builder.terminate_scope(sp, mv$(stmt_scope));
                    }
                    else {
                        auto _ = mv$(stmt_scope);
                    }
                }
                
                // - For the last node, don't bother with a statement scope
                {
                    auto& subnode = node.m_nodes.back();
                    const Span& sp = subnode->span();
                    
                    auto stmt_scope = m_builder.new_scope_temp(sp);
                    this->visit_node_ptr(subnode);
                    if( m_builder.has_result() || m_builder.block_active() ) {
                        ASSERT_BUG(sp, m_builder.block_active(), "Result yielded, but no active block");
                        ASSERT_BUG(sp, m_builder.has_result(), "Active block but no result yeilded");
                        // PROBLEM:
                        res = m_builder.get_result(sp);
                        m_builder.terminate_scope(sp, mv$(stmt_scope));
                        res_valid = true;
                    }
                    else {
                        auto _ = mv$(stmt_scope);
                        res_valid = false;
                    }
                }
                
                // Drop all bindings introduced during this block.
                if( m_builder.block_active() ) {
                    m_builder.terminate_scope( node.span(), mv$(scope) );
                }
                else {
                    auto _ = mv$(scope);
                }
                
                // Result from last node (if it didn't diverge)
                if( res_valid ) {
                    if( node.m_yields_final ) {
                        m_builder.set_result( node.span(), mv$(res) );
                    }
                    else {
                        m_builder.set_result(node.span(), ::MIR::RValue::make_Tuple({}));
                    }
                }
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
            
            m_builder.push_stmt_assign( node.span(), ::MIR::LValue::make_Return({}),  m_builder.get_result(node.span()) );
            m_builder.terminate_scope_early( node.span(), m_builder.fcn_scope() );
            m_builder.end_block( ::MIR::Terminator::make_Return({}) );
        }
        void visit(::HIR::ExprNode_Let& node) override
        {
            TRACE_FUNCTION_F("_Let");
            this->define_vars_from(node.span(), node.m_pattern);
            if( node.m_value )
            {
                this->visit_node_ptr(node.m_value);
                
                this->destructure_from(node.span(), node.m_pattern, m_builder.get_result_in_lvalue(node.m_value->span(), node.m_type));
            }
            m_builder.set_result(node.span(), ::MIR::RValue::make_Tuple({}));
        }
        void visit(::HIR::ExprNode_Loop& node) override
        {
            TRACE_FUNCTION_FR("_Loop", "_Loop");
            auto loop_body_scope = m_builder.new_scope_loop(node.span());
            auto loop_block = m_builder.new_bb_linked();
            auto loop_next = m_builder.new_bb_unlinked();
            
            m_loop_stack.push_back( LoopDesc { mv$(loop_body_scope), node.m_label, loop_block, loop_next } );
            this->visit_node_ptr(node.m_code);
            auto loop_scope = mv$(m_loop_stack.back().scope);
            m_loop_stack.pop_back();
            
            // If there's a stray result, drop it
            if( m_builder.has_result() ) {
                assert( m_builder.block_active() );
                // TODO: Properly drop this? Or just discard it?
                m_builder.get_result(node.span());
            }
            // Terminate block with a jump back to the start
            // - Also inserts the jump if this didn't uncondtionally diverge
            if( m_builder.block_active() )
            {
                DEBUG("- Reached end, loop back");
                // Insert drop of all scopes within the current scope
                m_builder.terminate_scope( node.span(), mv$(loop_scope) );
                m_builder.end_block( ::MIR::Terminator::make_Goto(loop_block) );
            }
            else
            {
                // Terminate scope without emitting cleanup (cleanup was handled by `break`)
                m_builder.terminate_scope( node.span(), mv$(loop_scope), false );
            }
            
            if( ! node.m_diverges )
            {
                DEBUG("- Doesn't diverge");
                m_builder.set_cur_block(loop_next);
                m_builder.set_result(node.span(), ::MIR::RValue::make_Tuple({{}}));
            }
            else
            {
                DEBUG("- Diverges");
                assert( !m_builder.has_result() );
                
                m_builder.set_cur_block(loop_next);
                m_builder.end_split_arm_early(node.span());
                assert( !m_builder.has_result() );
                m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
            }
        }
        void visit(::HIR::ExprNode_LoopControl& node) override
        {
            TRACE_FUNCTION_F("_LoopControl \"" << node.m_label << "\"");
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
            
            // TODO: Insert drop of all active scopes within the loop
            m_builder.terminate_scope_early( node.span(), target_block->scope );
            if( node.m_continue ) {
                m_builder.end_block( ::MIR::Terminator::make_Goto(target_block->cur) );
            }
            else {
                m_builder.end_block( ::MIR::Terminator::make_Goto(target_block->next) );
            }
        }
        
        void visit(::HIR::ExprNode_Match& node) override
        {
            TRACE_FUNCTION_FR("_Match", "_Match");
            this->visit_node_ptr(node.m_value);
            auto match_val = m_builder.get_result_in_lvalue(node.m_value->span(), node.m_value->m_res_type);
            
            if( node.m_arms.size() == 0 ) {
                // Nothing
                //const auto& ty = node.m_value->m_res_type;
                // TODO: Ensure that the type is a zero-variant enum or !
                m_builder.end_split_arm_early(node.span());
                m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
            }
            else if( node.m_arms.size() == 1 && node.m_arms[0].m_patterns.size() == 1 && ! node.m_arms[0].m_cond ) {
                // - Shortcut: Single-arm match
                auto scope = m_builder.new_scope_var( node.span() );
                this->define_vars_from(node.span(), node.m_arms[0].m_patterns[0]);
                this->destructure_from(node.span(), node.m_arms[0].m_patterns[0], mv$(match_val));
                this->visit_node_ptr(node.m_arms[0].m_code);
                if( m_builder.block_active() ) {
                    m_builder.terminate_scope( node.span(), mv$(scope) );
                }
                else {
                    auto _ = mv$(scope);
                }
            }
            else {
                MIR_LowerHIR_Match(m_builder, *this, node, mv$(match_val));
            }
        } // ExprNode_Match
        
        void visit(::HIR::ExprNode_If& node) override
        {
            TRACE_FUNCTION_FR("_If", "_If");
            
            this->visit_node_ptr(node.m_cond);
            auto decision_val = m_builder.get_result_in_lvalue(node.m_cond->span(), node.m_cond->m_res_type);
            
            auto true_branch = m_builder.new_bb_unlinked();
            auto false_branch = m_builder.new_bb_unlinked();
            auto next_block = m_builder.new_bb_unlinked();
            m_builder.end_block( ::MIR::Terminator::make_If({ mv$(decision_val), true_branch, false_branch }) );
            
            auto result_val = m_builder.new_temporary(node.m_res_type);
            
            // Scope handles cases where one arm moves a value but the other doesn't
            auto scope = m_builder.new_scope_split( node.m_true->span() );
            
            // 'true' branch
            {
                m_builder.set_cur_block(true_branch);
                this->visit_node_ptr(node.m_true);
                if( m_builder.block_active() || m_builder.has_result() ) {
                    m_builder.push_stmt_assign( node.span(), result_val.clone(), m_builder.get_result(node.m_true->span()) );
                    m_builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
                    m_builder.end_split_arm(node.span(), scope, true);
                }
                else {
                    m_builder.end_split_arm(node.span(), scope, false);
                }
            }
            
            // 'false' branch
            m_builder.set_cur_block(false_branch);
            if( node.m_false )
            {
                this->visit_node_ptr(node.m_false);
                if( m_builder.block_active() )
                {
                    m_builder.push_stmt_assign( node.span(), result_val.clone(), m_builder.get_result(node.m_false->span()) );
                    m_builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
                    m_builder.end_split_arm(node.span(), scope, true);
                }
                else {
                    m_builder.end_split_arm(node.span(), scope, false);
                }
            }
            else
            {
                // Assign `()` to the result
                m_builder.push_stmt_assign(node.span(),  result_val.clone(), ::MIR::RValue::make_Tuple({}) );
                m_builder.end_block( ::MIR::Terminator::make_Goto(next_block) );
                m_builder.end_split_arm(node.span(), scope, true);
            }
            m_builder.set_cur_block(next_block);
            m_builder.terminate_scope( node.span(), mv$(scope) );
            
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
                m_builder.push_stmt_assign(sp, mv$(res_slot), ::MIR::RValue::make_BinOp({ mv$(val_l), op, mv$(val_r) }));
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
                m_builder.push_stmt_assign(sp, mv$(res_slot), ::MIR::RValue::make_BinOp({ mv$(val_l), op, mv$(val_r) }));
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
                m_builder.push_stmt_assign(sp, mv$(res_slot), ::MIR::RValue::make_BinOp({ mv$(val_l), op, mv$(val_r) }));
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
                m_builder.push_stmt_assign(sp, mv$(res_slot), ::MIR::RValue::make_BinOp({ mv$(val_l), op, mv$(val_r) }));
                break;
            }
        }
        
        void visit(::HIR::ExprNode_Assign& node) override
        {
            TRACE_FUNCTION_F("_Assign");
            const auto& sp = node.span();
            
            this->visit_node_ptr(node.m_value);
            ::MIR::RValue val = m_builder.get_result(sp);
            
            this->visit_node_ptr(node.m_slot);
            auto dst = m_builder.get_result_unwrap_lvalue(sp);
            
            const auto& ty_slot = node.m_slot->m_res_type;
            const auto& ty_val  = node.m_value->m_res_type;
            
            if( node.m_op != ::HIR::ExprNode_Assign::Op::None )
            {
                auto dst_clone = dst.clone();
                auto val_lv = m_builder.lvalue_or_temp( node.span(), ty_val, mv$(val) );
                
                ASSERT_BUG(sp, ty_slot.m_data.is_Primitive(), "Assignment operator overloads are only valid on primitives - ty_slot="<<ty_slot);
                ASSERT_BUG(sp, ty_val.m_data.is_Primitive(), "Assignment operator overloads are only valid on primitives - ty_val="<<ty_val);
                
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
            }
            else
            {
                ASSERT_BUG(sp, ty_slot == ty_val, "Types must match for assignment - " << ty_slot << " != " << ty_val);
                m_builder.push_stmt_assign(node.span(), mv$(dst), mv$(val));
            }
            m_builder.set_result(node.span(), ::MIR::RValue::make_Tuple({}));
        }
        
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            const auto& sp = node.span();
            TRACE_FUNCTION_F("_BinOp");
            
            const auto& ty_l = node.m_left->m_res_type;
            this->visit_node_ptr(node.m_left);
            auto left = m_builder.get_result_in_lvalue(node.m_left->span(), ty_l);
            
            const auto& ty_r = node.m_right->m_res_type;
            this->visit_node_ptr(node.m_right);
            auto right = m_builder.get_result_in_lvalue(node.m_right->span(), ty_r);
            
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
                m_builder.push_stmt_assign(node.span(), res.clone(), ::MIR::RValue( ::MIR::Constant::make_Bool(false) ));
                m_builder.end_block( ::MIR::Terminator::make_Goto(bb_next) );
                
                // If left is true, assign result to right
                m_builder.set_cur_block( bb_true );
                m_builder.push_stmt_assign(node.span(), res.clone(), mv$(right));    // TODO: Right doens't need to be an LValue here.
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
                m_builder.push_stmt_assign(node.span(), res.clone(), ::MIR::RValue( ::MIR::Constant::make_Bool(true) ));
                m_builder.end_block( ::MIR::Terminator::make_Goto(bb_next) );
                
                // If left is false, assign result to right
                m_builder.set_cur_block( bb_false );
                m_builder.push_stmt_assign(node.span(), res.clone(), mv$(right));    // TODO: Right doens't need to be an LValue here.
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
            auto val = m_builder.get_result_in_lvalue(node.m_value->span(), ty_val);
            
            ::MIR::RValue   res;
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
                res = ::MIR::RValue::make_UniOp({ mv$(val), ::MIR::eUniOp::INV });
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
                res = ::MIR::RValue::make_UniOp({ mv$(val), ::MIR::eUniOp::NEG });
                break;
            }
            m_builder.set_result( node.span(), mv$(res) );
        }
        void visit(::HIR::ExprNode_Borrow& node) override
        {
            TRACE_FUNCTION_F("_Borrow");
            
            const auto& ty_val = node.m_value->m_res_type;
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.get_result_in_lvalue(node.m_value->span(), ty_val);
            
            auto res = m_builder.new_temporary(node.m_res_type);
            m_builder.push_stmt_assign( node.span(), res.as_Temporary(), ::MIR::RValue::make_Borrow({ 0, node.m_type, mv$(val) }));
            m_builder.set_result( node.span(), mv$(res) );
        }
        void visit(::HIR::ExprNode_Cast& node) override
        {
            TRACE_FUNCTION_F("_Cast");
            this->visit_node_ptr(node.m_value);
            
            const auto& ty_out = node.m_res_type;
            const auto& ty_in = node.m_value->m_res_type;
            
            if( ty_out == ty_in ) {
                return ;
            }
            
            auto val = m_builder.get_result_in_lvalue(node.m_value->span(), node.m_value->m_res_type);
            
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty_out.m_data), (de),
            (
                BUG(node.span(), "Invalid cast to " << ty_out << " from " << ty_in);
                ),
            (Pointer,
                if( ty_in.m_data.is_Primitive() ) {
                    const auto& ie = ty_in.m_data.as_Primitive();
                    switch(ie)
                    {
                    case ::HIR::CoreType::Bool:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        BUG(node.span(), "Cannot cast to pointer from " << ty_in);
                    default:
                        break;
                    }
                    // TODO: Only valid if T: Sized in *{const/mut/move} T
                }
                else TU_IFLET( ::HIR::TypeRef::Data, ty_in.m_data, Borrow, se,
                    if( *de.inner != *se.inner ) {
                        BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                    }
                    // Valid
                )
                else TU_IFLET( ::HIR::TypeRef::Data, ty_in.m_data, Function, se,
                    if( *de.inner != ::HIR::TypeRef::new_unit() ) {
                        BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                    }
                    // Valid
                )
                else TU_IFLET( ::HIR::TypeRef::Data, ty_in.m_data, Pointer, se,
                    // Valid
                )
                else {
                    BUG(node.span(), "Cannot cast to pointer from " << ty_in);
                }
                ),
            (Primitive,
                switch(de)
                {
                case ::HIR::CoreType::Str:
                    BUG(node.span(), "Cannot cast to str");
                    break;
                case ::HIR::CoreType::Char:
                    if( ty_in.m_data.is_Primitive() && ty_in.m_data.as_Primitive() == ::HIR::CoreType::U8 ) {
                        // Valid
                    }
                    else {
                        BUG(node.span(), "Cannot cast to char from " << ty_in);
                    }
                    break;
                case ::HIR::CoreType::Bool:
                    BUG(node.span(), "Cannot cast to bool");
                    break;
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    TU_IFLET(::HIR::TypeRef::Data, ty_in.m_data, Primitive, se,
                        switch(de)
                        {
                        case ::HIR::CoreType::Str:
                        case ::HIR::CoreType::Char:
                        case ::HIR::CoreType::Bool:
                            BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                            break;
                        default:
                            // Valid
                            break;
                        }
                    )
                    else {
                        BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                    }
                    break;
                default:
                    TU_IFLET(::HIR::TypeRef::Data, ty_in.m_data, Primitive, se,
                        switch(de)
                        {
                        case ::HIR::CoreType::Str:
                            BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                        default:
                            // Valid
                            break;
                        }
                    )
                    else TU_IFLET(::HIR::TypeRef::Data, ty_in.m_data, Path, se,
                        TU_IFLET(::HIR::TypeRef::TypePathBinding, se.binding, Enum, pbe,
                            // TODO: Check if it's a repr(ty/C) enum - and if the type matches
                        )
                        else {
                            BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                        }
                    )
                    // NOTE: Valid for all integer types
                    else if( ty_in.m_data.is_Pointer() ) {
                        // TODO: Only valid for T: Sized?
                    }
                    else if( de == ::HIR::CoreType::Usize && ty_in.m_data.is_Function() ) {
                        // TODO: Always valid?
                    }
                    else {
                        BUG(node.span(), "Cannot cast to " << ty_out << " from " << ty_in);
                    }
                    break;
                }
                )
            )
            auto res = m_builder.new_temporary(node.m_res_type);
            m_builder.push_stmt_assign(node.span(), res.clone(), ::MIR::RValue::make_Cast({ mv$(val), node.m_res_type.clone() }));
            m_builder.set_result( node.span(), mv$(res) );
        }
        void visit(::HIR::ExprNode_Unsize& node) override
        {
            TRACE_FUNCTION_F("_Unsize");
            this->visit_node_ptr(node.m_value);
            
            const auto& ty_out = node.m_res_type;
            const auto& ty_in = node.m_value->m_res_type;
            
            if( ty_out == ty_in ) {
                return ;
            }
            
            auto ptr_lval = m_builder.get_result_in_lvalue(node.m_value->span(), node.m_value->m_res_type);
            
            if( ty_out.m_data.is_Borrow() && ty_in.m_data.is_Borrow() )
            {
                const auto& oe = ty_out.m_data.as_Borrow();
                const auto& ie = ty_in.m_data.as_Borrow();
                const auto& ty_out = *oe.inner;
                const auto& ty_in = *ie.inner;
                TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty_out.m_data), (e),
                (
                    const auto& lang_Unsize = m_builder.crate().get_lang_item_path(node.span(), "unsize");
                    if( m_builder.resolve().find_impl( node.span(), lang_Unsize, ::HIR::PathParams(ty_out.clone()), ty_in.clone(), [](auto ){ return true; }) )
                    {
                        // - HACK: Emit a cast operation on the pointers. Leave it up to monomorph to 'fix' it
                        m_builder.set_result( node.span(), ::MIR::RValue::make_Cast({ mv$(ptr_lval), node.m_res_type.clone() }) );
                    }
                    else
                    {
                        // Probably an error.
                        TODO(node.span(), "MIR _Unsize to " << ty_out);
                    }
                    ),
                (Slice,
                    if( ty_in.m_data.is_Array() )
                    {
                        const auto& in_array = ty_in.m_data.as_Array();
                        auto size_lval = m_builder.lvalue_or_temp( node.span(), ::HIR::TypeRef(::HIR::CoreType::Usize), ::MIR::Constant( static_cast<uint64_t>(in_array.size_val) ) );
                        m_builder.set_result( node.span(), ::MIR::RValue::make_MakeDst({ mv$(ptr_lval), mv$(size_lval) }) );
                    }
                    else if( ty_in.m_data.is_Generic() )
                    {
                        // HACK: FixedSizeArray uses `A: Unsize<[T]>` which will lead to the above code not working (as the size isn't known).
                        // - Maybe _Meta on the `&A` would work as a stopgap (since A: Sized, it won't collide with &[T] or similar)
                        auto size_lval = m_builder.lvalue_or_temp( node.span(), ::HIR::TypeRef(::HIR::CoreType::Usize), ::MIR::RValue::make_DstMeta({ ptr_lval.clone() }) );
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
                        node.span(),
                        ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, mv$(vtable_type)),
                        ::MIR::RValue( ::MIR::Constant::make_ItemAddr(mv$(vtable)) )
                        );
                    
                    m_builder.set_result( node.span(), ::MIR::RValue::make_MakeDst({ mv$(ptr_lval), mv$(vtable_lval) }) );
                    )
                )
            }
            else
            {
                // NOTES: (from IRC: eddyb)
                // < eddyb> they're required that T and U are the same struct definition (with different type parameters) and exactly one field differs in type between T and U (ignoring PhantomData)
                // < eddyb> Mutabah: I forgot to mention that the field that differs in type must also impl CoerceUnsized

                // TODO: Just emit a cast and leave magic handling to codegen
                // - This code _could_ do inspection of the types and insert a destructure+unsize+restructure, but that does't handle direct `T: CoerceUnsize<U>`
                m_builder.set_result( node.span(), ::MIR::RValue::make_Cast({ mv$(ptr_lval), node.m_res_type.clone() }) );
            }
        }
        void visit(::HIR::ExprNode_Index& node) override
        {
            TRACE_FUNCTION_F("_Index");
            
            // NOTE: Calculate the index first (so if it borrows from the source, it's over by the time that's needed)
            const auto& ty_idx = node.m_index->m_res_type;
            this->visit_node_ptr(node.m_index);
            auto index = m_builder.get_result_in_lvalue(node.m_index->span(), ty_idx);
            
            const auto& ty_val = node.m_value->m_res_type;
            this->visit_node_ptr(node.m_value);
            auto value = m_builder.get_result_in_lvalue(node.m_value->span(), ty_val);
           
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
                auto limit_lval = m_builder.lvalue_or_temp( node.span(), ty_idx, mv$(limit_val) );
                
                auto cmp_res = m_builder.new_temporary( ::HIR::CoreType::Bool );
                m_builder.push_stmt_assign(node.span(), cmp_res.clone(), ::MIR::RValue::make_BinOp({ index.clone(), ::MIR::eBinOp::GE, mv$(limit_lval) }));
                auto arm_panic = m_builder.new_bb_unlinked();
                auto arm_continue = m_builder.new_bb_unlinked();
                m_builder.end_block( ::MIR::Terminator::make_If({ mv$(cmp_res), arm_panic, arm_continue }) );
                
                m_builder.set_cur_block( arm_panic );
                // TODO: Call an "index fail" method which always panics.
                //m_builder.end_block( ::MIR::Terminator::make_Panic({}) );
                m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
                
                m_builder.set_cur_block( arm_continue );
            }
            
            m_builder.set_result( node.span(), ::MIR::LValue::make_Index({ box$(value), box$(index) }) );
        }
        
        void visit(::HIR::ExprNode_Deref& node) override
        {
            const Span& sp = node.span();
            TRACE_FUNCTION_F("_Deref");
            
            const auto& ty_val = node.m_value->m_res_type;
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.get_result_in_lvalue(node.m_value->span(), ty_val);
            
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty_val.m_data), (te),
            (
                if( m_builder.is_type_owned_box( ty_val ) )
                {
                    // Box magically derefs.
                    // HACK: Break out of the switch used for TU_MATCH_DEF
                    break;
                }
                BUG(sp, "Deref on unsupported type - " << ty_val);
                ),
            (Pointer,
                // Deref on a pointer - TODO: Requires unsafe
                ),
            (Borrow,
                // Deref on a borrow - Always valid... assuming borrowck is there :)
                )
            )
            
            m_builder.set_result( node.span(), ::MIR::LValue::make_Deref({ box$(val) }) );
        }
        
        void visit(::HIR::ExprNode_Emplace& node) override
        {
            if( node.m_type == ::HIR::ExprNode_Emplace::Type::Noop ) {
                return node.m_value->visit(*this);
            }
            //auto path_Placer = ::HIR::SimplePath("core", {"ops", "Placer"});
            auto path_BoxPlace = ::HIR::SimplePath("core", {"ops", "BoxPlace"});
            auto path_Place = ::HIR::SimplePath("core", {"ops", "Place"});
            auto path_Boxed = ::HIR::SimplePath("core", {"ops", "Boxed"});
            //auto path_InPlace = ::HIR::SimplePath("core", {"ops", "InPlace"});
            
            const auto& data_ty = node.m_value->m_res_type;
            
            // 1. Obtain the type of the `place` variable
            ::HIR::TypeRef  place_type;
            switch( node.m_type )
            {
            case ::HIR::ExprNode_Emplace::Type::Noop:
                throw "";
            case ::HIR::ExprNode_Emplace::Type::Boxer: {
                place_type = ::HIR::TypeRef::new_path( ::HIR::Path(node.m_res_type.clone(), ::HIR::GenericPath(path_Boxed), "Place", {}), {} );
                m_builder.resolve().expand_associated_types( node.span(), place_type );
                break; }
            case ::HIR::ExprNode_Emplace::Type::Placer:
                TODO(node.span(), "_Emplace - Placer");
                break;
            }
            
            // 2. Initialise the place
            auto place = m_builder.new_temporary( place_type );
            auto place__panic = m_builder.new_bb_unlinked();
            auto place__ok = m_builder.new_bb_unlinked();
            switch( node.m_type )
            {
            case ::HIR::ExprNode_Emplace::Type::Noop:
                throw "";
            case ::HIR::ExprNode_Emplace::Type::Boxer: {
                auto fcn_ty_data = ::HIR::FunctionType { false, "", box$( place_type.clone() ), {} };
                ::HIR::PathParams   trait_params;
                trait_params.m_types.push_back( data_ty.clone() );
                auto fcn_path = ::HIR::Path(place_type.clone(), ::HIR::GenericPath(path_BoxPlace, mv$(trait_params)), "make_place", {});
                auto fcn_val = m_builder.new_temporary( ::HIR::TypeRef(mv$(fcn_ty_data)) );
                m_builder.push_stmt_assign( node.span(), fcn_val.clone(), ::MIR::RValue::make_Constant( ::MIR::Constant(mv$(fcn_path)) ) );
                m_builder.end_block(::MIR::Terminator::make_Call({
                    place__ok, place__panic,
                    place.clone(), mv$(fcn_val),
                    {}
                    }));
                break; }
            case ::HIR::ExprNode_Emplace::Type::Placer:
                TODO(node.span(), "_Emplace - Placer");
                break;
            }
            
            // TODO: Proper panic handling, including scope destruction
            m_builder.set_cur_block(place__panic);
            // TODO: Drop `place`
            m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
            m_builder.set_cur_block(place__ok);
            
            // 2. Get `place_raw`
            auto place_raw__type = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Unique, node.m_value->m_res_type.clone());
            auto place_raw = m_builder.new_temporary( place_raw__type );
            auto place_raw__panic = m_builder.new_bb_unlinked();
            auto place_raw__ok = m_builder.new_bb_unlinked();
            {
                auto place_refmut__type = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, place_type.clone());
                auto place_refmut = m_builder.lvalue_or_temp(node.span(), place_refmut__type,  ::MIR::RValue::make_Borrow({ 0, ::HIR::BorrowType::Unique, place.clone() }));
                auto fcn_ty_data = ::HIR::FunctionType { false, "", box$( place_raw__type.clone() ), ::make_vec1(mv$(place_refmut__type)) };
                // <typeof(place) as ops::Place>::pointer
                auto fcn_path = ::HIR::Path(place_type.clone(), ::HIR::GenericPath(path_Place), "pointer", {});
                auto fcn_val = m_builder.new_temporary( ::HIR::TypeRef(mv$(fcn_ty_data)) );
                m_builder.push_stmt_assign( node.span(), fcn_val.clone(), ::MIR::RValue::make_Constant( ::MIR::Constant(mv$(fcn_path)) ) );
                m_builder.end_block(::MIR::Terminator::make_Call({
                    place_raw__ok, place_raw__panic,
                    place_raw.clone(), mv$(fcn_val),
                    ::make_vec1( mv$(place_refmut) )
                    }));
            }
            
            // TODO: Proper panic handling, including scope destruction
            m_builder.set_cur_block(place_raw__panic);
            // TODO: Drop `place`
            m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
            m_builder.set_cur_block(place_raw__ok);
            
            
            // 3. Get the value and assign it into `place_raw`
            node.m_value->visit(*this);
            auto val = m_builder.get_result(node.span());
            m_builder.push_stmt_assign( node.span(), ::MIR::LValue::make_Deref({ box$(place_raw.clone()) }), mv$(val) );
            
            // 3. Return a call to `finalize`
            ::HIR::Path  finalize_path(::HIR::GenericPath {});
            switch( node.m_type )
            {
            case ::HIR::ExprNode_Emplace::Type::Noop:
                throw "";
            case ::HIR::ExprNode_Emplace::Type::Boxer:
                finalize_path = ::HIR::Path(node.m_res_type.clone(), ::HIR::GenericPath(path_Boxed), "finalize");
                break;
            case ::HIR::ExprNode_Emplace::Type::Placer:
                TODO(node.span(), "_Emplace - Placer");
                break;
            }
            
            auto res = m_builder.new_temporary( node.m_res_type );
            auto res__panic = m_builder.new_bb_unlinked();
            auto res__ok = m_builder.new_bb_unlinked();
            {
                auto fcn_ty_data = ::HIR::FunctionType { true, "", box$(node.m_res_type.clone()), ::make_vec1(mv$(place_type)) };
                auto fcn_val = m_builder.new_temporary( ::HIR::TypeRef(mv$(fcn_ty_data)) );
                m_builder.push_stmt_assign( node.span(), fcn_val.clone(), ::MIR::RValue::make_Constant( ::MIR::Constant(mv$(finalize_path)) ) );
                m_builder.end_block(::MIR::Terminator::make_Call({
                    res__ok, res__panic,
                    res.clone(), mv$(fcn_val),
                    ::make_vec1( mv$(place) )
                    }));
            }
            
            // TODO: Proper panic handling, including scope destruction
            m_builder.set_cur_block(res__panic);
            // TODO: Should this drop the value written to the rawptr?
            // - No, becuase it's likely invalid now. Goodbye!
            m_builder.end_block( ::MIR::Terminator::make_Diverge({}) );
            m_builder.set_cur_block(res__ok);
            
            m_builder.set_result( node.span(), mv$(res) );
        }
        
        void visit(::HIR::ExprNode_TupleVariant& node) override
        {
            TRACE_FUNCTION_F("_TupleVariant");
            ::std::vector< ::MIR::LValue>   values;
            values.reserve( node.m_args.size() );
            for(auto& arg : node.m_args)
            {
                this->visit_node_ptr(arg);
                values.push_back( m_builder.get_result_in_lvalue(arg->span(), arg->m_res_type) );
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
                values.push_back( m_builder.get_result_in_lvalue(arg->span(), arg->m_res_type) );
                m_builder.moved_lvalue( arg->span(), values.back() );
            }
            
            // TODO: Obtain function type for this function (i.e. a type that is specifically for this function)
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
            m_builder.push_stmt_assign( node.span(), fcn_val.clone(), ::MIR::RValue::make_Constant( ::MIR::Constant(node.m_path.clone()) ) );
            
            auto panic_block = m_builder.new_bb_unlinked();
            auto next_block = m_builder.new_bb_unlinked();
            auto res = m_builder.new_temporary( node.m_res_type );
            m_builder.end_block(::MIR::Terminator::make_Call({
                next_block, panic_block,
                res.clone(), mv$(fcn_val),
                mv$(values)
                }));
            
            m_builder.set_cur_block(panic_block);
            // TODO: Proper panic handling, including scope destruction
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
            auto fcn_val = m_builder.get_result_in_lvalue( node.m_value->span(), node.m_value->m_res_type );
            
            ::std::vector< ::MIR::LValue>   values;
            values.reserve( node.m_args.size() );
            for(auto& arg : node.m_args)
            {
                this->visit_node_ptr(arg);
                values.push_back( m_builder.get_result_in_lvalue(arg->span(), arg->m_res_type) );
                m_builder.moved_lvalue( arg->span(), values.back() );
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
            // TODO: Allow use on trait objects? May not be needed, depends.
            BUG(node.span(), "Leftover _CallMethod");
        }
        void visit(::HIR::ExprNode_Field& node) override
        {
            TRACE_FUNCTION_F("_Field");
            this->visit_node_ptr(node.m_value);
            auto val = m_builder.get_result_in_lvalue(node.m_value->span(), node.m_value->m_res_type);
            
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
            const auto& sp = node.span();
            TRACE_FUNCTION_F("_PathValue - " << node.m_path);
            TU_MATCH( ::HIR::Path::Data, (node.m_path.m_data), (pe),
            (Generic,
                if( node.m_target == ::HIR::ExprNode_PathValue::ENUM_VAR_CONSTR ) {
                    auto enum_path = pe.m_path;
                    enum_path.m_components.pop_back();
                    const auto& var_name = pe.m_path.m_components.back();
                    
                    const auto& enm = m_builder.crate().get_enum_by_path(sp, enum_path);
                    auto var_it = ::std::find_if(enm.m_variants.begin(), enm.m_variants.end(), [&](const auto& x){ return x.first == var_name; });
                    ASSERT_BUG(sp, var_it != enm.m_variants.end(), "Variant " << pe.m_path << " isn't present");
                    const auto& var = var_it->second;
                    ASSERT_BUG(sp, var.is_Tuple(), "Variant " << pe.m_path << " isn't a tuple variant");
                    
                    // TODO: Ideally, the creation of the wrapper function would happen somewhere before this?
                    auto tmp = m_builder.new_temporary( node.m_res_type );
                    m_builder.push_stmt_assign( sp, tmp.clone(), ::MIR::Constant::make_ItemAddr(node.m_path.clone()) );
                    m_builder.set_result( sp, mv$(tmp) );
                    return ;
                }
                const auto& vi = m_builder.crate().get_valitem_by_path(node.span(), pe.m_path);
                TU_MATCHA( (vi), (e),
                (Import,
                    BUG(sp, "All references via imports should be replaced");
                    ),
                (Constant,
                    auto tmp = m_builder.new_temporary( e.m_type );
                    m_builder.push_stmt_assign( sp, tmp.clone(), ::MIR::Constant::make_Const({node.m_path.clone()}) );
                    m_builder.set_result( node.span(), mv$(tmp) );
                    ),
                (Static,
                    m_builder.set_result( node.span(), ::MIR::LValue::make_Static(node.m_path.clone()) );
                    ),
                (StructConstant,
                    // TODO: Why is this still a PathValue?
                    m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                        pe.clone(),
                        {}
                        }) );
                    ),
                (Function,
                    // TODO: Why not use the result type?
                    //auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr, nullptr, &pe.m_params);
                    auto monomorph_cb = [&](const auto& gt)->const auto& {
                        const auto& e = gt.m_data.as_Generic();
                        if( e.binding == 0xFFFF ) {
                            BUG(sp, "Reference to Self in free function - " << gt);
                        }
                        else if( (e.binding >> 8) == 0 ) {
                            BUG(sp, "Reference to impl-level param in free function - " << gt);
                        }
                        else if( (e.binding >> 8) == 1 ) {
                            auto idx = e.binding & 0xFF;
                            if( idx >= pe.m_params.m_types.size() ) {
                                BUG(sp, "Generic param out of input range - " << gt << " >= " << pe.m_params.m_types.size());
                            }
                            return pe.m_params.m_types[idx];
                        }
                        else {
                            BUG(sp, "Unknown param in free function - " << gt);
                        }
                        };
                    
                    // TODO: Obtain function type for this function (i.e. a type that is specifically for this function)
                    auto fcn_ty_data = ::HIR::FunctionType {
                        e.m_unsafe,
                        e.m_abi,
                        box$( monomorphise_type_with(sp, e.m_return, monomorph_cb) ),
                        {}
                        };
                    fcn_ty_data.m_arg_types.reserve( e.m_args.size() );
                    for(const auto& arg : e.m_args)
                    {
                        fcn_ty_data.m_arg_types.push_back( monomorphise_type_with(sp, arg.second, monomorph_cb) );
                    }
                    auto tmp = m_builder.new_temporary( ::HIR::TypeRef( mv$(fcn_ty_data) ) );
                    m_builder.push_stmt_assign( sp, tmp.clone(), ::MIR::Constant::make_ItemAddr(node.m_path.clone()) );
                    m_builder.set_result( sp, mv$(tmp) );
                    ),
                (StructConstructor,
                    // TODO: Ideally, the creation of the wrapper function would happen somewhere before this?
                    auto tmp = m_builder.new_temporary( node.m_res_type );
                    m_builder.push_stmt_assign( sp, tmp.clone(), ::MIR::Constant::make_ItemAddr(node.m_path.clone()) );
                    m_builder.set_result( sp, mv$(tmp) );
                    )
                )
                ),
            (UfcsKnown,
                // Check what item type this is (from the trait)
                const auto& tr = m_builder.crate().get_trait_by_path(sp, pe.trait.m_path);
                auto it = tr.m_values.find(pe.item);
                ASSERT_BUG(sp, it != tr.m_values.end(), "Cannot find trait item for " << node.m_path);
                TU_MATCHA( (it->second), (e),
                (Constant,
                    m_builder.set_result( sp, ::MIR::Constant::make_ItemAddr(node.m_path.clone()) );
                    ),
                (Static,
                    TODO(sp, "Associated statics (non-rustc) - " << node.m_path);
                    ),
                (Function,
                    auto tmp = m_builder.new_temporary( node.m_res_type.clone() );
                    m_builder.push_stmt_assign( sp, tmp.clone(), ::MIR::Constant::make_ItemAddr(node.m_path.clone()) );
                    m_builder.set_result( sp, mv$(tmp) );
                    )
                )
                ),
            (UfcsUnknown,
                BUG(sp, "PathValue - Encountered UfcsUnknown - " << node.m_path);
                ),
            (UfcsInherent,
                // 1. Find item in an impl block
                auto rv = m_builder.crate().find_type_impls(*pe.type, [&](const auto& ty)->const auto& { return ty; },
                    [&](const auto& impl) {
                        DEBUG("- impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                        // Associated functions
                        {
                            auto it = impl.m_methods.find(pe.item);
                            if( it != impl.m_methods.end() ) {
                                m_builder.set_result( sp, ::MIR::Constant::make_ItemAddr(node.m_path.clone()) );
                                return true;
                            }
                        }
                        // Associated consts
                        {
                            auto it = impl.m_constants.find(pe.item);
                            if( it != impl.m_constants.end() ) {
                                m_builder.set_result( sp, ::MIR::Constant::make_Const({node.m_path.clone()}) );
                                return true;
                            }
                        }
                        // Associated static (undef)
                        return false;
                    });
                if( !rv ) {
                    ERROR(sp, E0000, "Failed to locate item for " << node.m_path);
                }
                )
            )
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
                base_val = m_builder.get_result_in_lvalue(node.m_base_value->span(), node.m_base_value->m_res_type);
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
                auto& valnode = ent.second;
                auto idx = ::std::find_if(fields.begin(), fields.end(), [&](const auto&x){ return x.first == ent.first; }) - fields.begin();
                assert( !values_set[idx] );
                values_set[idx] = true;
                this->visit_node_ptr(valnode);
                values.at(idx) = m_builder.lvalue_or_temp( valnode->span(), valnode->m_res_type, m_builder.get_result(valnode->span()) );
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
                    // Partial move support will handle dropping the rest?
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
                values.push_back( m_builder.lvalue_or_temp( arg->span(), arg->m_res_type, m_builder.get_result(arg->span()) ) );
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
                values.push_back( m_builder.lvalue_or_temp( arg->span(), arg->m_res_type, m_builder.get_result(arg->span()) ) );
            }
            
            m_builder.set_result( node.span(), ::MIR::RValue::make_Array({
                mv$(values)
                }) );
        }
        
        void visit(::HIR::ExprNode_ArraySized& node) override
        {
            TRACE_FUNCTION_F("_ArraySized");
            this->visit_node_ptr( node.m_val );
            auto value = m_builder.lvalue_or_temp( node.span(), node.m_val->m_res_type, m_builder.get_result(node.m_val->span()) );
            
            m_builder.set_result( node.span(), ::MIR::RValue::make_SizedArray({
                mv$(value),
                static_cast<unsigned int>(node.m_size_val)
                }) );
        }
        
        void visit(::HIR::ExprNode_Closure& node) override
        {
            TRACE_FUNCTION_F("_Closure - " << node.m_obj_path);
            
            ::std::vector< ::MIR::LValue>   vals;
            vals.reserve( node.m_captures.size() );
            for(auto& arg : node.m_captures)
            {
                this->visit_node_ptr(arg);
                vals.push_back( m_builder.get_result_in_lvalue(arg->span(), arg->m_res_type) );
            }
            
            m_builder.set_result( node.span(), ::MIR::RValue::make_Struct({
                node.m_obj_path.clone(),
                mv$(vals)
                }) );
        }
    };
}


::MIR::FunctionPointer LowerMIR(const StaticTraitResolve& resolve, const ::HIR::ExprPtr& ptr, const ::HIR::Function::args_t& args)
{
    TRACE_FUNCTION;
    
    ::MIR::Function fcn;
    fcn.named_variables.reserve(ptr.m_bindings.size());
    for(const auto& t : ptr.m_bindings)
        fcn.named_variables.push_back( t.clone() );
    
    // Scope ensures that builder cleanup happens before `fcn` is moved
    {
        MirBuilder  builder { ptr->span(), resolve, args, fcn };
        ExprVisitor_Conv    ev { builder, ptr.m_bindings };
        
        // 1. Apply destructuring to arguments
        unsigned int i = 0;
        for( const auto& arg : args )
        {
            ev.define_vars_from(ptr->span(), arg.first);
            ev.destructure_from(ptr->span(), arg.first, ::MIR::LValue::make_Argument({i}));
            i ++;
        }
        
        // 2. Destructure code
        ::HIR::ExprNode& root_node = const_cast<::HIR::ExprNode&>(*ptr);
        root_node.visit( ev );
    }
    
    return ::MIR::FunctionPointer(new ::MIR::Function(mv$(fcn)));
}

namespace {
    // TODO: Create visitor that handles setting up a StaticTraitResolve?
    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_resolve(crate)
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
                    auto fcn = LowerMIR(m_resolve, *e.size, {});
                    e.size->m_mir = mv$(fcn);
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
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            if( item.m_code )
            {
                DEBUG("Function code " << p);
                item.m_code.m_mir = LowerMIR(m_resolve, item.m_code, item.m_args);
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
                item.m_value.m_mir = LowerMIR(m_resolve, item.m_value, {});
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                DEBUG("`const` value " << p);
                item.m_value.m_mir = LowerMIR(m_resolve, item.m_value, {});
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    e.expr.m_mir = LowerMIR(m_resolve, e.expr, {});
                )
            }
        }
        
        // Boilerplate
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override {
            auto _ = this->m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override {
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override {
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
    };
}

// --------------------------------------------------------------------

void HIR_GenerateMIR(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

