/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/from_hir.hpp
 * - Construction of MIR from the HIR expression tree
 */
#include "mir.hpp"
#include "mir_ptr.hpp"
#include <hir/expr.hpp>
#include <algorithm>

namespace {
    class ExprVisitor_Conv:
        public ::HIR::ExprVisitor
    {
        ::MIR::Function&    m_output;
        
        unsigned int    m_current_block;
        
        unsigned int    m_result_tmp_idx;
        
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
        
        
        void push_stmt_assign(::MIR::LValue dst, ::MIR::RValue val)
        {
            m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Assign({ mv$(dst), mv$(val) }) );
        }
        void push_stmt_drop(::MIR::LValue val)
        {
            m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Drop({ ::MIR::eDropKind::DEEP, mv$(val) }) );
        }
        
        void end_block(::MIR::Terminator term)
        {
            if( m_current_block == 0 && (m_output.blocks.size() > 2 || ! m_output.blocks[0].terminator.is_Return()) ) {
                BUG(Span(), "Terminating block when none active");
            }
            m_output.blocks.at(m_current_block).terminator = mv$(term);
            m_current_block = 0;
        }
        void set_cur_block(unsigned int new_block)
        {
            if( m_current_block != 0 || m_output.blocks.size() <= 1) {
                BUG(Span(), "Updating block when previous is active");
            }
            m_current_block = new_block;
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
        
        void destructure_from(const Span& sp, const ::HIR::Pattern& pat, unsigned int temp_idx)
        {
            // TODO: Destructure
        }
        
        // -- ExprVisitor
        void visit(::HIR::ExprNode_Block& node) override
        {
            // TODO: Does this actually need to create a new BB?
            // Creates a BB, all expressions end up as part of it (with all but the final expression having their results dropped)
            if( node.m_nodes.size() > 0 )
            {
                m_block_stack.push_back( {} );
                for(unsigned int i = 0; i < node.m_nodes.size()-1; i ++)
                {
                    this->visit_node_ptr(node.m_nodes.back());
                    this->push_stmt_drop( ::MIR::LValue::make_Temporary({m_result_tmp_idx}) );
                }
                
                this->visit_node_ptr(node.m_nodes.back());
                auto ret = m_result_tmp_idx;
                
                auto bd = mv$( m_block_stack.back() );
                m_block_stack.pop_back();
                
                // Drop all bindings introduced during this block.
                for( auto& var_idx : bd.bindings ) {
                    this->push_stmt_drop( ::MIR::LValue::make_Variable(var_idx) );
                }
                
                m_result_tmp_idx = ret;
            }
            else
            {
                TODO(node.span(), "Lower empty blocks");
            }
        }
        void visit(::HIR::ExprNode_Return& node) override
        {
            this->visit_node_ptr(node.m_value);
            
            this->push_stmt_assign( ::MIR::LValue::make_Return({}), ::MIR::RValue::make_Use( ::MIR::LValue::make_Temporary({m_result_tmp_idx}) ) );
            this->end_block( ::MIR::Terminator::make_Return({}) );
            
            m_result_tmp_idx = 0;
        }
        void visit(::HIR::ExprNode_Let& node) override
        {
            if( node.m_value )
            {
                this->visit_node_ptr(node.m_value);
                
                this->destructure_from(node.span(), node.m_pattern, m_result_tmp_idx);
            }
            m_result_tmp_idx = 0;
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
            
            m_result_tmp_idx = 0;
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
            m_result_tmp_idx = 0;
        }
        
        void visit(::HIR::ExprNode_Match& node) override
        {
            this->visit_node_ptr(node.m_value);
            //auto match_val = m_result_tmp_idx;
            
            // TODO: How to convert an arbitary match into a MIR construct.
            TODO(node.span(), "Convert match into MIR");
        }
    };
}


::MIR::FunctionPointer LowerMIR(const ::HIR::ExprPtr& ptr, const ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >& args)
{
    ::MIR::Function fcn;
    
    // 1. Apply destructuring to arguments
    // 2. Destructure code
    
    return ::MIR::FunctionPointer();
}

