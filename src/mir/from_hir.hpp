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
#include <hir_typeck/static.hpp>    // StaticTraitResolve for Copy

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

enum class VarState {
    Uninit, // No value assigned yet
    Init,   // Initialised and valid at this point
    MaybeMoved, // Possibly has been moved
    Moved,  // Definitely moved
    Dropped,    // Dropped (out of scope)
};
extern ::std::ostream& operator<<(::std::ostream& os, VarState x);

struct SplitArm {
    bool    has_early_terminated = false;
    bool    always_early_terminated = false;    // Populated on completion
    ::std::vector<bool> changed_var_states; // Indexed by binding bumber
    ::std::vector<VarState> var_states;
};

TAGGED_UNION(ScopeType, Variables,
    (Variables, struct {
        ::std::vector<unsigned int> vars;   // List of owned variables
        }),
    (Temporaries, struct {
        ::std::vector<unsigned int> temporaries;    // Controlled temporaries
        }),
    (Split, struct {
        ::std::vector<SplitArm> arms;
        }),
    (Loop, struct {
        })
    );

/// Helper class to construct MIR
class MirBuilder
{
    friend class ScopeHandle;
    
    const Span& m_root_span;
    const StaticTraitResolve& m_resolve;
    ::MIR::Function&    m_output;
    
    unsigned int    m_current_block;
    bool    m_block_active;
    
    ::MIR::RValue   m_result;
    bool    m_result_valid;
    
    ::std::vector<VarState> m_variable_states;
    ::std::vector<VarState> m_temporary_states;
    
    struct ScopeDef
    {
        const Span& span;
        bool complete = false;
        ScopeType   data;
        
        ScopeDef(const Span& span):
            span(span)
        {
        }
        ScopeDef(const Span& span, ScopeType data):
            span(span),
            data(mv$(data))
        {
        }
    };
    
    ::std::vector<ScopeDef> m_scopes;
    ::std::vector<unsigned int> m_scope_stack;
    ScopeHandle m_fcn_scope;
public:
    MirBuilder(const Span& sp, const StaticTraitResolve& resolve, ::MIR::Function& output);
    ~MirBuilder();
    
    const ::HIR::Crate& crate() const { return m_resolve.m_crate; }
    
    // - Values
    ::MIR::LValue new_temporary(const ::HIR::TypeRef& ty);
    ::MIR::LValue lvalue_or_temp(const Span& sp, const ::HIR::TypeRef& ty, ::MIR::RValue val);
    
    bool has_result() const {
        return m_result_valid;
    }
    void set_result(const Span& sp, ::MIR::RValue val);
    ::MIR::RValue get_result(const Span& sp);
    /// Obtains the result, unwrapping into a LValue (and erroring if not)
    ::MIR::LValue get_result_unwrap_lvalue(const Span& sp);
    /// Obtains the result, copying into a temporary if required
    ::MIR::LValue get_result_in_lvalue(const Span& sp, const ::HIR::TypeRef& ty);
    
    // - Statements
    // Push an assignment. NOTE: This also marks the rvalue as moved
    void push_stmt_assign(const Span& sp, ::MIR::LValue dst, ::MIR::RValue val);
    // Push a drop (likely only used by scope cleanup)
    void push_stmt_drop(const Span& sp, ::MIR::LValue val);
    
    // - Block management
    bool block_active() const {
        return m_block_active;
    }
    
    void set_cur_block(unsigned int new_block);
    ::MIR::BasicBlockId pause_cur_block();
    void end_block(::MIR::Terminator term);
    
    ::MIR::BasicBlockId new_bb_linked();
    ::MIR::BasicBlockId new_bb_unlinked();
    
    // --- Scopes ---
    ScopeHandle new_scope_var(const Span& sp);
    ScopeHandle new_scope_temp(const Span& sp);
    ScopeHandle new_scope_split(const Span& sp);
    ScopeHandle new_scope_loop(const Span& sp);
    void terminate_scope(const Span& sp, ScopeHandle );
    void terminate_scope_early(const Span& sp, const ScopeHandle& );
    void end_split_arm(const Span& sp, const ScopeHandle& , bool reachable);
    
    const ScopeHandle& fcn_scope() const {
        return m_fcn_scope;
    }

    /// Introduce a new variable within the current scope
    void define_variable(unsigned int idx);
    // Helper - Marks a variable/... as moved (and checks if the move is valid)
    void moved_lvalue(const Span& sp, const ::MIR::LValue& lv);
private:
    VarState get_variable_state(const Span& sp, unsigned int idx) const;
    void set_variable_state(const Span& sp, unsigned int idx, VarState state);
    VarState get_temp_state(const Span& sp, unsigned int idx) const;
    void set_temp_state(const Span& sp, unsigned int idx, VarState state);
    
    void drop_scope_values(const ScopeDef& sd);
    void complete_scope(ScopeDef& sd);
    
    void with_val_type(const Span& sp, const ::MIR::LValue& val, ::std::function<void(const ::HIR::TypeRef&)> cb);
    bool lvalue_is_copy(const Span& sp, const ::MIR::LValue& lv);
};

class MirConverter:
    public ::HIR::ExprVisitor
{
public:
    virtual void destructure_from(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval, bool allow_refutable=false) = 0;
    virtual void define_vars_from(const Span& sp, const ::HIR::Pattern& pat) = 0;
};

extern void MIR_LowerHIR_Match(MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val);
