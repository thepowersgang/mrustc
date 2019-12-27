/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/borrow_check.cpp
 * - Borrow Checker
 *
 */
#include "main_bindings.hpp"
#include "mir.hpp"
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>
#include <mir/helpers.hpp>
#include <mir/operations.hpp>
#include <mir/visit_crate_mir.hpp>
#include <algorithm>
#include <iomanip>

namespace {
    enum class ValState
    {
        Uninit, // No value written yet
        FullInit,   // Value written, can be read
        Shared, // immutably borrowed
        Frozen, // mutably borrowed
    };
    struct VarState
    {
        unsigned state : 2;
        unsigned partial_idx : 14;    // If non-zero, it's a partial init/borrow

        VarState(): state(static_cast<unsigned>(ValState::Uninit)), partial_idx(0) {}
        VarState(ValState vs): state(static_cast<unsigned>(ValState::Shared)), partial_idx(0) {}
    };

    struct FcnState
    {
        VarState    retval;
        std::vector<VarState>   args;
        std::vector<VarState>   locals;
        std::map<HIR::Path, VarState>   static_mut;

        std::vector<VarState>   inner;

        FcnState(size_t n_args, size_t n_locals)
            :retval()
            ,args(n_args)
            ,locals(n_locals)
        {
        }

        void check_inner_state(const ::MIR::TypeResolve& state, const MIR::LValue& lv, std::function<bool(ValState vs)> cb) const
        {
            const auto& val_state = get_state(state, lv);
            if( val_state.partial_idx != 0 )
            {
                // Recurse into all inner entries
            }
            else
            {
                if( !cb(static_cast<ValState>(val_state.state)) )
                {
                    // Error!
                }
            }
        }

        const VarState& get_state_root(const MIR::LValue::Storage& lv_root) const {
            TU_MATCH_HDRA( (lv_root), {)
            TU_ARMA(Return, e)
                return retval;
            TU_ARMA(Local, e)
                return locals.at(e);
            TU_ARMA(Argument, e)
                return args.at(e);
            TU_ARMA(Static, e) {
                // TODO: If it's a static mut, return ValState::FullInit?
                static const VarState vs_static = VarState(ValState::Shared);
                return vs_static;
                }
            }
        }
        VarState& get_state_root_mut(const MIR::LValue::Storage& lv_root) {
            TU_MATCH_HDRA( (lv_root), {)
                TU_ARMA(Return, e)
                return retval;
            TU_ARMA(Local, e)
                return locals.at(e);
            TU_ARMA(Argument, e)
                return args.at(e);
            TU_ARMA(Static, e) {
                // TODO: If it's a static mut, return ValState::FullInit?
                auto it = static_mut.find(e);
                if( it == static_mut.end() )
                    it = static_mut.insert(::std::make_pair( e.clone(), VarState(ValState::FullInit) )).first;
                return it->second;
                }
            }
        }
        const VarState& get_state(const ::MIR::TypeResolve& state, const MIR::LValue& lv) const {
            const VarState* rv = &this->get_state_root(lv.m_root);
            for(const auto& w : lv.m_wrappers)
            {
                if(rv->partial_idx == 0)
                    break;
                TU_MATCH_HDRA( (w), {)
                TU_ARMA(Deref, e) {
                    MIR_TODO(state, "get_state - Deref");
                    }
                TU_ARMA(Field, e) {
                    MIR_TODO(state, "get_state - Field");
                    }
                TU_ARMA(Downcast, e) {
                    MIR_TODO(state, "get_state - Variant");
                    }
                TU_ARMA(Index, e) {
                    return *rv;
                    }
                }
            }
            return *rv;
        }
        VarState* get_state_mut(const ::MIR::TypeResolve& state, const MIR::LValue& lv, bool allow_parent) {
            VarState* rv = &this->get_state_root_mut(lv.m_root);
            for(const auto& w : lv.m_wrappers)
            {
                TU_MATCH_HDRA( (w), {)
                TU_ARMA(Deref, e) {
                    MIR_TODO(state, "get_state_mut - Deref");
                    }
                TU_ARMA(Field, e) {
                    MIR_TODO(state, "get_state_mut - Field");
                    }
                TU_ARMA(Downcast, e) {
                    MIR_TODO(state, "get_state_mut - Variant");
                    }
                TU_ARMA(Index, e) {
                    if(allow_parent)
                        return rv;
                    return nullptr;
                    }
                }
            }
            return rv;
        }

        void move_lvalue(const ::MIR::TypeResolve& state, const MIR::LValue& lv) {
            // Must be `init` (or if Copy, `Shared`)
            if( state.lvalue_is_copy(lv) )
            {
                check_inner_state(state, lv, [&](const ValState vs){ return vs == ValState::FullInit || vs == ValState::Shared; });
            }
            else
            {
                check_inner_state(state, lv, [&](const ValState vs){ return vs == ValState::FullInit; });
                // Need to update state
                // - For this to work, we need a direct handle.
                //auto* h = get_state_mut(state, lv, /*allow_parent=*/false);
            }
        }
        void write_lvalue(const ::MIR::TypeResolve& state, const MIR::LValue& lv) {
        }
        void borrow_lvalue(const ::MIR::TypeResolve& state, ::HIR::BorrowType bt, const MIR::LValue& lv) {
        }
    };
}

void MIR_BorrowCheck(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    static Span sp;
    TRACE_FUNCTION_F(path);
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };
    auto val_states = FcnState(args.size(), fcn.locals.size());

    // 1. Determine the lifetime (scope) of each variable (from assignment to last use)
    // 2. Run full state tracking, including tracking of borrow sources.
    // TODO: Figure out the rest
}
