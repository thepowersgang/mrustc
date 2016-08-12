/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/from_hir.hpp
 * - Construction of MIR from the HIR expression tree
 */
#pragma once
#include "mir.hpp"
#include <hir/type.hpp>
#include <hir/expr.hpp> // for ExprNode_Match

/// Helper class to construct MIR
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
    
    ::MIR::LValue new_temporary(const ::HIR::TypeRef& ty);
    ::MIR::LValue lvalue_or_temp(const ::HIR::TypeRef& ty, ::MIR::RValue val);
    
    bool has_result() const {
        return m_result_valid;
    }
    ::MIR::RValue get_result(const Span& sp);
    ::MIR::LValue get_result_lvalue(const Span& sp);
    void set_result(const Span& sp, ::MIR::RValue val);
    
    void push_stmt_assign(::MIR::LValue dst, ::MIR::RValue val);
    void push_stmt_drop(::MIR::LValue val);
    
    bool block_active() const {
        return m_block_active;
    }
    
    void set_cur_block(unsigned int new_block);
    void pause_cur_block();
    void end_block(::MIR::Terminator term);
    
    ::MIR::BasicBlockId new_bb_linked();
    ::MIR::BasicBlockId new_bb_unlinked();
};

class MirConverter:
    public ::HIR::ExprVisitor
{
public:
    virtual void destructure_from(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval, bool allow_refutable=false) = 0;
};

extern void MIR_LowerHIR_Match(MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val);
