/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/borrow_check.cpp
 * - MIR Borrow Checker
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
    /// Value states
    enum class ValState
    {
        Uninit, // No value written yet
        FullInit,   // Value written, can be read
        Shared, // immutably borrowed
        Frozen, // mutably borrowed
    };
    /// Borrow status for a single variable
    struct VarState
    {
        /// State bits, see `ValState`
        unsigned state : 2;
        /// Index into `FcnState.inner` for the _first_ partially-borrowed for this variable
        unsigned partial_idx : 14;    // If non-zero, it's a partial init/borrow

        VarState(): state(static_cast<unsigned>(ValState::Uninit)), partial_idx(0) {}
        VarState(ValState vs): state(static_cast<unsigned>(vs)), partial_idx(0) {}
    };

    struct FcnState
    {
        VarState    retval;
        std::vector<VarState>   args;
        std::vector<VarState>   locals;
        /// Mutable state flags for statics
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
                    //MIR_BUG(state, "Borrow check failure: ");
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
            throw "";
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
            throw "";
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

        void set_state(const ::MIR::TypeResolve& state, const MIR::LValue& lv, ValState target) {
            VarState* rv = &this->get_state_root_mut(lv.m_root);
            for(const auto& w : lv.m_wrappers)
            {
                TU_MATCH_HDRA( (w), {)
                TU_ARMA(Deref, e) {
                    // Can't set to `Uninit` through a deref
                    MIR_ASSERT(state, target != ValState::Uninit, "Attempting to move out of borrow");
                    }
                TU_ARMA(Field, e) {
                    }
                TU_ARMA(Downcast, e) {
                    }
                TU_ARMA(Index, e) {
                    // Can't set to `Uninit` through an index
                    MIR_ASSERT(state, target != ValState::Uninit, "Attempting to move through indexing");
                    }
                }
            }
            size_t i;
            for(i = 0; i < lv.m_wrappers.size(); i ++)
            {
                const auto& w = lv.m_wrappers[i];
                TU_MATCH_HDRA( (w), {)
                TU_ARMA(Deref, e) {
                    // Doesn't consume an inner, but stops the lookup?
                    // - Could track borrows through a deref, but can't track/allow moves
                    break;
                    }
                TU_ARMA(Field, e) {
                    // Since partial is set, look it up
                    //if( !rv->has_partial() ) {
                    //    rv->partial_idx = this->allocate_partial(state, *ty_p);
                    //}
                    rv = &this->inner[static_cast<size_t>(rv->partial_idx) + e];
                    }
                TU_ARMA(Downcast, e) {
                    // Doesn't consume an inner
                    }
                TU_ARMA(Index, e) {
                    // Doesn't consume an inner, but stops the lookup
                    break;
                    }
                }
            }
            while(i --)
            {
                switch(static_cast<ValState>(rv->state))
                {
                case ValState::Uninit:
                case ValState::FullInit:
                    rv->state = static_cast<unsigned>(target);
                    break;
                case ValState::Shared:
                    rv->state = static_cast<unsigned>(target);
                    break;
                case ValState::Frozen:
                    rv->state = static_cast<unsigned>(target);
                    break;
                }
            }
        }

        void move_lvalue(const ::MIR::TypeResolve& state, const MIR::LValue& lv) {
            // Must be `init` (or if Copy, `Shared`)
            if( state.lvalue_is_copy(lv) ) {
                check_inner_state(state, lv, [&](const ValState vs){ return vs == ValState::FullInit || vs == ValState::Shared; });
            }
            else {
                check_inner_state(state, lv, [&](const ValState vs){ return vs == ValState::FullInit; });
                set_state(state, lv, ValState::Uninit);
            }
        }
        void write_lvalue(const ::MIR::TypeResolve& state, const MIR::LValue& lv) {
            check_inner_state(state, lv, [&](const ValState vs){ return vs != ValState::Shared || vs != ValState::Frozen; });
            set_state(state, lv, ValState::FullInit);
        }
        void borrow_lvalue(const ::MIR::TypeResolve& state, ::HIR::BorrowType bt, const MIR::LValue& lv) {
            switch(bt) {
            case ::HIR::BorrowType::Owned:
            case ::HIR::BorrowType::Unique:
                check_inner_state(state, lv, [&](const ValState vs){ return vs == ValState::FullInit; });
                set_state(state, lv, ValState::Frozen);
                break;
            case ::HIR::BorrowType::Shared:
                check_inner_state(state, lv, [&](const ValState vs){ return vs == ValState::FullInit || vs == ValState::Shared; });
                set_state(state, lv, ValState::Shared);
                break;
            }
        }
    };


    struct StmtRef
    {
        MIR::BasicBlockId   block;
        size_t  stmt_idx;

        StmtRef(MIR::BasicBlockId bb, size_t stmt): block(bb), stmt_idx(stmt) {}
    };
    struct SubStmtRef
    {
        StmtRef stmt;
        size_t  sub_idx;

        SubStmtRef(StmtRef stmt, size_t sub_idx): stmt(stmt), sub_idx(sub_idx) {}
        SubStmtRef(MIR::BasicBlockId bb, size_t stmt, size_t sub_idx): stmt(bb, stmt), sub_idx(sub_idx) {}
    };
    struct ValidRegion
    {
        StmtRef start;
        std::vector<MIR::BasicBlockId>  path;
        SubStmtRef end;
    };


    class BorrowState
    {
        const ::MIR::TypeResolve&   state;

        struct LifetimeInfo
        {
            SubStmtRef  origin;
            MIR::LValue value;
        };
        std::vector<LifetimeInfo>   local_lifetimes;

        struct LifetimeIvar
        {
            //unsigned    target_binding;
            std::vector<HIR::LifetimeRef>   srcs;
            std::vector<HIR::LifetimeRef>   dsts;
        };
        std::vector<LifetimeIvar>   ivar_lifetimes;

    public:
        BorrowState(const ::MIR::TypeResolve& state)
            : state(state)
        {
        }

        /// <summary>
        /// Assign two lifetimes (e.g. via an assignment, or a function call)
        /// </summary>
        /// <param name="target">Target lifetime (LHS or receiver)</param>
        /// <param name="src">Source lifetime</param>
        void lifetime_assign(const HIR::LifetimeRef& target, const HIR::LifetimeRef& src)
        {
            DEBUG(state << target << " = " << src);
            // One of these must be an ivar?
            // - Record the to/from for each.
            if( auto* iv = opt_ivar(target) ) {
                iv->srcs.push_back(src);
            }
            if( auto* iv = opt_ivar(src) ) {
                iv->dsts.push_back(target);
            }
        }

        void type_assign_pp(const HIR::PathParams& dst, const HIR::PathParams& src) {
            MIR_ASSERT(state, dst.m_lifetimes.size() == src.m_lifetimes.size(), "Param count error - " << dst << " == " << src);
            MIR_ASSERT(state, dst.m_types    .size() == src.m_types    .size(), "Param count error - " << dst << " == " << src);
            for(size_t i = 0; i < dst.m_lifetimes.size(); i ++) {
                lifetime_assign(dst.m_lifetimes[i], src.m_lifetimes[i]);
            }
            for(size_t i = 0; i < dst.m_types.size(); i ++) {
                type_assign(dst.m_types[i], src.m_types[i]);
            }
        }
        void type_assign(const HIR::TypeRef& dst_ty, const HIR::TypeRef& src_ty)
        {
            MIR_ASSERT(state, dst_ty.data().tag() == src_ty.data().tag(), dst_ty << " != " << src_ty);
            TU_MATCH_HDRA( (dst_ty.data(), src_ty.data()),  { )
            TU_ARMA(Infer, de, se) MIR_BUG(state, "Unexpected infer - " << dst_ty << ", " << src_ty);
            TU_ARMA(Generic, de, se) {}
            TU_ARMA(Diverge, de, se) {}
            TU_ARMA(Primitive, de, se) {}
            TU_ARMA(Borrow, de, se) {
                lifetime_assign(de.lifetime, se.lifetime);
                type_assign(de.inner, se.inner);
                }
            TU_ARMA(Pointer, de, se) {
                type_assign(de.inner, se.inner);
                }
            TU_ARMA(TraitObject, de, se) {
                lifetime_assign(de.m_lifetime, se.m_lifetime);
                type_assign_pp(de.m_trait.m_path.m_params, se.m_trait.m_path.m_params);
                // TODO: Markers
                }
            TU_ARMA(Closure, de, se) MIR_BUG(state, "Unexpected Closure");
            TU_ARMA(Generator, de, se) MIR_BUG(state, "Unexpected Generator");
            TU_ARMA(ErasedType, de, se) MIR_BUG(state, "Unexpected ErasedType");
            TU_ARMA(Path, de, se) {
                MIR_ASSERT(state, de.binding == se.binding, dst_ty << " != " << src_ty);
                MIR_ASSERT(state, de.path.m_data.tag() == se.path.m_data.tag(), dst_ty << " != " << src_ty);
                TU_MATCH_HDRA( (de.path.m_data, se.path.m_data), { )
                TU_ARMA(Generic, dpe, spe) {
                    type_assign_pp(dpe.m_params, spe.m_params);
                    }
                TU_ARMA(UfcsInherent, dpe, spe) {
                    type_assign_pp(dpe.impl_params, spe.impl_params);
                    type_assign(dpe.type, spe.type);
                    type_assign_pp(dpe.params, spe.params);
                    }
                TU_ARMA(UfcsKnown, dpe, spe) {
                    type_assign_pp(dpe.trait.m_params, spe.trait.m_params);
                    type_assign(dpe.type, spe.type);
                    type_assign_pp(dpe.params, spe.params);
                    }
                TU_ARMA(UfcsUnknown, dpe, spe) MIR_BUG(state, "Unexpected UfcsUnknown - " << dst_ty << ", " << src_ty);
                }
                }
            TU_ARMA(Array, de, se) {
                type_assign(de.inner, se.inner);
                }
            TU_ARMA(Slice, de, se) {
                type_assign(de.inner, se.inner);
                }
            TU_ARMA(Tuple, de, se) {
                assert(de.size() == se.size());
                for(size_t i = 0; i < de.size(); i ++) {
                    type_assign(de[i], se[i]);
                }
                }
            TU_ARMA(NamedFunction, de, se) {
                MIR_TODO(state, "NamedFunction MIR borrowcheck");
                }
            TU_ARMA(Function, de, se) {
                MIR_ASSERT(state, de.m_arg_types.size() == se.m_arg_types.size(), "Arg count error");
                for(size_t i = 0; i < de.m_arg_types.size(); i ++)
                {
                    type_assign(de.m_arg_types[i], se.m_arg_types[i]);
                }
                type_assign(de.m_rettype, se.m_rettype);
                }
            }
        }

        void handle_param(const HIR::TypeRef& target, const MIR::Param& param, size_t ofs)
        {
            if( const auto* b = param.opt_Borrow() ) {
                HIR::TypeRef    tmp;
                auto src_ty = state.get_lvalue_type(tmp, b->val).clone_shallow();
                auto lft = borrow_lvalue(ofs, b->type, b->val);
                type_assign(target, ::HIR::TypeRef::new_borrow(b->type, mv$(src_ty), lft));
            }
            else {
                HIR::TypeRef    tmp;
                type_assign(target, state.get_param_type(tmp, param));
            }
        }

        void do_assign(const MIR::LValue& lv, const HIR::TypeRef& src_ty)
        {
            HIR::TypeRef    tmp;
            const auto& dst_ty = state.get_lvalue_type(tmp, lv);
            type_assign(dst_ty, src_ty);
        }

        /// <summary>
        /// Borrow a lvalue, returning a lifetime reference created to point at the current position
        /// </summary>
        /// <param name="stmt_inner_ofs">Offset within the statement (e.g. argument index)</param>
        /// <param name="lv">LValue</param>
        HIR::LifetimeRef borrow_lvalue(size_t stmt_inner_ofs, HIR::BorrowType bt, const MIR::LValue& lv)
        {
            MIR::LValue::CRef   lvr(lv);
            // Unwrap until a deref or the bottom value
            while( lvr.wrapper_count() > 0 && !lvr.is_Deref() ) {
                lvr = lvr.inner_ref();
            }

            TU_MATCH_HDRA( (lvr), { )
            TU_ARMA(Downcast, _)    throw "";
            TU_ARMA(Index, _)    throw "";
            TU_ARMA(Field, _)    throw "";

            TU_ARMA(Deref, _) {
                HIR::TypeRef    tmp;
                const auto& inner_ty = state.get_lvalue_type(tmp, lvr.inner_ref());
                if( const auto* tep = inner_ty.data().opt_Borrow() ) {
                    return tep->lifetime;
                }
                else if( inner_ty.data().is_Pointer() ) {
                    // TODO: Return an unbound lifetime
                    return HIR::LifetimeRef::new_static();
                }
                else {
                    MIR_BUG(state, "Unexpected type: " << inner_ty);
                }
                }
            
            TU_ARMA(Static, _)
                return HIR::LifetimeRef::new_static();
            TU_ARMA(Return, _)
                MIR_BUG(state, "Borrowing return slot");
            TU_ARMA(Local, _) {
                // Allocate/find a local borrow reference for this slot
                // - Record the entire lvalue for this borrow
                return this->allocate_local( SubStmtRef(state.get_cur_block(), state.get_cur_stmt_ofs(), stmt_inner_ofs), lv.clone() );
                }
            TU_ARMA(Argument, _) {
                // Allocate/find a local borrow reference for this slot
                // - Record the entire lvalue for this borrow
                return this->allocate_local( SubStmtRef(state.get_cur_block(), state.get_cur_stmt_ofs(), stmt_inner_ofs), lv.clone() );
                }
            }
            throw "";
        }

        HIR::LifetimeRef allocate_ivar()
        {
            auto idx = ivar_lifetimes.size();
            ivar_lifetimes.push_back(LifetimeIvar());
            return HIR::LifetimeRef(static_cast<uint32_t>(idx + 0x14000));
        }
    private:
        HIR::LifetimeRef allocate_local(SubStmtRef origin, MIR::LValue value)
        {
            DEBUG(state << "New local: " << value);
            auto idx = local_lifetimes.size();
            local_lifetimes.push_back(LifetimeInfo { origin, std::move(value) });
            assert(idx < (0x4000 - 0));
            return HIR::LifetimeRef(static_cast<uint32_t>(idx + 0x10000));
        }

        LifetimeIvar* opt_ivar(const HIR::LifetimeRef& lr)
        {
            if( 0x14000 <= lr.binding )
            {
                return &ivar_lifetimes.at(lr.binding - 0x14000);
            }
            return nullptr;
        }
    };
}

void MIR_BorrowCheck(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    static Span sp;
    TRACE_FUNCTION_F(path);
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };

    DEBUG(FMT_CB(ss, MIR_Dump_Fcn(ss, fcn)));

    BorrowState borrow_state { state };
    
    // 0. Create liftime names/references for all borrows
    // - Each type instance gets its own new lifetime reference
    {
        TRACE_FUNCTION_FR("Fill", "Fill");
        struct V: 
            public MIR::visit::VisitorMut
        {
            const ::MIR::TypeResolve&   state;
            BorrowState&    borrow_state;
            V(const ::MIR::TypeResolve& state, BorrowState& borrow_state)
                : state(state)
                , borrow_state(borrow_state)
            {}

            void visit_lifetime_ref(::HIR::LifetimeRef& lr)
            {
                if( lr.binding == ::HIR::LifetimeRef::UNKNOWN )
                {
                    lr = borrow_state.allocate_ivar();
                }
            }

            void visit_pathparams(::HIR::PathParams& pp)
            {
                for(auto& lr : pp.m_lifetimes)
                {
                    visit_lifetime_ref(lr);
                }
            }

            void visit_type(::HIR::TypeRef& t) override
            {
                // Visit inner types, allocating lifetimes
                visit_ty_with_mut(t, [&](HIR::TypeRef& ty)->bool {
                    TU_MATCH_HDRA( (ty.data_mut()), { )
                    default:
                        // No referenced lifetimes
                        break;
                    TU_ARMA(Path, te) {
                        // Visit path params
                        TU_MATCH_HDRA( (te.path.m_data), {)
                        TU_ARMA(Generic, pe) {
                            this->visit_pathparams(pe.m_params);
                            }
                        TU_ARMA(UfcsKnown, pe) {
                            this->visit_pathparams(pe.trait.m_params);
                            this->visit_pathparams(pe.params);
                            }
                        TU_ARMA(UfcsInherent, pe) {
                            this->visit_pathparams(pe.params);
                            }
                        TU_ARMA(UfcsUnknown, pe) MIR_BUG(state, "Unexpected UfcsUnknown - " << ty);
                        }
                        }
                    TU_ARMA(Borrow, te) {
                        visit_lifetime_ref(te.lifetime);
                        }
                    TU_ARMA(TraitObject, te) {
                        visit_lifetime_ref(te.m_lifetime);
                        }
                    TU_ARMA(ErasedType, te) MIR_BUG(state, "Unexpected " << ty);
                    }
                    return false;
                    });
            };
        }   v { state, borrow_state };
        v.visit_function(state, fcn);
    }
    // - Run inference/assignment of lifetime references (between named lifetimes and borrows)
    {
        TRACE_FUNCTION_FR("Assign", "Assign");
        for(auto& blk : fcn.blocks)
        {
            for(auto& stmt : blk.statements)
            {
                state.set_cur_stmt(&blk - fcn.blocks.data(), &stmt - blk.statements.data());
                DEBUG(state << stmt);
                TU_MATCH_HDRA( (stmt), {)
                TU_ARMA(Assign, se) {
                    TU_MATCH_HDRA((se.src), {)
                    TU_ARMA(Use, rse) {
                        HIR::TypeRef    tmp;
                        borrow_state.do_assign(se.dst, state.get_lvalue_type(tmp, rse));
                        }
                    TU_ARMA(Borrow, rse) {
                        HIR::TypeRef    tmp;
                        auto src_ty = state.get_lvalue_type(tmp, rse.val).clone_shallow();
                        auto lft = borrow_state.borrow_lvalue(0, rse.type, rse.val);
                        borrow_state.do_assign(se.dst, HIR::TypeRef::new_borrow(rse.type, mv$(src_ty), lft));
                        }
                    TU_ARMA(Array, rse) {
                        HIR::TypeRef    tmp;
                        const auto& dst_ty = state.get_lvalue_type(tmp, se.dst).data().as_Array().inner;
                        for(size_t i = 0; i < rse.vals.size(); i ++) {
                            borrow_state.handle_param(dst_ty, rse.vals[i], i);
                        }
                        }
                    TU_ARMA(SizedArray, rse) {
                        HIR::TypeRef    tmp;
                        const auto& dst_ty = state.get_lvalue_type(tmp, se.dst).data().as_Array().inner;
                        borrow_state.handle_param(dst_ty, rse.val, 0);
                        }
                    TU_ARMA(Struct, rse) {
                        const auto& str = resolve.m_crate.get_struct_by_path(state.sp, rse.path.m_path);
                        MonomorphStatePtr   ms(nullptr, &rse.path.m_params, nullptr);
                        HIR::TypeRef    tmp;
                        auto maybe_monomorph = [&](const auto& ty)->const HIR::TypeRef& {
                            return resolve.monomorph_expand_opt(sp, tmp, ty, ms);
                        };
                        auto get_field_ty = [&](size_t field_index)->const HIR::TypeRef& {
                            TU_MATCH_HDRA( (str.m_data), {)
                            TU_ARMA(Unit, se) {
                                MIR_BUG(state, "Field on unit-like struct - " << rse.path);
                                }
                            TU_ARMA(Tuple, se) {
                                MIR_ASSERT(state, field_index < se.size(), "Field index out of range in tuple-struct " << rse.path);
                                return maybe_monomorph(se[field_index].ent);
                                }
                            TU_ARMA(Named, se) {
                                MIR_ASSERT(state, field_index < se.size(), "Field index out of range in struct " << rse.path);
                                return maybe_monomorph(se[field_index].second.ent);
                                }
                            }
                            throw "";
                            };
                        for(size_t i = 0; i < rse.vals.size(); i ++)
                        {
                            borrow_state.handle_param(get_field_ty(i), rse.vals[i], i);
                        }
                        }
                    TU_ARMA(EnumVariant, rse) {
                        const auto& enm = resolve.m_crate.get_enum_by_path(state.sp, rse.path.m_path);
                        MonomorphStatePtr   ms(nullptr, &rse.path.m_params, nullptr);
                        HIR::TypeRef    tmp;
                        //auto maybe_monomorph = [&](const auto& ty)->const HIR::TypeRef& {
                        //    return resolve.monomorph_expand_opt(sp, tmp, ty, ms);
                        //};
                        if( rse.vals.size() > 0 ) {
                            MIR_ASSERT(state, enm.m_data.is_Data(), "");
                            const auto& variants = enm.m_data.as_Data();
                            MIR_ASSERT(state, rse.index < variants.size(), "Variant index out of range for " << rse.path);
                            const auto& variant = variants[rse.index];

                            const auto& var_ty = resolve.monomorph_expand_opt(sp, tmp, variant.type, MonomorphStatePtr(nullptr, &rse.path.m_params, nullptr));
                            const auto& str = *var_ty.data().as_Path().binding.as_Struct();
                            const auto& s_path = var_ty.data().as_Path().path.m_data.as_Generic();
                            auto maybe_monomorph = [&](const HIR::TypeRef& ty)->const HIR::TypeRef& {
                                return resolve.monomorph_expand_opt(sp, tmp, ty, MonomorphStatePtr(nullptr, &s_path.m_params, nullptr));
                            };
                            TU_MATCH_HDRA( (str.m_data), {)
                            TU_ARMA(Unit, se) {
                                }
                            TU_ARMA(Tuple, se) {
                                MIR_ASSERT(state, se.size() == rse.vals.size(), "Field index out of range in tuple enum variant " << rse.path);
                                for(size_t i = 0; i < rse.vals.size(); i ++)
                                {
                                    borrow_state.handle_param(maybe_monomorph(se[i].ent), rse.vals[i], i);
                                }
                                }
                            TU_ARMA(Named, se) {
                                MIR_ASSERT(state, se.size() == rse.vals.size(), "Field index out of range in named enum variant " << rse.path);
                                for(size_t i = 0; i < rse.vals.size(); i ++)
                                {
                                    borrow_state.handle_param(maybe_monomorph(se[i].second.ent), rse.vals[i], i);
                                }
                                }
                            }
                        }
                        }
                    TU_ARMA(UnionVariant, rse) {
                        MIR_TODO(state, "");
                        }
                    TU_ARMA(Tuple, rse) {
                        HIR::TypeRef    tmp;
                        const auto& dst_ty = state.get_lvalue_type(tmp, se.dst);
                        const auto& de = dst_ty.data().as_Tuple();
                        MIR_ASSERT(state, de.size() == rse.vals.size(), "Tuple size and rvalue mismatch");
                        for(size_t i = 0; i < rse.vals.size(); i++) {
                            borrow_state.handle_param(de[i], rse.vals[i], i);
                        }
                        }
                    TU_ARMA(DstPtr, rse) {
                        }
                    TU_ARMA(DstMeta, rse) {
                        // TODO &'static for vtables
                        }
                    TU_ARMA(MakeDst, rse) {
                        HIR::TypeRef    tmp;
                        const auto& dst_ty = state.get_lvalue_type(tmp, se.dst);
                        if( dst_ty.data().is_Borrow() ) {
                            if( rse.ptr_val.is_Borrow() ) {
                                // TODO: Make the borrow?
                            }
                            else {
                                HIR::TypeRef    tmp2;
                                const auto& src_ty = state.get_param_type(tmp2, rse.ptr_val);
                                borrow_state.lifetime_assign(dst_ty.data().as_Borrow().lifetime, src_ty.data().as_Borrow().lifetime);
                            }
                        }
                        }
                    TU_ARMA(UniOp, rse) {
                        }
                    TU_ARMA(BinOp, rse) {
                        }
                    TU_ARMA(Constant, rse) {
                        borrow_state.do_assign(se.dst, state.get_const_type(rse));
                        }
                    TU_ARMA(Cast, rse) {
                        HIR::TypeRef    tmp;
                        const auto& dst_ty = state.get_lvalue_type(tmp, se.dst);
                        HIR::TypeRef    tmp2;
                        const auto& src_ty = state.get_lvalue_type(tmp2, rse.val);
                        // Handle both being borrows
                        if( dst_ty.data().is_Borrow() && src_ty.data().is_Borrow() ) {
                            borrow_state.lifetime_assign(dst_ty.data().as_Borrow().lifetime, src_ty.data().as_Borrow().lifetime);
                        }
                        }
                    }
                    }
                TU_ARMA(SetDropFlag, se) {}
                TU_ARMA(ScopeEnd, se) { /* todo */ }
                TU_ARMA(Drop, se) { /* todo */ }
                TU_ARMA(Asm, se) {}
                TU_ARMA(Asm2, se) {}
                }
                // Note: Also need to pass through function calls, assignments, and structs
                // - Drop needs to be handled for anything with drop glue (as it counts as a use of contained borrows)
            }

            state.set_cur_stmt_term(&blk - fcn.blocks.data());
            DEBUG(state << blk.terminator);
            TU_MATCH_HDRA( (blk.terminator), { )
            default:
                break;
            TU_ARMA(Call, e) {
                TU_MATCH_HDRA( (e.fcn), {)
                TU_ARMA(Intrinsic, fe) {
                    }
                TU_ARMA(Value, fe) {
                    HIR::TypeRef    tmp;
                    const auto& ty = state.get_lvalue_type(tmp, fe);
                    const auto& fcn = ty.data().as_Function();
                    // TODO: HKTs
                    MIR_ASSERT(state, fcn.m_arg_types.size() == e.args.size(), "");
                    for(size_t i = 0; i < fcn.m_arg_types.size(); i ++)
                    {
                        borrow_state.handle_param(fcn.m_arg_types[i], e.args[i], i);
                    }
                    borrow_state.do_assign(e.ret_val, fcn.m_rettype);
                    }
                TU_ARMA(Path, fe) {
                    HIR::TypeRef    tmp;

                    MonomorphState  ms;
                    auto v = resolve.get_value(state.sp, fe, ms, true);
                    auto maybe_monomorph = [&](const ::HIR::TypeRef& ty)->const HIR::TypeRef& {
                        return resolve.monomorph_expand_opt(state.sp, tmp, ty, ms);
                    };

                    const auto& fcn = *v.as_Function();
                    MIR_ASSERT(state, fcn.m_args.size() <= e.args.size(), "");
                    for(size_t i = 0; i < fcn.m_args.size(); i ++)
                    {
                        // Handle the param, unify types.
                        const auto& exp_ty = maybe_monomorph(fcn.m_args[i].second);
                        DEBUG("ARG" << i << " " << exp_ty << " = " << e.args[i]);

                        borrow_state.handle_param(exp_ty, e.args[i], i);
                    }
                    const auto& rv_ty = maybe_monomorph(fcn.m_return);
                    DEBUG("RV" << " " << e.ret_val << " = " << rv_ty);
                    borrow_state.do_assign(e.ret_val, rv_ty);
                    }
                }
                }
            }
        }
    }

    auto val_states = FcnState(args.size(), fcn.locals.size());

    // 1. Determine the lifetime (scope) of each variable (from assignment to last use)
    // - Needs to represent disjoint lifetimes (i.e. same variable assigned multiple times)
    // - Walk the graph, finding assignments of locals and tracking last use until next assignment
    //   > At next assignment (or drop/move), record that lifetime span (as start,path,end)

    // 2. Run full state tracking, including tracking of borrow sources.
    // TODO: Figure out the rest
}

void MIR_BorrowCheck_Crate(::HIR::Crate& crate)
{
    ::MIR::OuterVisitor    ov { crate, [&](const auto& res, const auto& p, ::HIR::ExprPtr& expr_ptr, const auto& args, const auto& ty){
        MIR_BorrowCheck(res, p, expr_ptr.get_mir_or_error_mut(Span()), args, ty);
    } };
    ov.visit_crate(crate);
}


