/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/check_full.cpp
 * - Full MIR correctness checks (expensive value state checks)
 */
#include "main_bindings.hpp"
#include "mir.hpp"
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>
#include <mir/helpers.hpp>
#include <mir/visit_crate_mir.hpp>

namespace
{
    struct State
    {
        // 0 = invalid
        // -1 = valid
        // other = 1-based index into `inner_states`
        unsigned int    index;

        State(): index(0) {}
        State(bool valid): index(valid ? ~0u : 0) {}
        State(size_t idx):
            index(idx+1)
        {
        }

        bool is_composite() const {
            return index != 0 && index != ~0u;
        }
        bool is_valid() const {
            return index != 0;
        }

        bool operator==(const State& x) const {
            return index == x.index;
        }
        bool operator!=(const State& x) const {
            return !(*this == x);
        }
    };
    struct ValueStates
    {
        ::std::vector<State> vars;
        ::std::vector<State> temporaries;
        ::std::vector<State> arguments;
        State   return_value;
        ::std::vector<bool> drop_flags;

        ::std::vector< ::std::vector<State> >   inner_states;

        ::std::vector<unsigned int> bb_path;

        ValueStates clone() const
        {
            return *this;
        }
        bool is_equivalent_to(const ValueStates& x) const
        {
            return true;
        }

        void ensure_param_valid(const ::MIR::TypeResolve& mir_res, const ::MIR::Param& lv) const
        {
            if(const auto* e = lv.opt_LValue())
            {
                this->ensure_lvalue_valid(mir_res, *e);
            }
        }
        void ensure_lvalue_valid(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& lv) const
        {
            auto vs = get_lvalue_state(mir_res, lv);
            ::std::vector<unsigned int> path;
            ensure_valid(mir_res, lv, vs, path);
        }
    private:
        void ensure_valid(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& root_lv, const State& vs, ::std::vector<unsigned int>& path) const
        {
            if( vs.is_composite() )
            {
                MIR_ASSERT(mir_res, vs.index-1 < this->inner_states.size(), "");
                const auto& states = this->inner_states.at( vs.index - 1 );

                path.push_back(0);
                for(const auto& inner_vs : states)
                {
                    ensure_valid(mir_res,root_lv, inner_vs, path);
                    path.back() ++;
                }
                path.pop_back();
            }
            else if( !vs.is_valid() )
            {
                MIR_BUG(mir_res, "Accessing invalidated lvalue - " << root_lv << " - field path=[" << path << "], BBs=[" << this->bb_path << "]");
            }
            else
            {
            }
        }

    public:
        void move_lvalue(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& lv)
        {
            this->ensure_lvalue_valid(mir_res, lv);

            ::HIR::TypeRef  tmp;
            const auto& ty = mir_res.get_lvalue_type(tmp, lv);
            if( mir_res.m_resolve.type_is_copy(mir_res.sp, ty) )
            {
            }
            else
            {
                this->set_lvalue_state(mir_res, lv, State(false));
            }
        }
        void mark_lvalue_valid(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& lv)
        {
            this->set_lvalue_state(mir_res, lv, State(true));
        }
    private:
        State allocate_composite(unsigned int n_fields, State basis)
        {
            auto idx = inner_states.size();
            inner_states.push_back( ::std::vector<State>(n_fields, basis) );
            return State(idx);
        }

        State get_lvalue_state(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& lv) const
        {
            TU_MATCHA( (lv), (e),
            (Variable,
                return vars.at(e);
                ),
            (Temporary,
                return temporaries.at(e.idx);
                ),
            (Argument,
                return arguments.at(e.idx);
                ),
            (Static,
                return State(true);
                ),
            (Return,
                return return_value;
                ),
            (Field,
                auto vs = get_lvalue_state(mir_res, *e.val);
                if( vs.is_composite() )
                {
                    MIR_ASSERT(mir_res, vs.index-1 < this->inner_states.size(), "");
                    const auto& states = this->inner_states.at( vs.index - 1 );
                    MIR_ASSERT(mir_res, e.field_index < states.size(), "Field index out of range");
                    return states[e.field_index];
                }
                else
                {
                    return vs;
                }
                ),
            (Deref,
                auto vs = get_lvalue_state(mir_res, *e.val);
                if( vs.is_composite() )
                {
                    MIR_TODO(mir_res, "Deref with composite state");
                }
                else
                {
                    return vs;
                }
                ),
            (Index,
                auto vs_v = get_lvalue_state(mir_res, *e.val);
                auto vs_i = get_lvalue_state(mir_res, *e.idx);
                MIR_ASSERT(mir_res, !vs_v.is_composite(), "");
                MIR_ASSERT(mir_res, !vs_i.is_composite(), "");
                return State(vs_v.is_valid() && vs_i.is_valid());
                ),
            (Downcast,
                auto vs_v = get_lvalue_state(mir_res, *e.val);
                MIR_ASSERT(mir_res, !vs_v.is_composite(), "");
                return vs_v;
                )
            )
            throw "";
        }
        void set_lvalue_state(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& lv, State new_vs)
        {
            TU_MATCHA( (lv), (e),
            (Variable,
                vars.at(e) = new_vs;
                ),
            (Temporary,
                temporaries.at(e.idx) = new_vs;
                ),
            (Argument,
                arguments.at(e.idx) = new_vs;
                ),
            (Static,
                // Ignore.
                ),
            (Return,
                return_value = new_vs;
                ),
            (Field,
                auto cur_vs = get_lvalue_state(mir_res, *e.val);
                if( !cur_vs.is_composite() && cur_vs == new_vs )
                {
                    // Not a composite, and no state change
                }
                else
                {
                    if( !cur_vs.is_composite() )
                    {
                        ::HIR::TypeRef    tmp;
                        const auto& ty = mir_res.get_lvalue_type(tmp, *e.val);
                        unsigned int n_fields = 0;
                        if( const auto* e = ty.m_data.opt_Tuple() )
                        {
                            n_fields = e->size();
                        }
                        else if( ty.m_data.is_Path() && ty.m_data.as_Path().binding.is_Struct() )
                        {
                            const auto& e = ty.m_data.as_Path().binding.as_Struct();
                            TU_MATCHA( (e->m_data), (se),
                            (Unit,
                                n_fields = 0;
                                ),
                            (Tuple,
                                n_fields = se.size();
                                ),
                            (Named,
                                n_fields = se.size();
                                )
                            )
                        }
                        else {
                            MIR_BUG(mir_res, "Unknown type being accessed with Field - " << ty);
                        }
                        cur_vs = this->allocate_composite(n_fields, cur_vs);
                        set_lvalue_state(mir_res, *e.val, cur_vs);
                    }
                    // Get composite state and assign into it
                    auto& states = this->inner_states.at( cur_vs.index - 1 );
                    MIR_ASSERT(mir_res, e.field_index < states.size(), "Field index out of range");
                    states[e.field_index] = new_vs;
                }
                ),
            (Deref,
                auto cur_vs = get_lvalue_state(mir_res, *e.val);
                if( cur_vs.is_composite() )
                {
                    MIR_TODO(mir_res, "Deref with composite state");
                }
                else if( new_vs.is_composite() )
                {
                    MIR_TODO(mir_res, "Deref with composite state (store a composite)");
                }
                else if( cur_vs != new_vs )
                {
                    MIR_TODO(mir_res, "Deref with composite state, store mismatched");
                }
                else
                {
                }
                ),
            (Index,
                auto vs_v = get_lvalue_state(mir_res, *e.val);
                auto vs_i = get_lvalue_state(mir_res, *e.idx);
                MIR_ASSERT(mir_res, !vs_v.is_composite(), "");
                MIR_ASSERT(mir_res, !vs_i.is_composite(), "");

                MIR_ASSERT(mir_res, vs_v.is_valid(), "Indexing an invalid value");
                MIR_ASSERT(mir_res, vs_i.is_valid(), "Indexing with an invalid index");

                // NOTE: Ignore
                ),
            (Downcast,
                // NOTE: Ignore
                )
            )
        }
    };


    struct StateSet
    {
        ::std::vector<ValueStates>   known_state_sets;

        bool add_state(const ValueStates& state_set)
        {
            for(const auto& s : this->known_state_sets)
            {
                if( s.is_equivalent_to(state_set) )
                {
                    return false;
                }
            }
            this->known_state_sets.push_back( state_set.clone() );
            return true;
        }
    };
}


// "Executes" the function, keeping track of drop flags and variable validities
void MIR_Validate_FullValState(::MIR::TypeResolve& mir_res, const ::MIR::Function& fcn)
{
    ::std::vector<StateSet> block_entry_states( fcn.blocks.size() );

    ValueStates state;
    state.arguments.resize( mir_res.m_args.size(), State(true) );
    state.vars.resize( fcn.named_variables.size() );
    state.temporaries.resize( fcn.temporaries.size() );
    state.drop_flags = fcn.drop_flags;

    ::std::vector< ::std::pair<unsigned int, ValueStates> > todo_queue;
    todo_queue.push_back( ::std::make_pair(0, mv$(state)) );
    while( ! todo_queue.empty() )
    {
        auto cur_block = todo_queue.back().first;
        auto state = mv$(todo_queue.back().second);
        todo_queue.pop_back();

        if( ! block_entry_states[cur_block].add_state(state) )
        {
            continue ;
        }
        state.bb_path.push_back( cur_block );

        const auto& blk = fcn.blocks.at(cur_block);
        for(size_t i = 0; i < blk.statements.size(); i++)
        {
            mir_res.set_cur_stmt(cur_block, i);

            TU_MATCHA( (blk.statements[i]), (se),
            (Assign,
                DEBUG(mir_res << " " << se.dst << " = " << se.src);
                TU_MATCHA( (se.src), (ve),
                (Use,
                    state.move_lvalue(mir_res, ve);
                    ),
                (Constant,
                    ),
                (SizedArray,
                    state.ensure_param_valid(mir_res, ve.val);
                    ),
                (Borrow,
                    state.ensure_lvalue_valid(mir_res, ve.val);
                    ),
                // Cast on primitives
                (Cast,
                    state.ensure_lvalue_valid(mir_res, ve.val);
                    ),
                // Binary operation on primitives
                (BinOp,
                    state.ensure_param_valid(mir_res, ve.val_l);
                    state.ensure_param_valid(mir_res, ve.val_r);
                    ),
                // Unary operation on primitives
                (UniOp,
                    state.ensure_lvalue_valid(mir_res, ve.val);
                    ),
                // Extract the metadata from a DST pointer
                // NOTE: If used on an array, this yields the array size (for generics)
                (DstMeta,
                    state.ensure_lvalue_valid(mir_res, ve.val);
                    ),
                // Extract the pointer from a DST pointer (as *const ())
                (DstPtr,
                    state.ensure_lvalue_valid(mir_res, ve.val);
                    ),
                // Construct a DST pointer from a thin pointer and metadata
                (MakeDst,
                    state.ensure_param_valid(mir_res, ve.ptr_val);
                    state.ensure_param_valid(mir_res, ve.meta_val);
                    ),
                (Tuple,
                    for(const auto& v : ve.vals)
                        if(const auto* e = v.opt_LValue())
                            state.move_lvalue(mir_res, *e);
                    ),
                // Array literal
                (Array,
                    for(const auto& v : ve.vals)
                        if(const auto* e = v.opt_LValue())
                            state.move_lvalue(mir_res, *e);
                    ),
                // Create a new instance of a union (and eventually enum)
                (Variant,
                    if(const auto* e = ve.val.opt_LValue())
                        state.move_lvalue(mir_res, *e);
                    ),
                // Create a new instance of a struct (or enum)
                (Struct,
                    for(const auto& v : ve.vals)
                        if(const auto* e = v.opt_LValue())
                            state.move_lvalue(mir_res, *e);
                    )
                )
                state.mark_lvalue_valid(mir_res, se.dst);
                ),
            (Asm,
                for(const auto& v : se.inputs)
                    state.ensure_lvalue_valid(mir_res, v.second);
                for(const auto& v : se.outputs)
                    state.mark_lvalue_valid(mir_res, v.second);
                ),
            (SetDropFlag,
                if( se.other == ~0u )
                {
                    state.drop_flags[se.idx] = se.new_val;
                }
                else
                {
                    state.drop_flags[se.idx] = (se.new_val != state.drop_flags[se.other]);
                }
                ),
            (Drop,
                if( se.flag_idx == ~0u || state.drop_flags.at(se.flag_idx) )
                {
                    // TODO: Treat shallow drops differently
                    state.move_lvalue(mir_res, se.slot);
                }
                )
            )
        }

        mir_res.set_cur_stmt_term(cur_block);
        DEBUG(mir_res << " " << blk.terminator);
        TU_MATCHA( (blk.terminator), (te),
        (Incomplete,
            ),
        (Return,
            ),
        (Diverge,
            ),
        (Goto,   // Jump to another block
            todo_queue.push_back( ::std::make_pair(te, mv$(state)) );
            ),
        (Panic,
            todo_queue.push_back( ::std::make_pair(te.dst, mv$(state)) );
            ),
        (If,
            state.ensure_lvalue_valid(mir_res, te.cond);
            todo_queue.push_back( ::std::make_pair(te.bb0, state.clone()) );
            todo_queue.push_back( ::std::make_pair(te.bb1, mv$(state)) );
            ),
        (Switch,
            state.ensure_lvalue_valid(mir_res, te.val);
            for(size_t i = 0; i < te.targets.size(); i ++)
            {
                todo_queue.push_back( ::std::make_pair(te.targets[i], i == te.targets.size()-1 ? mv$(state) : state.clone()) );
            }
            ),
        (Call,
            if(const auto* e = te.fcn.opt_Value())
            {
                state.ensure_lvalue_valid(mir_res, *e);
            }
            for(auto& arg : te.args)
            {
                if(const auto* e = arg.opt_LValue())
                {
                    state.move_lvalue(mir_res, *e);
                }
            }
            todo_queue.push_back( ::std::make_pair(te.panic_block, state.clone()) );
            state.mark_lvalue_valid(mir_res, te.ret_val);
            todo_queue.push_back( ::std::make_pair(te.ret_block, mv$(state)) );
            )
        )
    }
}

void MIR_Validate_Full(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, const ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    TRACE_FUNCTION_F(path);
    Span    sp;
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };
    // Validation rules:

    MIR_Validate_FullValState(state, fcn);
}

// --------------------------------------------------------------------

void MIR_CheckCrate_Full(/*const*/ ::HIR::Crate& crate)
{
    ::MIR::OuterVisitor    ov(crate, [](const auto& res, const auto& p, auto& expr, const auto& args, const auto& ty)
        {
            MIR_Validate_Full(res, p, *expr.m_mir, args, ty);
        }
        );
    ov.visit_crate( crate );
}

