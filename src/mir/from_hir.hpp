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

class MirBuilder;

class ScopeHandle
{
    friend class MirBuilder;
    
    const MirBuilder& m_builder;
    unsigned int    idx;
    
    ScopeHandle(const MirBuilder& builder, unsigned int idx):
        m_builder(builder),
        idx(idx)
    {
    }
public:
    ScopeHandle(const ScopeHandle& x) = delete;
    ScopeHandle(ScopeHandle&& x):
        m_builder(x.m_builder),
        idx(x.idx)
    {
        x.idx = ~0;
    }
    ScopeHandle& operator=(const ScopeHandle& x) = delete;
    ScopeHandle& operator=(ScopeHandle&& x) = delete;
    ~ScopeHandle();
};

/// Helper class to construct MIR
class MirBuilder
{
    friend class ScopeHandle;
    
    ::MIR::Function&    m_output;
    
    unsigned int    m_current_block;
    bool    m_block_active;
    
    ::MIR::RValue   m_result;
    bool    m_result_valid;
    
    ::std::vector<bool> variables_valid;
    ::std::vector<bool> temporaries_valid;
    
    struct ScopeDef
    {
        bool complete = false;
        ::std::vector<unsigned int> variables;  // Variables to be dropped at the end of this scope
        ::std::vector<unsigned int> temporaries;  // Temporaries introduced during this scope
        
        bool is_conditional = false;
        #if 0
        ::std::vector<bool> moved_vars; // Bitmap of variables moved in the current branch
        ::std::vector<bool> all_moved_vars; // Bitmap of variables moved in at least one branch
        ::std::vector<bool> all_unmoved_vars; // Bitmap of variables NOT moved in at least one branch
        #endif
    };
    
    ::std::vector<ScopeDef> m_scopes;
    ::std::vector<unsigned int> m_scope_stack;
    ScopeHandle m_fcn_scope;
public:
    MirBuilder(::MIR::Function& output):
        m_output(output),
        m_block_active(false),
        m_result_valid(false),
        m_fcn_scope(*this, 0)
    {
        set_cur_block( new_bb_unlinked() );
        m_scopes.push_back( ScopeDef {} );
        m_scope_stack.push_back( 0 );
    }
    ~MirBuilder();
    
    // - Values
    ::MIR::LValue new_temporary(const ::HIR::TypeRef& ty);
    ::MIR::LValue lvalue_or_temp(const ::HIR::TypeRef& ty, ::MIR::RValue val);
    
    bool has_result() const {
        return m_result_valid;
    }
    ::MIR::RValue get_result(const Span& sp);
    ::MIR::LValue get_result_lvalue(const Span& sp);
    void set_result(const Span& sp, ::MIR::RValue val);
    
    // - Statements
    // Push an assignment. NOTE: This also marks the rvalue as moved
    void push_stmt_assign(::MIR::LValue dst, ::MIR::RValue val);
    // Push a drop (likely only used by scope cleanup)
    void push_stmt_drop(::MIR::LValue val);
    
    // - Block management
    bool block_active() const {
        return m_block_active;
    }
    
    void set_cur_block(unsigned int new_block);
    void pause_cur_block();
    void end_block(::MIR::Terminator term);
    
    ::MIR::BasicBlockId new_bb_linked();
    ::MIR::BasicBlockId new_bb_unlinked();
    
    // --- Scopes ---
    /// `is_conditional`: If true, this scope is the contents of a conditional branch of the parent scope
    ScopeHandle new_scope(const Span& sp, bool is_conditional=false);
    void terminate_scope(const Span& sp, ScopeHandle );
    void terminate_scope_early(const Span& sp, const ScopeHandle& );
    
    const ScopeHandle& fcn_scope() const {
        return m_fcn_scope;
    }

private:
    void drop_scope_values(const ScopeDef& sd);
    // Helper - Marks a variable/... as moved (and checks if the move is valid)
    void moved_lvalue(const ::MIR::LValue& lv);
};

class MirConverter:
    public ::HIR::ExprVisitor
{
public:
    virtual void destructure_from(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval, bool allow_refutable=false) = 0;
};

extern void MIR_LowerHIR_Match(MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val);
