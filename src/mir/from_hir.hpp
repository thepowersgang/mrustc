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

    friend ::std::ostream& operator<<(::std::ostream& os, const ScopeHandle& x) {
        os << x.idx;
        return os;
    }
};

// - Needs to handle future DerefMove (which can't use the Box hack)
enum class InvalidType {
    Uninit,
    Moved,
    Descoped,
};
// NOTE: If there's a optional move and a partial merging, it becomes a partial?
TAGGED_UNION_EX(VarState, (), Invalid, (
    // Currently invalid
    (Invalid, InvalidType),
    // Partially valid (Map of field states)
    (Partial, struct {
        ::std::vector<VarState> inner_states;
        unsigned int outer_flag;   // If ~0u there's no condition on the outer
        }),
    (MovedOut, struct {
        ::std::unique_ptr<VarState>   inner_state;
        unsigned int outer_flag;
        }),
    // Optionally valid (integer indicates the drop flag index)
    (Optional, unsigned int),
    // Fully valid
    (Valid, struct {})
    ),
    (), (),
    (
        VarState clone() const;
        bool operator==(const VarState& x) const;
        bool operator!=(const VarState& x) const { return !(*this == x); }
        )
    );
extern ::std::ostream& operator<<(::std::ostream& os, const VarState& x);

struct SplitArm {
    bool    has_early_terminated = false;
    bool    always_early_terminated = false;    // Populated on completion
    //BasicBlockId  source_block;
    ::std::map<unsigned int, VarState>  states;
    ::std::map<unsigned int, VarState>  arg_states;
};
struct SplitEnd {
    ::std::map<unsigned int, VarState>  states;
    ::std::map<unsigned int, VarState>  arg_states;
};

TAGGED_UNION(ScopeType, Owning,
    (Owning, struct {
        bool is_temporary;
        ::std::vector<unsigned int> slots;  // List of owned variables
        }),
    (Split, struct {
        bool end_state_valid = false;
        SplitEnd    cond_state;
        SplitEnd    end_state;
        ::std::vector<SplitArm> arms;
        }),
    (Loop, struct {
        // NOTE: This contains the original state for variables changed after `exit_state_valid` is true
        ::std::map<unsigned int,VarState>    changed_slots;
        ::std::map<unsigned int,VarState>    changed_args;
        bool exit_state_valid;
        SplitEnd    exit_state;
        // TODO: Any drop flags allocated in the loop must be re-initialised at the start of the loop (or before a loopback)
        ::MIR::BasicBlockId   entry_bb;
        ::std::vector<unsigned> drop_flags;
        }),
    // State which should end up with no mutation of variable states
    (Freeze, struct {
        ::std::map<unsigned int,VarState>    changed_slots;
        //::std::map<unsigned int,VarState>    changed_args;
        std::vector<bool>   original_aliases;
        })
    );

#define FIELD_DEREF 255
#define FIELD_INDEX_MAX 128

struct field_path_t
{
    ::std::vector<uint8_t>  data;

    size_t size() const { return data.size(); }
    void push_back(uint8_t v) { data.push_back(v); }
    void pop_back() { data.pop_back(); }
    uint8_t& back() { return data.back(); }

    bool operator==(const field_path_t& x) const { return data == x.data; }
    Ordering ord(const field_path_t& x) const { return ::ord(data, x.data); }

    friend ::std::ostream& operator<<(::std::ostream& os, const field_path_t& x) {
        for(auto idx : x.data) {
            os << ".";
            if( idx == FIELD_DEREF ) {
                os << "*";
            }
            else if( idx > FIELD_INDEX_MAX ) {
                idx -= FIELD_INDEX_MAX;
                idx = FIELD_INDEX_MAX - idx;
                os << "-" << static_cast<unsigned int>(idx);
            }
            else {
                os << static_cast<unsigned int>(idx);
            }
        }
        return os;
    }
};
/// Binding from an expanded pattern
struct PatternBinding
{
    field_path_t    field;
    const ::HIR::PatternBinding*    binding;
    std::pair<size_t,size_t>    split_slice;

    PatternBinding(field_path_t field, const ::HIR::PatternBinding& binding)
        : field(std::move(field))
        , binding(&binding)
        , split_slice(SIZE_MAX,SIZE_MAX)
    {
    }

    bool is_split_slice() const {
        return split_slice.first != SIZE_MAX;
    }

    bool operator==(const PatternBinding& x) const {
        return field == x.field && binding == x.binding && split_slice == x.split_slice;
    }
    friend ::std::ostream& operator<<(::std::ostream& os, const PatternBinding& x) {
        os << *x.binding << x.field;
        if( x.is_split_slice() ) {
            os << "[" << x.split_slice.first << "..-" << x.split_slice.second << "]";
        }
        return os;
    }
};

/// Helper class to construct MIR
class MirBuilder
{
    friend class ScopeHandle;

    const Span& m_root_span;
    const StaticTraitResolve& m_resolve;
    const ::HIR::TypeRef&   m_ret_ty;
    const ::HIR::Function::args_t&  m_args;
    ::MIR::Function&    m_output;

    const ::HIR::SimplePath*    m_lang_Box;

    unsigned int    m_current_block;
    bool    m_block_active;

    ::MIR::RValue   m_result;
    bool    m_result_valid;

    // TODO: Extra information (e.g. mutability)
    VarState    m_return_state;
    ::std::vector<VarState>   m_arg_states;
    ::std::vector<VarState>   m_slot_states;
    size_t  m_first_temp_idx;

    ::std::map<unsigned,unsigned>   m_var_arg_mappings;

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

    typedef std::pair<HIR::PatternBinding::Type, MIR::LValue>   var_alias_t;
    ::std::vector<var_alias_t>  m_variable_aliases;

    // LValue used only for the condition of `if`
    // - Using a fixed temporary simplifies parts of lowering (scope related) and reduces load on
    //   the optimiser.
    ::MIR::LValue   m_if_cond_lval;
public:
    MirBuilder(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ret_ty, const ::HIR::Function::args_t& args, ::MIR::Function& output);
    
    void final_cleanup();

    const ::HIR::SimplePath* lang_Box() const { return m_lang_Box; }
    const ::HIR::Crate& crate() const { return m_resolve.m_crate; }
    const StaticTraitResolve& resolve() const { return m_resolve; }

    /// Check if the passed type is Box<T> and returns a pointer to the T type if so, otherwise nullptr
    const ::HIR::TypeRef* is_type_owned_box(const ::HIR::TypeRef& ty) const;

    // Variable aliases (used for match guards)
    void add_variable_alias(const Span& sp, unsigned idx, HIR::PatternBinding::Type ty, MIR::LValue lv) {
        DEBUG("#" << idx << " = " << int(ty) << " " << lv);
        ASSERT_BUG(sp, idx < m_variable_aliases.size(), "Variable alias #" << idx << " out of bounds");
        ASSERT_BUG(sp, m_variable_aliases[idx].second == MIR::LValue(), "Variable alias #" << idx << " already exists: " << m_variable_aliases[idx].second << " setting " << lv);
        m_variable_aliases[idx] = std::make_pair(ty, mv$(lv));
    }
    const var_alias_t* get_variable_alias(const Span& sp, unsigned idx) const {
        ASSERT_BUG(sp, idx < m_variable_aliases.size(), "Variable alias #" << idx << " out of bounds");
        if(m_variable_aliases[idx].second == MIR::LValue()) {
            return nullptr;
        }
        else {
            return &m_variable_aliases[idx];
        }
    }

    // - Values
    ::MIR::LValue get_variable(const Span& sp, unsigned idx) const {
        auto it = m_var_arg_mappings.find(idx);
        if(it != m_var_arg_mappings.end())
            return ::MIR::LValue::new_Argument(it->second);
        return ::MIR::LValue::new_Local(idx);
    }
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
    ::MIR::LValue get_result_in_lvalue(const Span& sp, const ::HIR::TypeRef& ty, bool allow_missing_value=false);
    /// Obtains a result in a param (or a lvalue)
    ::MIR::Param get_result_in_param(const Span& sp, const ::HIR::TypeRef& ty, bool allow_missing_value=false);

    ::MIR::LValue get_if_cond() const {
        return m_if_cond_lval.clone();
    }
    ::MIR::LValue get_rval_in_if_cond(const Span& sp, ::MIR::RValue val) {
        push_stmt_assign(sp, m_if_cond_lval.clone(), mv$(val));
        return m_if_cond_lval.clone();
    }
    ::MIR::LValue get_result_in_if_cond(const Span& sp) {
        return get_rval_in_if_cond(sp, get_result(sp));
    }

    // - Statements
    // Push an assignment. NOTE: This also marks the rvalue as moved
    void push_stmt_assign(const Span& sp, ::MIR::LValue dst, ::MIR::RValue val, bool drop_destination=true);
    // Push a drop (likely only used by scope cleanup)
    void push_stmt_drop(const Span& sp, ::MIR::LValue val, unsigned int drop_flag=~0u);
    // Push a shallow drop (for Box)
    void push_stmt_drop_shallow(const Span& sp, ::MIR::LValue val, unsigned int drop_flag=~0u);
    // Push an inline assembly statement (NOTE: inputs aren't marked as moved)
    void push_stmt_asm(const Span& sp, ::MIR::Statement::Data_Asm data);
    // Push a setting/clearing of a drop flag
    void push_stmt_set_dropflag_val(const Span& sp, unsigned int index, bool value);
    void push_stmt_set_dropflag_other(const Span& sp, unsigned int index, unsigned int other);
    void push_stmt_set_dropflag_default(const Span& sp, unsigned int index);

    void push_stmt(const Span& sp, ::MIR::Statement stmt);

    // - Block management
    bool block_active() const {
        return m_block_active;
    }

    // Mark a value as initialised (used for Call, because it has to be done after the panic block is populated)
    void mark_value_assigned(const Span& sp, const ::MIR::LValue& val);

    // Moves control of temporaries up to the specified scope (or to above it)
    void raise_temporaries(const Span& sp, const ::MIR::LValue& val, const ScopeHandle& scope, bool to_above=false);
    void raise_temporaries(const Span& sp, const ::MIR::RValue& rval, const ScopeHandle& scope, bool to_above=false);

    void set_cur_block(unsigned int new_block);
    ::MIR::BasicBlockId pause_cur_block();

    void end_block(::MIR::Terminator term);

    ::MIR::BasicBlockId new_bb_linked();
    ::MIR::BasicBlockId new_bb_unlinked();

    unsigned int new_drop_flag(bool default_state);
    unsigned int new_drop_flag_and_set(const Span& sp, bool set_state);
    bool get_drop_flag_default(const Span& sp, unsigned int index);

    // --- Scopes ---
    ScopeHandle new_scope_var(const Span& sp);
    ScopeHandle new_scope_temp(const Span& sp);
    ScopeHandle new_scope_split(const Span& sp);
    ScopeHandle new_scope_loop(const Span& sp);
    ScopeHandle new_scope_freeze(const Span& sp);

    /// Raises every variable defined in the source scope into the target scope
    void raise_all(const Span& sp, ScopeHandle src, const ScopeHandle& target);
    /// Drop all defined values in the scope (emits the drops if `cleanup` is set)
    void terminate_scope(const Span& sp, ScopeHandle , bool cleanup=true);
    /// Terminates a scope early (e.g. via return/break/...)
    void terminate_scope_early(const Span& sp, const ScopeHandle& , bool loop_exit=false);
    /// Marks the end of a split arm (end match arm, if body, ...)
    void end_split_arm(const Span& sp, const ScopeHandle& , bool reachable, bool early=false);
    /// Terminates the current split early (TODO: What does this mean?)
    void end_split_arm_early(const Span& sp);
    /// Terminates the current split condition clause (used for the conditional portion of a match arm)
    void end_split_condition(const Span& sp, const ScopeHandle&);

    const ScopeHandle& fcn_scope() const {
        return m_fcn_scope;
    }

    /// Introduce a new variable within the current scope
    void define_variable(unsigned int idx);
    // Helper - Marks a variable/... as moved (and checks if the move is valid)
    void moved_lvalue(const Span& sp, const ::MIR::LValue& lv);
private:
    enum class SlotType {
        Local,  // Local ~0u is return
        Argument
    };
    const VarState& get_slot_state(const Span& sp, unsigned int idx, SlotType type, unsigned int skip_count=0) const;
    VarState& get_slot_state_mut(const Span& sp, unsigned int idx, SlotType type);

    VarState* get_val_state_mut_p(const Span& sp, const ::MIR::LValue& lv, bool expect_valid=false);

    void terminate_loop_early(const Span& sp, ScopeType::Data_Loop& sd_loop);

    void drop_value_from_state(const Span& sp, const VarState& vs, ::MIR::LValue lv);
    void drop_scope_values(const ScopeDef& sd);
    void complete_scope(ScopeDef& sd);

public:
    void with_val_type(const Span& sp, const ::MIR::LValue& val, ::std::function<void(const ::HIR::TypeRef&)> cb, const ::MIR::LValue::Wrapper* stop_wrapper=nullptr) const;
    bool lvalue_is_copy(const Span& sp, const ::MIR::LValue& lv) const;

    // Obtain the base fat poiner for a dst reference. Errors if it wasn't via a fat pointer
    ::MIR::LValue get_ptr_to_dst(const Span& sp, const ::MIR::LValue& lv) const;

    /// Get the set of currently valid (fully,optional,partial) variables
    class SavedActiveLocal {
        friend class MirBuilder;
        VarState    state;
        SavedActiveLocal(VarState vs): state(mv$(vs)) {}
    public:
    };
    std::map<unsigned, SavedActiveLocal> get_active_locals() const;

    // Calls `drop_value_from_state` on the value
    void drop_actve_local(const Span& sp, ::MIR::LValue lv, const SavedActiveLocal& loc);
};

/// Wrapper interfae
class MirConverter:
    public ::HIR::ExprVisitor
{
public:
    //virtual void destructure_from(const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue lval, bool allow_refutable=false) = 0;
    virtual void define_vars_from(const Span& sp, const ::HIR::Pattern& pat) = 0;

    virtual void destructure_from_list(const Span& sp, const ::HIR::TypeRef& ty, ::MIR::LValue lval, const ::std::vector<PatternBinding>& bindings) = 0;
    virtual void destructure_aliases_from_list(const Span& sp, const ::HIR::TypeRef& ty, ::MIR::LValue lval, const ::std::vector<PatternBinding>& bindings) = 0;
};

extern void MIR_LowerHIR_Match(MirBuilder& builder, MirConverter& conv, ::HIR::ExprNode_Match& node, ::MIR::LValue match_val);
extern void MIR_LowerHIR_Let(MirBuilder& builder, MirConverter& conv, const Span& sp, const ::HIR::Pattern& pat, ::MIR::LValue val, const ::HIR::ExprNode* else_node);

extern void MIR_LowerHIR_GetTypeValueForPath(
    const Span& sp, MirBuilder& builder,
    const ::HIR::TypeRef& top_ty, const ::MIR::LValue& top_val,
    const field_path_t& field_path,// unsigned int field_path_ofs,
    /*Out ->*/ ::HIR::TypeRef& out_ty, ::MIR::LValue& out_val
);
