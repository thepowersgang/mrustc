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

// TODO: Replace VarState with a TU
#if 0
enum class InvalidType {
    Uninit,
    Moved,
    Descoped,
};
TAGGED_UNION(VarState, Uninit,
    // Currently invalid
    (Invalid, InvalidType),
    // Partially valid (Map of field states, Box is assumed to have one field)
    (Partial, ::std::vector<VarState>),
    // Optionally valid (integer indicates the drop flag index)
    (Optional, unsigned int),
    // Fully valid
    (Valid, struct {}),
    )
#endif

// TODO: Replace the first three states with just one (and flags for init/moved)
enum class VarState {
    Uninit, // No value assigned yet
    Moved,  // Definitely moved
    Dropped,    // Dropped (out of scope)

    // TODO: Store a bitmap of inner states?
    // - Needs to handle relatively arbitary patterns. Including moving out of a Box, but not out of Drop types
    InnerMoved, // The inner has been moved, but the container needs to be dropped
    //MaybeMovedInner,  // Inner possibly has been moved
    MaybeMoved, // Possibly has been moved

    Init,   // Initialised and valid at this point
};
extern ::std::ostream& operator<<(::std::ostream& os, VarState x);

struct SplitArm {
    bool    has_early_terminated = false;
    bool    always_early_terminated = false;    // Populated on completion
    ::std::map<unsigned int, VarState>  var_states;
    ::std::map<unsigned int, VarState>  tmp_states;
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
        ::std::set<unsigned int>    changed_vars;
        ::std::set<unsigned int>    changed_tmps;
        ::std::vector<SplitArm> exit_states;
        })
    );

/// Helper class to construct MIR
class MirBuilder
{
    friend class ScopeHandle;

    const Span& m_root_span;
    const StaticTraitResolve& m_resolve;
    const ::HIR::Function::args_t&  m_args;
    ::MIR::Function&    m_output;

    const ::HIR::SimplePath*    m_lang_Box;

    unsigned int    m_current_block;
    bool    m_block_active;

    ::MIR::RValue   m_result;
    bool    m_result_valid;

    // TODO: Extra information.
    //::std::vector<VarState>   m_arg_states;
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
    MirBuilder(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::Function::args_t& args, ::MIR::Function& output);
    ~MirBuilder();

    const ::HIR::SimplePath* lang_Box() const { return m_lang_Box; }
    const ::HIR::Crate& crate() const { return m_resolve.m_crate; }
    const StaticTraitResolve& resolve() const { return m_resolve; }

    //::HIR::TypeRef* is_type_owned_box(::HIR::TypeRef& ty) const {
    //}
    /// Check if the passed type is Box<T> and returns a pointer to the T type if so, otherwise nullptr
    const ::HIR::TypeRef* is_type_owned_box(const ::HIR::TypeRef& ty) const;

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
    // Push a shallow drop (for Box)
    void push_stmt_drop_shallow(const Span& sp, ::MIR::LValue val);

    // - Block management
    bool block_active() const {
        return m_block_active;
    }

    // Mark a value as initialised (used for Call, because it has to be done after the panic block is populated)
    void mark_value_assigned(const Span& sp, const ::MIR::LValue& val);

    // Moves control of temporaries up to the next scope
    void raise_variables(const Span& sp, const ::MIR::LValue& val, const ScopeHandle& scope);
    void raise_variables(const Span& sp, const ::MIR::RValue& rval, const ScopeHandle& scope);

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
    void terminate_scope(const Span& sp, ScopeHandle , bool cleanup=true);
    void terminate_scope_early(const Span& sp, const ScopeHandle& , bool loop_exit=false);
    void end_split_arm(const Span& sp, const ScopeHandle& , bool reachable);
    void end_split_arm_early(const Span& sp);

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

public:
    void with_val_type(const Span& sp, const ::MIR::LValue& val, ::std::function<void(const ::HIR::TypeRef&)> cb) const;
    bool lvalue_is_copy(const Span& sp, const ::MIR::LValue& lv) const;
};

class MirConverter:
    public ::HIR::ExprVisitor
{
public:
    virtual void destructure_from(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval, bool allow_refutable=false) = 0;
    virtual void define_vars_from(const Span& sp, const ::HIR::Pattern& pat) = 0;
};

extern void MIR_LowerHIR_Match(MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val);
