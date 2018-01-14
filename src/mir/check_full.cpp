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

// DISABLED: Unsizing intentionally leaks
#define ENABLE_LEAK_DETECTOR    0

namespace
{
    struct State
    {
        // 0 = invalid
        // -1 = valid
        // other = 1-based index into `inner_states`
        unsigned int    index;

        explicit State(const State&) = default;
        State(State&& x) = default;
        State& operator=(const State&) = delete;
        State& operator=(State&& ) = default;

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

    struct ValueStates;
}

struct StateFmt {
    const ValueStates&  vss;
    const State&    s;
    StateFmt( const ValueStates& vss, const State& s ):
        vss(vss), s(s)
    {}
};
::std::ostream& operator<<(::std::ostream& os, const StateFmt& x);

namespace
{
    struct ValueStates
    {
        State   return_value;
        ::std::vector<State> args;
        ::std::vector<State> locals;
        ::std::vector<bool> drop_flags;

        ::std::vector< ::std::vector<State> >   inner_states;

        ::std::vector<unsigned int> bb_path;

        ValueStates clone() const
        {
            struct H  {
                static ::std::vector<State> clone_state_list(const ::std::vector<State>& l) {
                    ::std::vector<State> rv;
                    rv.reserve(l.size());
                    for(const auto& s : l)
                        rv.push_back( State(s) );
                    return rv;
                }
            };
            ValueStates rv;
            rv.return_value = State(this->return_value);
            rv.args = H::clone_state_list(this->args);
            rv.locals = H::clone_state_list(this->locals);
            rv.drop_flags = this->drop_flags;
            rv.inner_states.reserve( this->inner_states.size() );
            for(const auto& isl : this->inner_states)
                rv.inner_states.push_back( H::clone_state_list(isl) );
            rv.bb_path = this->bb_path;
            return *this;
        }

        bool is_equivalent_to(const ValueStates& x) const
        {
            struct H {
                static bool equal(const ValueStates& vss_a, const State& a,  const ValueStates& vss_b, const State& b)
                {
                    if( a.index == 0 )
                    {
                        return b.index == 0;
                    }
                    if( a.index == ~0u )
                    {
                        return b.index == ~0u;
                    }
                    if( b.index == 0 || b.index == ~0u )
                    {
                        return false;
                    }

                    const auto& states_a = vss_a.inner_states.at( a.index - 1 );
                    const auto& states_b = vss_b.inner_states.at( b.index - 1 );
                    // NOTE: If there's two differen variants, this can happen.
                    if( states_a.size() != states_b.size() )
                        return false;

                    for(size_t i = 0; i < states_a.size(); i ++)
                    {
                        if( ! H::equal(vss_a, states_a[i],  vss_b, states_b[i]) )
                            return false;
                    }
                    // If the above loop didn't early exit, the two states are equal
                    return true;
                }
            };

            if( this->drop_flags != x.drop_flags )
                return false;
            if( ! H::equal(*this, return_value,  x, x.return_value) )
                return false;

            assert(args.size() == x.args.size());
            for(size_t i = 0; i < args.size(); i ++)
            {
                if( ! H::equal(*this, args[i],  x, x.args[i]) )
                    return false;
            }

            assert(locals.size() == x.locals.size());
            for(size_t i = 0; i < locals.size(); i ++)
            {
                if( ! H::equal(*this, locals[i],  x, x.locals[i]) )
                    return false;
            }
            return true;
        }

        StateFmt fmt_state(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& lv) const {
            return StateFmt(*this, get_lvalue_state(mir_res, lv));
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
            const auto& vs = get_lvalue_state(mir_res, lv);
            ::std::vector<unsigned int> path;
            ensure_valid(mir_res, lv, vs, path);
        }
    private:
        struct InvalidReason {
            enum  {
                Unwritten,
                Moved,
                Invalidated,
            }   ty;
            size_t  bb;
            size_t  stmt;

            void fmt(::std::ostream& os) const {
                switch(this->ty)
                {
                case Unwritten: os << "Not Written";    break;
                case Moved: os << "Moved at BB" << bb << "/" << stmt;   break;
                case Invalidated:   os << "Invalidated at BB" << bb << "/" << stmt;   break;
                }
            }
        };
        InvalidReason find_invalid_reason(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& root_lv) const
        {
            using ::MIR::visit::ValUsage;
            using ::MIR::visit::visit_mir_lvalues;

            ::HIR::TypeRef  tmp;
            bool is_copy = mir_res.m_resolve.type_is_copy( mir_res.sp, mir_res.get_lvalue_type(tmp, root_lv) );
            size_t cur_stmt = mir_res.get_cur_stmt_ofs();

            // Dump all statements
            if(true)
            {
                for(size_t i = 0; i < this->bb_path.size()-1; i++)
                {
                    size_t bb_idx = this->bb_path[i];
                    const auto& bb = mir_res.m_fcn.blocks.at(bb_idx);

                    for(size_t stmt_idx = 0; stmt_idx < bb.statements.size(); stmt_idx++)
                    {
                        DEBUG("BB" << bb_idx << "/" << stmt_idx << " - " << bb.statements[stmt_idx]);
                    }
                    DEBUG("BB" << bb_idx << "/TERM - " << bb.terminator);
                }

                {
                    size_t bb_idx = this->bb_path.back();
                    const auto& bb = mir_res.m_fcn.blocks.at(bb_idx);
                    for(size_t stmt_idx = 0; stmt_idx < cur_stmt; stmt_idx ++)
                    {
                        DEBUG("BB" << bb_idx << "/" << stmt_idx << " - " << bb.statements[stmt_idx]);
                    }
                }
            }

            if( !is_copy )
            {
                // Walk backwards through the BBs and find where it's used by value
                assert(this->bb_path.size() > 0);
                size_t bb_idx;
                size_t stmt_idx;

                bool was_moved = false;
                size_t  moved_bb, moved_stmt;
                auto visit_cb = [&](const auto& lv, auto vu) {
                    if(lv == root_lv && vu == ValUsage::Move) {
                        was_moved = true;
                        moved_bb = bb_idx;
                        moved_stmt = stmt_idx;
                        return false;
                    }
                    return false;
                    };
                // Most recent block (incomplete)
                {
                    bb_idx = this->bb_path.back();
                    const auto& bb = mir_res.m_fcn.blocks.at(bb_idx);
                    for(stmt_idx = cur_stmt; stmt_idx -- && !was_moved; )
                    {
                        visit_mir_lvalues(bb.statements[stmt_idx], visit_cb);
                    }
                }
                for(size_t i = this->bb_path.size()-1; i -- && !was_moved; )
                {
                    bb_idx = this->bb_path[i];
                    const auto& bb = mir_res.m_fcn.blocks.at(bb_idx);
                    stmt_idx = bb.statements.size();

                    visit_mir_lvalues(bb.terminator, visit_cb);

                    for(stmt_idx = bb.statements.size(); stmt_idx -- && !was_moved; )
                    {
                        visit_mir_lvalues(bb.statements[stmt_idx], visit_cb);
                    }
                }

                if( was_moved )
                {
                    // Reason found, the value was moved
                    DEBUG("- Moved in BB" << moved_bb << "/" << moved_stmt);
                    return InvalidReason { InvalidReason::Moved, moved_bb, moved_stmt };
                }
            }
            else
            {
                // Walk backwards to find assignment (if none, it's never initialized)
                assert(this->bb_path.size() > 0);
                size_t bb_idx;
                size_t stmt_idx;

                bool assigned = false;
                auto visit_cb = [&](const auto& lv, auto vu) {
                    if(lv == root_lv && vu == ValUsage::Write) {
                        assigned = true;
                        //assigned_bb = this->bb_path[i];
                        //assigned_stmt = j;
                        return true;
                    }
                    return false;
                    };

                // Most recent block (incomplete)
                {
                    bb_idx = this->bb_path.back();
                    const auto& bb = mir_res.m_fcn.blocks.at(bb_idx);
                    for(stmt_idx = cur_stmt; stmt_idx -- && !assigned; )
                    {
                        visit_mir_lvalues(bb.statements[stmt_idx], visit_cb);
                    }
                }
                for(size_t i = this->bb_path.size()-1; i -- && !assigned; )
                {
                    bb_idx = this->bb_path[i];
                    const auto& bb = mir_res.m_fcn.blocks.at(bb_idx);
                    stmt_idx = bb.statements.size();

                    visit_mir_lvalues(bb.terminator, visit_cb);

                    for(stmt_idx = bb.statements.size(); stmt_idx -- && !assigned; )
                    {
                        visit_mir_lvalues(bb.statements[stmt_idx], visit_cb);
                    }
                }

                if( !assigned )
                {
                    // Value wasn't ever assigned, that's why it's not valid.
                    DEBUG("- Not assigned");
                    return InvalidReason { InvalidReason::Unwritten, 0, 0 };
                }
            }
            // If neither of the above return a reason, check for blocks that don't have the value valid.
            // TODO: This requires access to the lifetime bitmaps to know where it was invalidated
            DEBUG("- (assume) lifetime invalidated [is_copy=" << is_copy << "]");
            return InvalidReason { InvalidReason::Invalidated, 0, 0 };
        }
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
                // Locate where it was invalidated.
                auto reason = find_invalid_reason(mir_res, root_lv);
                MIR_BUG(mir_res, "Accessing invalidated lvalue - " << root_lv << " - " << FMT_CB(s,reason.fmt(s);) << " - field path=[" << path << "], BBs=[" << this->bb_path << "]");
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
                // NOTE: Copy types aren't moved.
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

        // Scan states and clear unused composite slots
        void garbage_collect()
        {
            struct Marker {
                ::std::vector<bool> used;

                void mark_from_state(const ValueStates& vss, const State& s) {
                    if(s.is_composite()) {
                        used.at(s.index-1) = true;
                        for(const auto& is : vss.inner_states.at(s.index-1))
                            mark_from_state(vss, is);

                        // TODO: Should this compact composites with all-equal inner states?
                    }
                }
            };
            Marker  m;
            m.used.resize(this->inner_states.size(), false);

            m.mark_from_state(*this, this->return_value);
            for(const auto& s : this->args)
                m.mark_from_state(*this, s);
            for(const auto& s : this->locals)
                m.mark_from_state(*this, s);
        }
    private:
        ::std::vector<State>& allocate_composite_int(State& out_state)
        {
            // 1. Search for an unused (empty) slot
            for(size_t i = 0; i < this->inner_states.size(); i ++)
            {
                if( this->inner_states[i].size() == 0 )
                {
                    out_state = State(i);
                    return inner_states[i];
                }
            }
            // 2. If none avaliable, allocate a new slot
            auto idx = inner_states.size();
            inner_states.push_back({});
            out_state = State(idx);
            return inner_states.back();
        }
        State allocate_composite(unsigned int n_fields, const State& basis)
        {
            assert(n_fields > 0);
            assert(!basis.is_composite());

            State   rv;
            auto& sub_states = allocate_composite_int(rv);
            assert(sub_states.size() == 0);

            sub_states.reserve(n_fields);
            while(n_fields--)
            {
                sub_states.push_back( State(basis) );
            }

            return rv;
        }

    public:
        ::std::vector<State>& get_composite(const ::MIR::TypeResolve& mir_res, const State& vs)
        {
            MIR_ASSERT(mir_res, vs.index-1 < this->inner_states.size(), "");
            return this->inner_states.at( vs.index - 1 );
        }
        const ::std::vector<State>& get_composite(const ::MIR::TypeResolve& mir_res, const State& vs) const
        {
            MIR_ASSERT(mir_res, vs.index-1 < this->inner_states.size(), "");
            return this->inner_states.at( vs.index - 1 );
        }
        const State& get_lvalue_state(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& lv) const
        {
            TU_MATCHA( (lv), (e),
            (Return,
                return return_value;
                ),
            (Argument,
                return args.at(e.idx);
                ),
            (Local,
                return locals.at(e);
                ),
            (Static,
                static State    state_of_static(true);
                return state_of_static;
                ),
            (Field,
                const auto& vs = get_lvalue_state(mir_res, *e.val);
                if( vs.is_composite() )
                {
                    const auto& states = this->get_composite(mir_res, vs);
                    MIR_ASSERT(mir_res, e.field_index < states.size(), "Field index out of range");
                    return states[e.field_index];
                }
                else
                {
                    return vs;
                }
                ),
            (Deref,
                const auto& vs = get_lvalue_state(mir_res, *e.val);
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
                const auto& vs_v = get_lvalue_state(mir_res, *e.val);
                const auto& vs_i = get_lvalue_state(mir_res, *e.idx);
                MIR_ASSERT(mir_res, !vs_v.is_composite(), "");
                MIR_ASSERT(mir_res, !vs_i.is_composite(), "");
                //return State(vs_v.is_valid() && vs_i.is_valid());
                MIR_ASSERT(mir_res, vs_i.is_valid(), "Indexing with an invalidated value");
                return vs_v;
                ),
            (Downcast,
                const auto& vs_v = get_lvalue_state(mir_res, *e.val);
                if( vs_v.is_composite() )
                {
                    const auto& states = this->get_composite(mir_res, vs_v);
                    MIR_ASSERT(mir_res, states.size() == 1, "Downcast on composite of invalid size - " << StateFmt(*this, vs_v));
                    return states[0];
                }
                else
                {
                    return vs_v;
                }
                )
            )
            throw "";
        }

        void clear_state(const ::MIR::TypeResolve& mir_res, State& s) {
            if(s.is_composite()) {
                auto& sub_states = this->get_composite(mir_res, s);
                for(auto& ss : sub_states)
                    this->clear_state(mir_res, ss);
                sub_states.clear();
            }
        }

        void set_lvalue_state(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& lv, State new_vs)
        {
            TRACE_FUNCTION_F(lv << " = " << StateFmt(*this, new_vs) << " (from " << StateFmt(*this, get_lvalue_state(mir_res, lv)) << ")");
            TU_MATCHA( (lv), (e),
            (Return,
                this->clear_state(mir_res, return_value);
                return_value = mv$(new_vs);
                ),
            (Argument,
                auto& slot = args.at(e.idx);
                this->clear_state(mir_res, slot);
                slot = mv$(new_vs);
                ),
            (Local,
                auto& slot = locals.at(e);
                this->clear_state(mir_res, slot);
                slot = mv$(new_vs);
                ),
            (Static,
                // Ignore.
                ),
            (Field,
                const auto& cur_vs = get_lvalue_state(mir_res, *e.val);
                if( !cur_vs.is_composite() && cur_vs == new_vs )
                {
                    // Not a composite, and no state change
                }
                else
                {
                    ::std::vector<State>* states_p;
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

                        auto new_cur_vs = this->allocate_composite(n_fields, cur_vs);
                        set_lvalue_state(mir_res, *e.val, State(new_cur_vs));
                        states_p = &this->get_composite(mir_res, new_cur_vs);
                    }
                    else
                    {
                        states_p = &this->get_composite(mir_res, cur_vs);
                    }
                    // Get composite state and assign into it
                    auto& states = *states_p;
                    MIR_ASSERT(mir_res, e.field_index < states.size(), "Field index out of range");
                    this->clear_state(mir_res, states[e.field_index]);
                    states[e.field_index] = mv$(new_vs);
                }
                ),
            (Deref,
                const auto& cur_vs = get_lvalue_state(mir_res, *e.val);
                if( !cur_vs.is_composite() && cur_vs == new_vs )
                {
                    // Not a composite, and no state change
                }
                else
                {
                    ::std::vector<State>* states_p;
                    if( !cur_vs.is_composite() )
                    {
                        //::HIR::TypeRef    tmp;
                        //const auto& ty = mir_res.get_lvalue_type(tmp, *e.val);
                        // TODO: Should this check if the type is Box?

                        auto new_cur_vs = this->allocate_composite(2, cur_vs);
                        set_lvalue_state(mir_res, *e.val, State(new_cur_vs));
                        states_p = &this->get_composite(mir_res, new_cur_vs);
                    }
                    else
                    {
                        states_p = &this->get_composite(mir_res, cur_vs);
                    }
                    // Get composite state and assign into it
                    auto& states = *states_p;
                    MIR_ASSERT(mir_res, states.size() == 2, "Deref with invalid state list size");
                    this->clear_state(mir_res, states[1]);
                    states[1] = mv$(new_vs);
                }
                ),
            (Index,
                const auto& vs_v = get_lvalue_state(mir_res, *e.val);
                const auto& vs_i = get_lvalue_state(mir_res, *e.idx);
                MIR_ASSERT(mir_res, !vs_v.is_composite(), "");
                MIR_ASSERT(mir_res, !vs_i.is_composite(), "");

                MIR_ASSERT(mir_res, vs_v.is_valid(), "Indexing an invalid value");
                MIR_ASSERT(mir_res, vs_i.is_valid(), "Indexing with an invalid index");

                // NOTE: Ignore
                ),
            (Downcast,
                const auto& cur_vs = get_lvalue_state(mir_res, *e.val);
                if( !cur_vs.is_composite() && cur_vs == new_vs )
                {
                    // Not a composite, and no state change
                }
                else
                {
                    ::std::vector<State>* states_p;
                    if( !cur_vs.is_composite() )
                    {
                        auto new_cur_vs = this->allocate_composite(1, cur_vs);
                        set_lvalue_state(mir_res, *e.val, State(new_cur_vs));
                        states_p = &this->get_composite(mir_res, new_cur_vs);
                    }
                    else
                    {
                        states_p = &this->get_composite(mir_res, cur_vs);
                    }

                    // Get composite state and assign into it
                    auto& states = *states_p;
                    MIR_ASSERT(mir_res, states.size() == 1, "Downcast on composite of invalid size - " << *e.val << " - " << this->fmt_state(mir_res, *e.val));
                    this->clear_state(mir_res, states[0]);
                    states[0] = mv$(new_vs);
                }
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
            this->known_state_sets.back().bb_path = ::std::vector<unsigned int>();
            return true;
        }
    };
}

::std::ostream& operator<<(::std::ostream& os, const StateFmt& x)
{
    if(x.s.index == 0) {
        os << "_";
    }
    else if( x.s.index == ~0u ) {
        os << "X";
    }
    else {
        assert(x.s.index-1 < x.vss.inner_states.size());
        const auto& is = x.vss.inner_states[x.s.index-1];
        os << "[";
        for(const auto& s : is)
            os << StateFmt(x.vss, s);
        os << "]";
    }
    return os;
}

namespace std {
    ostream& operator<<(ostream& os, const ValueStates& x)
    {
        auto print_val = [&](auto tag, const State& s) {
            if(s.is_composite()) {
                os << tag << "=" << StateFmt(x,s);
            }
            else if( s.is_valid() ) {
                os << tag;
            }
            else {
            }
            };

        os << "ValueStates(path=[" << x.bb_path << "]";
        print_val(",rv", x.return_value);
        for(unsigned int i = 0; i < x.args.size(); i ++)
            print_val(FMT_CB(ss, ss << ",a" << i;), x.args[i]);
        for(unsigned int i = 0; i < x.locals.size(); i ++)
            print_val(FMT_CB(ss, ss << ",_" << i;), x.locals[i]);
        for(unsigned int i = 0; i < x.drop_flags.size(); i++)
            if(x.drop_flags[i])
                os << ",df" << i;
        os << ")";
        return os;
    }
}


// "Executes" the function, keeping track of drop flags and variable validities
void MIR_Validate_FullValState(::MIR::TypeResolve& mir_res, const ::MIR::Function& fcn)
{
    // TODO: Use a timer to check elapsed CPU time in this function, and check on each iteration
    // - If more than `n` (10?) seconds passes on one function, warn and abort
    //ElapsedTimeCounter    timer;
    ::std::vector<StateSet> block_entry_states( fcn.blocks.size() );

    // Determine value lifetimes (BBs in which Copy values are valid)
    // - Used to mask out Copy value (prevents combinatorial explosion)
    auto lifetimes = MIR_Helper_GetLifetimes(mir_res, fcn, /*dump_debug=*/true);
    DEBUG(lifetimes.m_block_offsets);

    ValueStates state;
    struct H {
        static ::std::vector<State> make_list(size_t n, bool pop) {
            ::std::vector<State>    rv;
            rv.reserve(n);
            while(n--)
                rv.push_back(State(pop));
            return rv;
        }
    };
    state.args = H::make_list(mir_res.m_args.size(), true);
    state.locals = H::make_list(fcn.locals.size(), false);
    state.drop_flags = fcn.drop_flags;

    ::std::vector< ::std::pair<unsigned int, ValueStates> > todo_queue;
    todo_queue.push_back( ::std::make_pair(0, mv$(state)) );
    while( ! todo_queue.empty() )
    {
        auto cur_block = todo_queue.back().first;
        auto state = mv$(todo_queue.back().second);
        todo_queue.pop_back();

        // Mask off any values which aren't valid in the first statement of this block
        {
            for(unsigned i = 0; i < state.locals.size(); i ++)
            {
                /*if( !variables_copy[i] )
                {
                    // Not Copy, don't apply masking
                }
                else*/ if( ! state.locals[i].is_valid() )
                {
                    // Already invalid
                }
                else if( lifetimes.slot_valid(i, cur_block, 0) )
                {
                    // Expected to be valid in this block, leave as-is
                }
                else
                {
                    // Copy value not used at/after this block, mask to false
                    DEBUG("BB" << cur_block << " - _" << i << " - Outside lifetime, discard");
                    state.locals[i] = State(false);
                }
            }
        }

        // If this state already exists in the map, skip
        if( ! block_entry_states[cur_block].add_state(state) )
        {
            DEBUG("BB" << cur_block << " - Nothing new");
            continue ;
        }
        DEBUG("BB" << cur_block << " - " << state);
        state.bb_path.push_back( cur_block );

        const auto& blk = fcn.blocks.at(cur_block);
        for(size_t i = 0; i < blk.statements.size(); i++)
        {
            mir_res.set_cur_stmt(cur_block, i);

            DEBUG(mir_res << blk.statements[i]);

            TU_MATCHA( (blk.statements[i]), (se),
            (Assign,
                if( ENABLE_LEAK_DETECTOR )
                {
                    // TODO: Check if the target isn't valid. Allow if either invaid, or too complex to know.
                }
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
                    if( se.kind == ::MIR::eDropKind::SHALLOW )
                    {
                        // HACK: A move out of a Box generates the following pattern: `[[[[X_]]X]]`
                        // - Ensure that that is the pattern we're seeing here.
                        const auto& vs = state.get_lvalue_state(mir_res, se.slot);

                        MIR_ASSERT(mir_res, vs.index != ~0u, "Shallow drop on fully-valid value - " << se.slot);

                        // Box<T> - Wrapper around Unique<T>
                        MIR_ASSERT(mir_res, vs.is_composite(), "Shallow drop on non-composite state - " << se.slot << " (state=" << StateFmt(state,vs) << ")");
                        const auto& sub_states = state.get_composite(mir_res, vs);
                        MIR_ASSERT(mir_res, sub_states.size() == 2, "Shallow drop of slot with incorrect state shape (state=" << StateFmt(state,vs) << ")");
                        MIR_ASSERT(mir_res, sub_states[0].is_valid(), "Shallow drop on deallocated Box - " << se.slot << " (state=" << StateFmt(state,vs) << ")");
                        // TODO: This is leak protection, enable it once the rest works
                        if( ENABLE_LEAK_DETECTOR )
                        {
                            MIR_ASSERT(mir_res, !sub_states[1].is_valid(), "Shallow drop on populated Box - " << se.slot << " (state=" << StateFmt(state,vs) << ")");
                        }

                        state.set_lvalue_state(mir_res, se.slot, State(false));
                    }
                    else
                    {
                        state.move_lvalue(mir_res, se.slot);
                    }
                }
                ),
            (ScopeEnd,
                // TODO: Mark all mentioned variables as invalid
                )
            )
        }

        state.garbage_collect();

        mir_res.set_cur_stmt_term(cur_block);
        DEBUG(mir_res << " " << blk.terminator);
        TU_MATCHA( (blk.terminator), (te),
        (Incomplete,
            ),
        (Return,
            state.ensure_lvalue_valid(mir_res, ::MIR::LValue::make_Return({}));
            if( ENABLE_LEAK_DETECTOR )
            {
                auto ensure_dropped = [&](const State& s, const ::MIR::LValue& lv) {
                    if( s.is_valid() ) {
                        // Check if !Copy
                        ::HIR::TypeRef  tmp;
                        const auto& ty = mir_res.get_lvalue_type(tmp, lv);
                        if( mir_res.m_resolve.type_is_copy(mir_res.sp, ty) ) {
                        }
                        else {
                            MIR_BUG(mir_res, "Value " << lv << " was not dropped at end of function");
                        }
                    }
                    };
                for(unsigned i = 0; i < state.locals.size(); i ++ ) {
                    ensure_dropped(state.locals[i], ::MIR::LValue::make_Local(i));
                }
                for(unsigned i = 0; i < state.args.size(); i ++ ) {
                    ensure_dropped(state.args[i], ::MIR::LValue::make_Argument({i}));
                }
            }
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
        (SwitchValue,
            state.ensure_lvalue_valid(mir_res, te.val);
            for(size_t i = 0; i < te.targets.size(); i ++)
            {
                todo_queue.push_back( ::std::make_pair(te.targets[i], state.clone()) );
            }
            todo_queue.push_back( ::std::make_pair(te.def_target, mv$(state)) );
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

