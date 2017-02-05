/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/optimise.cpp
 * - MIR Optimisations
 *
 */
#include "main_bindings.hpp"
#include "mir.hpp"
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>
#include <mir/helpers.hpp>
#include <mir/operations.hpp>
#include <mir/visit_crate_mir.hpp>

namespace {
    ::MIR::BasicBlockId get_new_target(const ::MIR::TypeResolve& state, ::MIR::BasicBlockId bb)
    {
        const auto& target = state.get_block(bb);
        if( target.statements.size() != 0 )
        {
            return bb;
        }
        else if( !target.terminator.is_Goto() )
        {
            return bb;
        }
        else
        {
            // Make sure we don't infinite loop
            if( bb == target.terminator.as_Goto() )
                return bb;

            auto rv = get_new_target(state, target.terminator.as_Goto());
            DEBUG(bb << " => " << rv);
            return rv;
        }
    }

    enum class ValUsage {
        Read,
        Write,
        Borrow,
    };

    bool visit_mir_lvalue_mut(::MIR::LValue& lv, ValUsage u, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        if( cb(lv, u) )
            return true;
        TU_MATCHA( (lv), (e),
        (Variable,
            ),
        (Argument,
            ),
        (Temporary,
            ),
        (Static,
            ),
        (Return,
            ),
        (Field,
            return visit_mir_lvalue_mut(*e.val, u, cb);
            ),
        (Deref,
            return visit_mir_lvalue_mut(*e.val, ValUsage::Read, cb);
            ),
        (Index,
            bool rv = false;
            rv |= visit_mir_lvalue_mut(*e.val, u, cb);
            rv |= visit_mir_lvalue_mut(*e.idx, ValUsage::Read, cb);
            return rv;
            ),
        (Downcast,
            return visit_mir_lvalue_mut(*e.val, u, cb);
            )
        )
        return false;
    }
    bool visit_mir_lvalue(const ::MIR::LValue& lv, ValUsage u, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        return visit_mir_lvalue_mut( const_cast<::MIR::LValue&>(lv), u, [&](auto& v, auto u) { return cb(v,u); } );
    }

    bool visit_mir_lvalues_mut(::MIR::RValue& rval, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        bool rv = false;
        TU_MATCHA( (rval), (se),
        (Use,
            rv |= visit_mir_lvalue_mut(se, ValUsage::Read, cb);
            ),
        (Constant,
            ),
        (SizedArray,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Read, cb);
            ),
        (Borrow,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Borrow, cb);
            ),
        (Cast,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Read, cb);
            ),
        (BinOp,
            rv |= visit_mir_lvalue_mut(se.val_l, ValUsage::Read, cb);
            rv |= visit_mir_lvalue_mut(se.val_r, ValUsage::Read, cb);
            ),
        (UniOp,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Read, cb);
            ),
        (DstMeta,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Read, cb);
            ),
        (DstPtr,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Read, cb);
            ),
        (MakeDst,
            rv |= visit_mir_lvalue_mut(se.ptr_val, ValUsage::Read, cb);
            rv |= visit_mir_lvalue_mut(se.meta_val, ValUsage::Read, cb);
            ),
        (Tuple,
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, ValUsage::Read, cb);
            ),
        (Array,
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, ValUsage::Read, cb);
            ),
        (Variant,
            rv |= visit_mir_lvalue_mut(se.val, ValUsage::Read, cb);
            ),
        (Struct,
            for(auto& v : se.vals)
                rv |= visit_mir_lvalue_mut(v, ValUsage::Read, cb);
            )
        )
        return rv;
    }
    bool visit_mir_lvalues(const ::MIR::RValue& rval, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        return visit_mir_lvalues_mut(const_cast<::MIR::RValue&>(rval), [&](auto& lv, auto u){ return cb(lv, u); });
    }

    bool visit_mir_lvalues_mut(::MIR::Statement& stmt, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        bool rv = false;
        TU_MATCHA( (stmt), (e),
        (Assign,
            rv |= visit_mir_lvalues_mut(e.src, cb);
            rv |= visit_mir_lvalue_mut(e.dst, ValUsage::Write, cb);
            ),
        (Asm,
            for(auto& v : e.inputs)
                rv |= visit_mir_lvalue_mut(v.second, ValUsage::Read, cb);
            for(auto& v : e.outputs)
                rv |= visit_mir_lvalue_mut(v.second, ValUsage::Write, cb);
            ),
        (SetDropFlag,
            ),
        (Drop,
            // Well, it mutates...
            rv |= visit_mir_lvalue_mut(e.slot, ValUsage::Write, cb);
            )
        )
        return rv;
    }
    bool visit_mir_lvalues(const ::MIR::Statement& stmt, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        return visit_mir_lvalues_mut(const_cast<::MIR::Statement&>(stmt), [&](auto& lv, auto im){ return cb(lv, im); });
    }

    void visit_mir_lvalues_mut(::MIR::Terminator& term, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        TU_MATCHA( (term), (e),
        (Incomplete,
            ),
        (Return,
            ),
        (Diverge,
            ),
        (Goto,
            ),
        (Panic,
            ),
        (If,
            visit_mir_lvalue_mut(e.cond, ValUsage::Read, cb);
            ),
        (Switch,
            visit_mir_lvalue_mut(e.val, ValUsage::Read, cb);
            ),
        (Call,
            if( e.fcn.is_Value() ) {
                visit_mir_lvalue_mut(e.fcn.as_Value(), ValUsage::Read, cb);
            }
            for(auto& v : e.args)
                visit_mir_lvalue_mut(v, ValUsage::Read, cb);
            visit_mir_lvalue_mut(e.ret_val, ValUsage::Write, cb);
            )
        )
    }

    void visit_mir_lvalues_mut(::MIR::TypeResolve& state, ::MIR::Function& fcn, ::std::function<bool(::MIR::LValue& , ValUsage)> cb)
    {
        for(unsigned int block_idx = 0; block_idx < fcn.blocks.size(); block_idx ++)
        {
            auto& block = fcn.blocks[block_idx];
            for(auto& stmt : block.statements)
            {
                state.set_cur_stmt(block_idx, (&stmt - &block.statements.front()));
                visit_mir_lvalues_mut(stmt, cb);
            }
            if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;
            state.set_cur_stmt_term(block_idx);
            visit_mir_lvalues_mut(block.terminator, cb);
        }
    }
    void visit_mir_lvalues(::MIR::TypeResolve& state, const ::MIR::Function& fcn, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb)
    {
        visit_mir_lvalues_mut(state, const_cast<::MIR::Function&>(fcn), [&](auto& lv, auto im){ return cb(lv, im); });
    }

    struct ParamsSet {
        ::HIR::PathParams   impl_params;
        const ::HIR::PathParams*  fcn_params;
        const ::HIR::TypeRef*   self_ty;

        ParamsSet():
            fcn_params(nullptr),
            self_ty(nullptr)
        {}

        t_cb_generic get_cb(const Span& sp) const {
            return monomorphise_type_get_cb(sp, self_ty, &impl_params, fcn_params, nullptr);
        }
    };
    const ::MIR::Function* get_called_mir(const ::MIR::TypeResolve& state, const ::HIR::Path& path, ParamsSet& params)
    {
        TU_MATCHA( (path.m_data), (pe),
        (Generic,
            const auto& fcn = state.m_crate.get_function_by_path(state.sp, pe.m_path);
            if( fcn.m_code.m_mir )
            {
                params.fcn_params = &pe.m_params;
                return &*fcn.m_code.m_mir;
            }
            ),
        (UfcsKnown,
            TRACE_FUNCTION_F(path);
            
            // Obtain trait pointer (for default impl and to know what the item type is)
            const auto& trait_ref = state.m_resolve.m_crate.get_trait_by_path(state.sp, pe.trait.m_path);
            auto trait_vi_it = trait_ref.m_values.find(pe.item);
            MIR_ASSERT(state, trait_vi_it != trait_ref.m_values.end(), "Couldn't find item " << pe.item << " in trait " << pe.trait.m_path);
            const auto& trait_vi = trait_vi_it->second;
            MIR_ASSERT(state, trait_vi.is_Function(), "Item '" << pe.item << " in trait " << pe.trait.m_path << " isn't a function");
            const auto& ve = trait_vi.as_Function();

            bool bound_found = false;
            bool is_spec = false;
            ::std::vector<::HIR::TypeRef>    best_impl_params;
            const ::HIR::TraitImpl* best_impl = nullptr;
            state.m_resolve.find_impl(state.sp, pe.trait.m_path, pe.trait.m_params, *pe.type, [&](auto impl_ref, auto is_fuzz) {
                DEBUG("[get_called_mir] Found " << impl_ref);
                if( ! impl_ref.m_data.is_TraitImpl() ) {
                    MIR_ASSERT(state, best_impl == nullptr, "Generic impl and `impl` block collided");
                    bound_found = true;
                    return true;
                }
                const auto& impl_ref_e = impl_ref.m_data.as_TraitImpl();
                const auto& impl = *impl_ref_e.impl;
                MIR_ASSERT(state, impl.m_trait_args.m_types.size() == pe.trait.m_params.m_types.size(), "Trait parameter count mismatch " << impl.m_trait_args << " vs " << pe.trait.m_params);

                if( best_impl == nullptr || impl.more_specific_than(*best_impl) ) {
                    best_impl = &impl;

                    auto fit = impl.m_methods.find(pe.item);
                    if( fit == impl.m_methods.end() ) {
                        DEBUG("[get_called_mir] Method " << pe.item << " missing in impl " << pe.trait << " for " << *pe.type);
                        return false;
                    }
                    best_impl_params.clear();
                    for(unsigned int i = 0; i < impl_ref_e.params.size(); i ++)
                    {
                        if( impl_ref_e.params[i] )
                            best_impl_params.push_back( impl_ref_e.params[i]->clone() );
                        else if( ! impl_ref_e.params_ph[i].m_data.is_Generic() )
                            best_impl_params.push_back( impl_ref_e.params_ph[i].clone() );
                        else
                            MIR_BUG(state, "[get_called_mir] Parameter " << i << " unset");
                    }
                    is_spec = fit->second.is_specialisable;
                    return !is_spec;
                }
                return false;
                });
            
            if( bound_found ) {
                return nullptr;
            }
            MIR_ASSERT(state, best_impl, "Couldn't find an impl for " << path);
            if( is_spec )
            {
                DEBUG(path << " pointed to a specialisable impl, not inlining");
                return nullptr;
            }
            const auto& impl = *best_impl;

            params.self_ty = &*pe.type;
            params.fcn_params = &pe.params;
            auto fit = impl.m_methods.find(pe.item);
            if( fit != impl.m_methods.end() )
            {
                params.impl_params.m_types = mv$(best_impl_params);
                DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                if( fit->second.data.m_code.m_mir )
                    return &*fit->second.data.m_code.m_mir;
            }
            else
            {
                params.impl_params = pe.trait.m_params.clone();
                if( ve.m_code.m_mir )
                    return &*ve.m_code.m_mir;
            }
            return nullptr;
            ),
        (UfcsInherent,
            const ::HIR::TypeImpl* best_impl;
            state.m_resolve.m_crate.find_type_impls(*pe.type, [](const auto&x)->const auto& { return x; }, [&](const auto& impl) {
                DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                // TODO: Specialisation.
                auto fit = impl.m_methods.find(pe.item);
                if( fit != impl.m_methods.end() )
                {
                    best_impl = &impl;
                    return true;
                }
                return false;
                });
            MIR_ASSERT(state, best_impl, "Couldn't find an impl for " << path);
            auto fit = best_impl->m_methods.find(pe.item);
            MIR_ASSERT(state, fit != best_impl->m_methods.end(), "Couldn't find method in best inherent impl");
            if( fit->second.data.m_code.m_mir )
            {
                params.self_ty = &*pe.type;
                params.fcn_params = &pe.params;
                params.impl_params = pe.impl_params.clone();
                return &*fit->second.data.m_code.m_mir;
            }
            return nullptr;
            ),
        (UfcsUnknown,
            MIR_BUG(state, "UfcsUnknown hit - " << path);
            )
        )
        return nullptr;
    }
}

bool MIR_Optimise_BlockSimplify(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_Inlining(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_PropagateSingleAssignments(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_UnifyTemporaries(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_UnifyBlocks(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_ConstPropagte(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_GarbageCollect_Partial(::MIR::TypeResolve& state, ::MIR::Function& fcn);
bool MIR_Optimise_GarbageCollect(::MIR::TypeResolve& state, ::MIR::Function& fcn);

void MIR_Optimise(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    static Span sp;
    TRACE_FUNCTION_F(path);
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };


    bool change_happened;
    unsigned int pass_num = 0;
    do
    {
        change_happened = false;
        TRACE_FUNCTION_FR("Pass " << pass_num, change_happened);

        // >> Simplify call graph
        MIR_Optimise_BlockSimplify(state, fcn);

        // >> Apply known constants
        change_happened |= MIR_Optimise_ConstPropagte(state, fcn);

        // >> Inline short functions
        bool inline_happened = MIR_Optimise_Inlining(state, fcn);
        if( inline_happened )
        {
            // Apply cleanup again (as monomorpisation in inlining may have exposed a vtable call)
            MIR_Cleanup(resolve, path, fcn, args, ret_type);
            //MIR_Dump_Fcn(::std::cout, fcn);
            change_happened = true;
        }

        // TODO: Convert `&mut *mut_foo` into `mut_foo` if the source is movable and not used afterwards

        // >> Propagate/remove dead assignments
        while( MIR_Optimise_PropagateSingleAssignments(state, fcn) )
            change_happened = true;

        // >> Unify duplicate temporaries
        // If two temporaries don't overlap in lifetime (blocks in which they're valid), unify the two
        change_happened |= MIR_Optimise_UnifyTemporaries(state, fcn);

        // >> Combine Duplicate Blocks
        change_happened |= MIR_Optimise_UnifyBlocks(state, fcn);
        #if 0
        if( change_happened )
        {
            //MIR_Dump_Fcn(::std::cout, fcn);
            MIR_Validate(resolve, path, fcn, args, ret_type);
        }
        #endif

        MIR_Optimise_GarbageCollect_Partial(state, fcn);
        pass_num += 1;
    } while( change_happened );


    #if 1
    if( debug_enabled() ) {
        MIR_Dump_Fcn(::std::cout, fcn);
    }
    #endif
    // DEFENCE: Run validation _before_ GC (so validation errors refer to the pre-gc numbers)
    MIR_Validate(resolve, path, fcn, args, ret_type);
    // GC pass on blocks and variables
    // - Find unused blocks, then delete and rewrite all references.
    MIR_Optimise_GarbageCollect(state, fcn);
}

// --------------------------------------------------------------------
// Performs basic simplications on the call graph (merging/removing blocks)
// --------------------------------------------------------------------
bool MIR_Optimise_BlockSimplify(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    // >> Replace targets that point to a block that is just a goto
    for(auto& block : fcn.blocks)
    {
        TU_MATCHA( (block.terminator), (e),
        (Incomplete,
            ),
        (Return,
            ),
        (Diverge,
            ),
        (Goto,
            if( &fcn.blocks[e] != &block )
                e = get_new_target(state, e);
            ),
        (Panic,
            ),
        (If,
            e.bb0 = get_new_target(state, e.bb0);
            e.bb1 = get_new_target(state, e.bb1);
            ),
        (Switch,
            for(auto& target : e.targets)
                target = get_new_target(state, target);
            ),
        (Call,
            e.ret_block = get_new_target(state, e.ret_block);
            e.panic_block = get_new_target(state, e.panic_block);
            )
        )
    }

    // >> Merge blocks where a block goto-s to a single-use block.
    {
        ::std::vector<bool> visited( fcn.blocks.size() );
        ::std::vector<unsigned int> uses( fcn.blocks.size() );
        ::std::vector< ::MIR::BasicBlockId> to_visit;
        to_visit.push_back( 0 );
        uses[0] ++;
        while( to_visit.size() > 0 )
        {
            auto bb = to_visit.back(); to_visit.pop_back();
            if( visited[bb] )
                continue ;
            visited[bb] = true;
            const auto& block = fcn.blocks[bb];

            TU_MATCHA( (block.terminator), (e),
            (Incomplete,
                ),
            (Return,
                ),
            (Diverge,
                ),
            (Goto,
                if( !visited[e] )   to_visit.push_back(e);
                uses[e] ++;
                ),
            (Panic,
                ),
            (If,
                if( !visited[e.bb0] )   to_visit.push_back(e.bb0);
                if( !visited[e.bb1] )   to_visit.push_back(e.bb1);
                uses[e.bb0] ++;
                uses[e.bb1] ++;
                ),
            (Switch,
                for(auto& target : e.targets)
                {
                    if( !visited[target] )
                        to_visit.push_back(target);
                    uses[target] ++;
                }
                ),
            (Call,
                if( !visited[e.ret_block] )     to_visit.push_back(e.ret_block);
                if( !visited[e.panic_block] )   to_visit.push_back(e.panic_block);
                uses[e.ret_block] ++;
                uses[e.panic_block] ++;
                )
            )
        }

        unsigned int i = 0;
        for(auto& block : fcn.blocks)
        {
            if( !visited[i] )
            {
                i++;
                continue ;
            }
            while( block.terminator.is_Goto() )
            {
                auto tgt = block.terminator.as_Goto();
                if( uses[tgt] != 1 )
                    break ;
                if( tgt == i )
                    break;
                DEBUG("Append bb " << tgt << " to bb" << i);

                assert( &fcn.blocks[tgt] != &block );
                // Move contents of source block, then set the TAGDEAD terminator to Incomplete
                auto src_block = mv$(fcn.blocks[tgt]);
                fcn.blocks[tgt].terminator = ::MIR::Terminator::make_Incomplete({});

                for(auto& stmt : src_block.statements)
                    block.statements.push_back( mv$(stmt) );
                block.terminator = mv$( src_block.terminator );
            }
            i ++;
        }
    }

    // NOTE: Not strictly true, but these can't trigger other optimisations
    return false;
}


// --------------------------------------------------------------------
// If two temporaries don't overlap in lifetime (blocks in which they're valid), unify the two
// --------------------------------------------------------------------
bool MIR_Optimise_Inlining(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    TRACE_FUNCTION;
    
    struct H
    {
        static bool can_inline(const ::HIR::Path& path, const ::MIR::Function& fcn)
        {
            // TODO: Allow functions that are just a switch on an input.
            if( fcn.blocks.size() == 1 )
            {
                return fcn.blocks[0].statements.size() < 5 && ! fcn.blocks[0].terminator.is_Goto();
            }
            else if( fcn.blocks.size() == 3 && fcn.blocks[0].terminator.is_Call() )
            {
                const auto& blk0_te = fcn.blocks[0].terminator.as_Call();
                if( !(fcn.blocks[1].terminator.is_Diverge() || fcn.blocks[1].terminator.is_Return()) )
                    return false;
                if( !(fcn.blocks[2].terminator.is_Diverge() || fcn.blocks[2].terminator.is_Return()) )
                    return false;
                if( fcn.blocks[0].statements.size() + fcn.blocks[1].statements.size() + fcn.blocks[2].statements.size() > 10 )
                    return false;
                // Detect and avoid simple recursion.
                // - This won't detect mutual recursion - that also needs prevention.
                if( blk0_te.fcn.is_Path() && blk0_te.fcn.as_Path() == path )
                    return false;
                return true;
            }
            else
            {
                return false;
            }
        }
    };
    struct Cloner
    {
        const Span& sp;
        const ::StaticTraitResolve& resolve;
        const ::MIR::Terminator::Data_Call& te;
        ParamsSet   params;
        unsigned int bb_base = ~0u;
        unsigned int tmp_base = ~0u;
        unsigned int var_base = ~0u;
        unsigned int df_base = ~0u;

        Cloner(const Span& sp, const ::StaticTraitResolve& resolve, ::MIR::Terminator::Data_Call& te):
            sp(sp),
            resolve(resolve),
            te(te)
        {}

        // TODO: Expand associated types
        ::HIR::TypeRef monomorph(const ::HIR::TypeRef& ty) const {
            auto rv = monomorphise_type_with(sp, ty, params.get_cb(sp));
            resolve.expand_associated_types(sp, rv);
            return rv;
        }
        ::HIR::GenericPath monomorph(const ::HIR::GenericPath& ty) const {
            auto rv = monomorphise_genericpath_with(sp, ty, params.get_cb(sp), false);
            for(auto& arg : rv.m_params.m_types)
                resolve.expand_associated_types(sp, arg);
            return rv;
        }
        ::HIR::Path monomorph(const ::HIR::Path& ty) const {
            auto rv = monomorphise_path_with(sp, ty, params.get_cb(sp), false);
            TU_MATCH(::HIR::Path::Data, (rv.m_data), (e2),
            (Generic,
                for(auto& arg : e2.m_params.m_types)
                    resolve.expand_associated_types(sp, arg);
                ),
            (UfcsInherent,
                resolve.expand_associated_types(sp, *e2.type);
                for(auto& arg : e2.params.m_types)
                    resolve.expand_associated_types(sp, arg);
                // TODO: impl params too?
                for(auto& arg : e2.impl_params.m_types)
                    resolve.expand_associated_types(sp, arg);
                ),
            (UfcsKnown,
                resolve.expand_associated_types(sp, *e2.type);
                for(auto& arg : e2.trait.m_params.m_types)
                    resolve.expand_associated_types(sp, arg);
                for(auto& arg : e2.params.m_types)
                    resolve.expand_associated_types(sp, arg);
                ),
            (UfcsUnknown,
                BUG(sp, "Encountered UfcsUnknown");
                )
            )
            return rv;
        }
        ::HIR::PathParams monomorph(const ::HIR::PathParams& ty) const {
            auto rv = monomorphise_path_params_with(sp, ty, params.get_cb(sp), false);
            for(auto& arg : rv.m_types)
                resolve.expand_associated_types(sp, arg);
            return rv;
        }

        ::MIR::BasicBlock clone_bb(const ::MIR::BasicBlock& src) const
        {
            ::MIR::BasicBlock   rv;
            rv.statements.reserve( src.statements.size() );
            for(const auto& stmt : src.statements)
            {
                TU_MATCHA( (stmt), (se),
                (Assign,
                    DEBUG(se.dst << " = " << se.src);
                    rv.statements.push_back( ::MIR::Statement::make_Assign({
                        this->clone_lval(se.dst),
                        this->clone_rval(se.src)
                        }) );
                    ),
                (Asm,
                    DEBUG("asm!");
                    rv.statements.push_back( ::MIR::Statement::make_Asm({
                        se.tpl,
                        this->clone_name_lval_vec(se.outputs),
                        this->clone_name_lval_vec(se.inputs),
                        se.clobbers,
                        se.flags
                        }) );
                    ),
                (SetDropFlag,
                    DEBUG("df" << se.idx << " = ");
                    rv.statements.push_back( ::MIR::Statement::make_SetDropFlag({
                        this->df_base + se.idx,
                        se.new_val,
                        se.other == ~0u ? ~0u : this->df_base + se.other
                        }) );
                    ),
                (Drop,
                    DEBUG("drop " << se.slot);
                    rv.statements.push_back( ::MIR::Statement::make_Drop({
                        se.kind,
                        this->clone_lval(se.slot),
                        se.flag_idx == ~0u ? ~0u : this->df_base + se.flag_idx
                        }) );
                    )
                )
            }
            DEBUG(src.terminator);
            rv.terminator = this->clone_term(src.terminator);
            return rv;
        }
        ::MIR::Terminator clone_term(const ::MIR::Terminator& src) const
        {
            TU_MATCHA( (src), (se),
            (Incomplete,
                return ::MIR::Terminator::make_Incomplete({});
                ),
            (Return,
                return ::MIR::Terminator::make_Goto(this->te.ret_block);
                ),
            (Diverge,
                return ::MIR::Terminator::make_Goto(this->te.panic_block);
                ),
            (Panic,
                return ::MIR::Terminator::make_Panic({});
                ),
            (Goto,
                return ::MIR::Terminator::make_Goto(se + this->bb_base);
                ),
            (If,
                return ::MIR::Terminator::make_If({
                    this->clone_lval(se.cond),
                    se.bb0 + this->bb_base,
                    se.bb1 + this->bb_base
                    });
                ),
            (Switch,
                ::std::vector<::MIR::BasicBlockId>  arms;
                arms.reserve(se.targets.size());
                for(const auto& bbi : se.targets)
                    arms.push_back( bbi + this->bb_base );
                return ::MIR::Terminator::make_Switch({ this->clone_lval(se.val), mv$(arms) });
                ),
            (Call,
                ::MIR::CallTarget   tgt;
                TU_MATCHA( (se.fcn), (ste),
                (Value,
                    tgt = ::MIR::CallTarget::make_Value( this->clone_lval(ste) );
                    ),
                (Path,
                    tgt = ::MIR::CallTarget::make_Path( this->monomorph(ste) );
                    ),
                (Intrinsic,
                    tgt = ::MIR::CallTarget::make_Intrinsic({ ste.name, this->monomorph(ste.params) });
                    )
                )
                return ::MIR::Terminator::make_Call({
                    this->bb_base + se.ret_block,
                    this->bb_base + se.panic_block,
                    this->clone_lval(se.ret_val),
                    mv$(tgt),
                    this->clone_lval_vec(se.args)
                    });
                )
            )
            throw "";
        }
        ::std::vector< ::std::pair<::std::string,::MIR::LValue> > clone_name_lval_vec(const ::std::vector< ::std::pair<::std::string,::MIR::LValue> >& src) const
        {
            ::std::vector< ::std::pair<::std::string,::MIR::LValue> >  rv;
            rv.reserve(src.size());
            for(const auto& e : src)
                rv.push_back(::std::make_pair(e.first, this->clone_lval(e.second)));
            return rv;
        }
        ::std::vector<::MIR::LValue> clone_lval_vec(const ::std::vector<::MIR::LValue>& src) const
        {
            ::std::vector<::MIR::LValue>    rv;
            rv.reserve(src.size());
            for(const auto& lv : src)
                rv.push_back( this->clone_lval(lv) );
            return rv;
        }
        ::MIR::LValue clone_lval(const ::MIR::LValue& src) const
        {
            TU_MATCHA( (src), (se),
            (Variable,
                return ::MIR::LValue::make_Variable(se + this->var_base);
                ),
            (Temporary,
                return ::MIR::LValue::make_Temporary({se.idx + this->tmp_base});
                ),
            (Argument,
                return this->te.args.at(se.idx).clone();
                ),
            (Return,
                return this->te.ret_val.clone();
                ),
            (Static,
                return this->monomorph( se );
                ),
            
            (Deref,
                return ::MIR::LValue::make_Deref({ box$(this->clone_lval(*se.val)) });
                ),
            (Field,
                return ::MIR::LValue::make_Field({ box$(this->clone_lval(*se.val)), se.field_index });
                ),
            (Index,
                return ::MIR::LValue::make_Index({
                    box$(this->clone_lval(*se.val)),
                    box$(this->clone_lval(*se.idx))
                    });
                ),
            (Downcast,
                return ::MIR::LValue::make_Downcast({ box$(this->clone_lval(*se.val)), se.variant_index });
                )
            )
            throw "";
        }
        ::MIR::RValue clone_rval(const ::MIR::RValue& src) const
        {
            TU_MATCHA( (src), (se),
            (Use,
                return ::MIR::RValue( this->clone_lval(se) );
                ),
            (Constant,
                TU_MATCHA( (se), (ce),
                (Int  , return ::MIR::Constant(ce);),
                (Uint , return ::MIR::Constant(ce);),
                (Float, return ::MIR::Constant(ce);),
                (Bool , return ::MIR::Constant(ce);),
                (Bytes, return ::MIR::Constant(ce);),
                (StaticString, return ::MIR::Constant(ce);),
                (Const,
                    return ::MIR::Constant::make_Const({ this->monomorph(ce.p) });
                    ),
                (ItemAddr,
                    return ::MIR::Constant::make_ItemAddr(this->monomorph(ce));
                    )
                )
                ),
            (SizedArray,
                return ::MIR::RValue::make_SizedArray({ this->clone_lval(se.val), se.count });
                ),
            (Borrow,
                // TODO: Region IDs
                return ::MIR::RValue::make_Borrow({ se.region, se.type, this->clone_lval(se.val) });
                ),
            (Cast,
                return ::MIR::RValue::make_Cast({ this->clone_lval(se.val), this->monomorph(se.type) });
                ),
            (BinOp,
                return ::MIR::RValue::make_BinOp({ this->clone_lval(se.val_l), se.op, this->clone_lval(se.val_r) });
                ),
            (UniOp,
                return ::MIR::RValue::make_UniOp({ this->clone_lval(se.val), se.op });
                ),
            (DstMeta,
                return ::MIR::RValue::make_DstMeta({ this->clone_lval(se.val) });
                ),
            (DstPtr,
                return ::MIR::RValue::make_DstPtr({ this->clone_lval(se.val) });
                ),
            (MakeDst,
                return ::MIR::RValue::make_MakeDst({ this->clone_lval(se.ptr_val), this->clone_lval(se.meta_val) });
                ),
            (Tuple,
                return ::MIR::RValue::make_Tuple({ this->clone_lval_vec(se.vals) });
                ),
            (Array,
                return ::MIR::RValue::make_Array({ this->clone_lval_vec(se.vals) });
                ),
            (Variant,
                return ::MIR::RValue::make_Variant({ this->monomorph(se.path), se.index, this->clone_lval(se.val) });
                ),
            (Struct,
                return ::MIR::RValue::make_Struct({ this->monomorph(se.path), se.variant_idx, this->clone_lval_vec(se.vals) });
                )
            )
            throw "";
        }
    };

    bool inline_happened = false;
    for(unsigned int i = 0; i < fcn.blocks.size(); i ++)
    {
        state.set_cur_stmt_term(i);
        if(auto* te = fcn.blocks[i].terminator.opt_Call())
        {
            if( ! te->fcn.is_Path() )
                continue ;
            const auto& path = te->fcn.as_Path();

            Cloner  cloner { state.sp, state.m_resolve, *te };
            const auto* called_mir = get_called_mir(state, path,  cloner.params);
            if( !called_mir )
                continue ;

            // Check the size of the target function.
            // Inline IF:
            // - First BB ends with a call and total count is 3
            // - Statement count smaller than 10
            if( ! H::can_inline(path, *called_mir) )
            {
                DEBUG("Can't inline " << path);
                continue ;
            }
            TRACE_FUNCTION_F("Inline " << path);

            // Monomorph values and append
            cloner.var_base = fcn.named_variables.size();
            for(const auto& ty : called_mir->named_variables)
                fcn.named_variables.push_back( cloner.monomorph(ty) );
            cloner.tmp_base = fcn.temporaries.size();
            for(const auto& ty : called_mir->temporaries)
                fcn.temporaries.push_back( cloner.monomorph(ty) );
            cloner.df_base = fcn.drop_flags.size();
            fcn.drop_flags.insert( fcn.drop_flags.end(), called_mir->drop_flags.begin(), called_mir->drop_flags.end() );
            cloner.bb_base = fcn.blocks.size();
            // Append monomorphised copy of all blocks.
            // > Arguments replaced by input lvalues
            ::std::vector<::MIR::BasicBlock>    new_blocks;
            new_blocks.reserve( called_mir->blocks.size() );
            for(const auto& bb : called_mir->blocks)
            {
                new_blocks.push_back( cloner.clone_bb(bb) );
            }

            // Apply
            fcn.blocks.reserve( fcn.blocks.size() + new_blocks.size() );
            for(auto& b : new_blocks)
            {
                fcn.blocks.push_back( mv$(b) );
            }
            fcn.blocks[i].terminator = ::MIR::Terminator::make_Goto( cloner.bb_base );
            inline_happened = true;
        }
    }
    return inline_happened;
}

// --------------------------------------------------------------------
// If two temporaries don't overlap in lifetime (blocks in which they're valid), unify the two
// --------------------------------------------------------------------
bool MIR_Optimise_UnifyTemporaries(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    TRACE_FUNCTION;
    ::std::vector<bool> replacable( fcn.temporaries.size() );
    // 1. Enumerate which (if any) temporaries share the same type
    {
        unsigned int n_found = 0;
        for(unsigned int tmpidx = 0; tmpidx < fcn.temporaries.size(); tmpidx ++)
        {
            if( replacable[tmpidx] )
                continue ;
            for(unsigned int i = tmpidx+1; i < fcn.temporaries.size(); i ++ )
            {
                if( replacable[i] )
                    continue ;
                if( fcn.temporaries[i] == fcn.temporaries[tmpidx] )
                {
                    replacable[i] = true;
                    replacable[tmpidx] = true;
                    n_found ++;
                }
            }
        }
        if( n_found == 0 )
            return false;
    }

    struct VarLifetime {
        ::std::vector<bool> blocks;

        VarLifetime(const ::MIR::Function& fcn):
            blocks(fcn.blocks.size())
        {
        }

        bool is_valid() const {
            for(auto v : blocks)
                if( v )
                    return true;
            return false;
        }
        bool overlaps(const VarLifetime& x) const {
            assert(blocks.size() == x.blocks.size());
            for(unsigned int i = 0; i < blocks.size(); i ++)
            {
                if( blocks[i] && x.blocks[i] )
                    return true;
            }
            return false;
        }
        void unify(const VarLifetime& x) {
            assert(blocks.size() == x.blocks.size());
            for(unsigned int i = 0; i < blocks.size(); i ++)
            {
                if( x.blocks[i] )
                    blocks[i] = true;
            }
        }
    };
    //::std::vector<VarLifetime>  var_lifetimes;
    ::std::vector<VarLifetime>  tmp_lifetimes( fcn.temporaries.size(), VarLifetime(fcn) );

    // 1. Calculate lifetimes of all variables/temporaries that are eligable to be merged
    // - Lifetime is from first write to last read. Borrows lead to the value being assumed to live forever
    // - > BUT: Since this is lazy, it's taken as only being the lifetime of non-Copy items (as determined by the drop call or a move)
    {
        auto mark_borrowed = [&](const ::MIR::LValue& lv) {
            if( const auto* ve = lv.opt_Temporary() ) {
                replacable[ve->idx] = false;
            }
            // TODO: Recurse!
            };

        struct State {
            //::std::vector<bool> vars;
            ::std::vector<bool> tmps;

            State() {}
            State(const ::MIR::Function& fcn):
                tmps(fcn.temporaries.size())
            {
            }

            bool merge(const State& other) {
                if( tmps.size() == 0 )
                {
                    assert(other.tmps.size() != 0);
                    tmps = other.tmps;
                    return true;
                }
                else
                {
                    assert(tmps.size() == other.tmps.size());
                    bool rv = false;
                    for(unsigned int i = 0; i < tmps.size(); i ++)
                    {
                        if( tmps[i] != other.tmps[i] && other.tmps[i] ) {
                            tmps[i] = true;
                            rv = true;
                        }
                    }
                    return rv;
                }
            }

            void mark_validity(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& lv, bool val) {
                if( const auto& ve = lv.opt_Temporary() ) {
                    tmps[ve->idx] = val;
                }
                else {
                }
            }
            void move_val(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& lv) {
                ::HIR::TypeRef  tmp;
                if( mir_res.m_resolve.type_is_copy( mir_res.sp, mir_res.get_lvalue_type(tmp, lv) ) ) {
                }
                else {
                    mark_validity(mir_res, lv, false);
                }
            }
        };
        ::std::vector<State>    block_states( fcn.blocks.size() );
        ::std::vector< ::std::pair<unsigned int, State> >   to_visit;
        auto add_to_visit = [&to_visit](unsigned int bb, State state) {
            to_visit.push_back( ::std::make_pair(bb, mv$(state)) );
            };
        to_visit.push_back( ::std::make_pair(0, State(fcn)) );
        while( !to_visit.empty() )
        {
            auto bb_idx = to_visit.back().first;
            auto val_state = mv$(to_visit.back().second);
            to_visit.pop_back();

            // 1. Merge with block state
            if( ! block_states[bb_idx].merge(val_state) )
                continue ;
            //DEBUG("BB" << bb_idx);

            // 2. Run block
            const auto& bb = fcn.blocks[bb_idx];
            for(unsigned int stmt_idx = 0; stmt_idx < bb.statements.size(); stmt_idx ++)
            {
                const auto& stmt = bb.statements[stmt_idx];
                state.set_cur_stmt(bb_idx, stmt_idx);

                switch( stmt.tag() )
                {
                case ::MIR::Statement::TAGDEAD:
                    throw "";
                case ::MIR::Statement::TAG_SetDropFlag:
                    break;
                case ::MIR::Statement::TAG_Drop:
                    val_state.mark_validity( state, stmt.as_Drop().slot, false );
                    break;
                case ::MIR::Statement::TAG_Asm:
                    for(const auto& v : stmt.as_Asm().outputs)
                        val_state.mark_validity( state, v.second, true );
                    break;
                case ::MIR::Statement::TAG_Assign:
                    // Check source (and invalidate sources)
                    TU_MATCH( ::MIR::RValue, (stmt.as_Assign().src), (se),
                    (Use,
                        val_state.move_val(state, se);
                        ),
                    (Constant,
                        ),
                    (SizedArray,
                        val_state.move_val(state, se.val);
                        ),
                    (Borrow,
                        mark_borrowed(se.val);
                        ),
                    (Cast,
                        ),
                    (BinOp,
                        ),
                    (UniOp,
                        ),
                    (DstMeta,
                        ),
                    (DstPtr,
                        ),
                    (MakeDst,
                        val_state.move_val(state, se.meta_val);
                        ),
                    (Tuple,
                        for(const auto& v : se.vals)
                            val_state.move_val(state, v);
                        ),
                    (Array,
                        for(const auto& v : se.vals)
                            val_state.move_val(state, v);
                        ),
                    (Variant,
                        val_state.move_val(state, se.val);
                        ),
                    (Struct,
                        for(const auto& v : se.vals)
                            val_state.move_val(state, v);
                        )
                    )
                    // Mark destination as valid
                    val_state.mark_validity( state, stmt.as_Assign().dst, true );
                    break;
                }
                block_states[bb_idx].merge(val_state);
            }

            // 3. During terminator, merge again
            state.set_cur_stmt_term(bb_idx);
            //DEBUG("- " << bb.terminator);
            TU_MATCH(::MIR::Terminator, (bb.terminator), (e),
            (Incomplete,
                // Should be impossible here.
                ),
            (Return,
                block_states[bb_idx].merge(val_state);
                ),
            (Diverge,
                ),
            (Goto,
                block_states[bb_idx].merge(val_state);
                // Push block with the new state
                add_to_visit( e, mv$(val_state) );
                ),
            (Panic,
                // What should be done here?
                ),
            (If,
                // Push blocks
                block_states[bb_idx].merge(val_state);
                add_to_visit( e.bb0, val_state );
                add_to_visit( e.bb1, mv$(val_state) );
                ),
            (Switch,
                block_states[bb_idx].merge(val_state);
                for(const auto& tgt : e.targets)
                {
                    add_to_visit( tgt, val_state );
                }
                ),
            (Call,
                for(const auto& arg : e.args)
                    val_state.move_val( state, arg );
                block_states[bb_idx].merge(val_state);
                // Push blocks (with return valid only in one)
                add_to_visit(e.panic_block, val_state);

                // TODO: If the function returns !, don't follow the ret_block
                val_state.mark_validity( state, e.ret_val, true );
                add_to_visit(e.ret_block, mv$(val_state));
                )
            )
        }

        // Convert block states into temp states
        for(unsigned int bb_idx = 0; bb_idx < block_states.size(); bb_idx ++)
        {
            for(unsigned int tmp_idx = 0; tmp_idx < block_states[bb_idx].tmps.size(); tmp_idx ++)
            {
                tmp_lifetimes[tmp_idx].blocks[bb_idx] = block_states[bb_idx].tmps[tmp_idx];
            }
        }
    }

    // 2. Unify variables of the same type with distinct non-overlapping lifetimes
    ::std::map<unsigned int, unsigned int> replacements;
    ::std::vector<bool> visited( fcn.temporaries.size() );
    bool replacement_needed = false;
    for(unsigned int tmpidx = 0; tmpidx < fcn.temporaries.size(); tmpidx ++)
    {
        if( ! replacable[tmpidx] )  continue ;
        if( visited[tmpidx] )   continue ;
        if( ! tmp_lifetimes[tmpidx].is_valid() )  continue ;
        visited[tmpidx] = true;

        for(unsigned int i = tmpidx+1; i < fcn.temporaries.size(); i ++)
        {
            if( !replacable[i] )
                continue ;
            if( fcn.temporaries[i] != fcn.temporaries[tmpidx] )
                continue ;
            if( ! tmp_lifetimes[i].is_valid() )  continue ;
            // Variables are of the same type, check if they overlap
            if( tmp_lifetimes[tmpidx].overlaps( tmp_lifetimes[i] ) )
                continue ;
            // They overlap, unify
            tmp_lifetimes[tmpidx].unify( tmp_lifetimes[i] );
            replacements[i] = tmpidx;
            replacement_needed = true;
            visited[i] = true;
        }
    }

    if( replacement_needed )
    {
        DEBUG("Replacing temporaries using {" << replacements << "}");
        visit_mir_lvalues_mut(state, fcn, [&](auto& lv, auto ) {
            if( auto* ve = lv.opt_Temporary() ) {
                auto it = replacements.find(ve->idx);
                if( it != replacements.end() )
                {
                    MIR_DEBUG(state, lv << " => Temporary(" << it->second << ")");
                    ve->idx = it->second;
                    return true;
                }
            }
            return false;
            });
    }

    return replacement_needed;
}

// --------------------------------------------------------------------
// --------------------------------------------------------------------
bool MIR_Optimise_UnifyBlocks(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    TRACE_FUNCTION_F("");
    struct H {
        static bool blocks_equal(const ::MIR::BasicBlock& a, const ::MIR::BasicBlock& b) {
            if( a.statements.size() != b.statements.size() )
                return false;
            for(unsigned int i = 0; i < a.statements.size(); i ++)
            {
                if( a.statements[i].tag() != b.statements[i].tag() )
                    return false;
                TU_MATCHA( (a.statements[i], b.statements[i]), (ae, be),
                (Assign,
                    if( ae.dst != be.dst )
                        return false;
                    if( ae.src != be.src )
                        return false;
                    ),
                (Asm,
                    if( ae.tpl != be.tpl )
                        return false;
                    if( ae.outputs != be.outputs )
                        return false;
                    if( ae.inputs != be.inputs )
                        return false;
                    if( ae.clobbers != be.clobbers )
                        return false;
                    if( ae.flags != be.flags )
                        return false;
                    ),
                (SetDropFlag,
                    if( ae.idx != be.idx )
                        return false;
                    if( ae.new_val != be.new_val )
                        return false;
                    if( ae.other != be.other )
                        return false;
                    ),
                (Drop,
                    if( ae.kind != be.kind )
                        return false;
                    if( ae.flag_idx != be.flag_idx )
                        return false;
                    if( ae.slot != be.slot )
                        return false;
                    )
                )
            }
            if( a.terminator.tag() != b.terminator.tag() )
                return false;
            TU_MATCHA( (a.terminator, b.terminator), (ae, be),
            (Incomplete,
                ),
            (Return,
                ),
            (Diverge,
                ),
            (Goto,
                if( ae != be )
                    return false;
                ),
            (Panic,
                if( ae.dst != be.dst )
                    return false;
                ),
            (If,
                if( ae.cond != be.cond )
                    return false;
                if( ae.bb0 != be.bb0 )
                    return false;
                if( ae.bb1 != be.bb1 )
                    return false;
                ),
            (Switch,
                if( ae.val != be.val )
                    return false;
                if( ae.targets != be.targets )
                    return false;
                ),
            (Call,
                if( ae.ret_block != be.ret_block )
                    return false;
                if( ae.panic_block != be.panic_block )
                    return false;
                if( ae.ret_val != be.ret_val )
                    return false;
                if( ae.args != be.args )
                    return false;

                if( ae.fcn.tag() != be.fcn.tag() )
                    return false;
                TU_MATCHA( (ae.fcn, be.fcn), (af, bf),
                (Value,
                    if( af != bf )
                        return false;
                    ),
                (Path,
                    if( af != bf )
                        return false;
                    ),
                (Intrinsic,
                    if( af.name != bf.name )
                        return false;
                    if( af.params != bf.params )
                        return false;
                    )
                )
                )
            )
            return true;
        }
    };
    // Locate duplicate blocks and replace
    ::std::vector<bool> visited( fcn.blocks.size() );
    ::std::map<unsigned int, unsigned int>  replacements;
    for(unsigned int bb_idx = 0; bb_idx < fcn.blocks.size(); bb_idx ++)
    {
        if( fcn.blocks[bb_idx].terminator.tag() == ::MIR::Terminator::TAGDEAD )
            continue ;
        if( fcn.blocks[bb_idx].terminator.is_Incomplete() && fcn.blocks[bb_idx].statements.size() == 0 )
            continue ;
        if( visited[bb_idx] )
            continue ;
        for(unsigned int i = bb_idx+1; i < fcn.blocks.size(); i ++)
        {
            if( visited[i] )
                continue ;
            if( H::blocks_equal(fcn.blocks[bb_idx], fcn.blocks[i]) ) {
                replacements[i] = bb_idx;
                visited[i] = true;
            }
        }
    }

    if( ! replacements.empty() )
    {
        //MIR_TODO(state, "Unify blocks - " << replacements);
        DEBUG("Unify blocks - " << replacements);
        auto patch_tgt = [&replacements](::MIR::BasicBlockId& tgt) {
            auto it = replacements.find(tgt);
            if( it != replacements.end() )
            {
                //DEBUG("BB" << tgt << " => BB" << it->second);
                tgt = it->second;
            }
            };
        for(auto& bb : fcn.blocks)
        {
            if( bb.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;
            TU_MATCHA( (bb.terminator), (te),
            (Incomplete,
                ),
            (Return,
                ),
            (Diverge,
                ),
            (Goto,
                patch_tgt(te);
                ),
            (Panic,
                patch_tgt(te.dst);
                ),
            (If,
                patch_tgt(te.bb0);
                patch_tgt(te.bb1);
                ),
            (Switch,
                for(auto& tgt : te.targets)
                    patch_tgt(tgt);
                ),
            (Call,
                patch_tgt(te.ret_block);
                patch_tgt(te.panic_block);
                )
            )
            //DEBUG("- " << bb.terminator);
        }

        for(const auto& r : replacements)
        {
            fcn.blocks[r.first] = ::MIR::BasicBlock {};
            //auto _ = mv$(fcn.blocks[r.first].terminator);
        }

        return true;
    }
    else
    {
        return false;
    }
}


// --------------------------------------------------------------------
// Propagate constants and eliminate known paths
// --------------------------------------------------------------------
bool MIR_Optimise_ConstPropagte(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool changed = false;
    TRACE_FUNCTION_FR("", changed);

    // - Remove calls to `size_of` and `align_of` (replace with value if known)
    for(auto& bb : fcn.blocks)
    {
        if( !bb.terminator.is_Call() )
            continue ;
        auto& te = bb.terminator.as_Call();
        if( !te.fcn.is_Intrinsic() )
            continue ;
        const auto& tef = te.fcn.as_Intrinsic();
        if( tef.name == "size_of" )
        {
            //size_t size_val = 0;
            //if( Target_GetSizeOf(tef.params.m_types.at(0), size_val) )
            //{
            //    bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(te.ret_val), ::MIR::Constant::make_Uint(size_val) }));
            //    bb.terminator = ::MIR::Terminator::make_Goto(te.ret_block);
            //    changed = true;
            //}
        }
        else if( tef.name == "align_of" )
        {
            //size_t size_val = 0;
            //if( Target_GetAlignOf(tef.params.m_types.at(0), size_val) )
            //{
            //    bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(te.ret_val), ::MIR::Constant::make_Uint(size_val) }));
            //    bb.terminator = ::MIR::Terminator::make_Goto(te.ret_block);
            //    changed = true;
            //}
        }
        // NOTE: Quick special-case for bswap<u8> (a no-op)
        else if( tef.name == "bswap" && tef.params.m_types.at(0) == ::HIR::CoreType::U8 )
        {
            DEBUG("bswap<u8> is a no-op");
            bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(te.ret_val), mv$(te.args.at(0)) }));
            bb.terminator = ::MIR::Terminator::make_Goto(te.ret_block);
            changed = true;
        }
        else
        {
            // Ignore any other intrinsics
        }
    }

    // - Propage constants within BBs
    //  > Evaluate BinOp with known values
    //  > Understand intrinsics like overflowing_* (with correct semantics)
    //   > NOTE: No need to locally stitch blocks, next pass will do that
    // TODO: Use ValState to do full constant propagation across blocks

    // 1. Remove based on known booleans within a single block
    // - Eliminates `if false`/`if true` branches
    for(auto& bb : fcn.blocks)
    {
        auto bbidx = &bb - &fcn.blocks.front();
        if( ! bb.terminator.is_If() )   continue;
        const auto& te = bb.terminator.as_If();

        // Restrict condition to being a temporary/variable
        if( te.cond.is_Temporary() )
            ;
        else if( te.cond.is_Argument() )
            ;
        else
            continue;

        auto has_cond = [&](const auto& lv, auto ut)->bool {
            return lv == te.cond;
            };
        bool val_known = false;
        bool known_val;
        for(unsigned int i = bb.statements.size(); i --; )
        {
            if( bb.statements[i].is_Assign() )
            {
                const auto& se = bb.statements[i].as_Assign();
                // If the condition was mentioned, don't assume it has the same value
                // TODO: What if the condition is a field/index and something else is edited?
                if( visit_mir_lvalues(se.src, has_cond) )
                    break;

                if( se.dst != te.cond )
                    continue;
                if( !se.src.is_Constant() )
                    continue;
                if( !se.src.as_Constant().is_Bool() )
                    continue;
                val_known = true;
                known_val = se.src.as_Constant().as_Bool();
                break;
            }
            else
            {
                if( visit_mir_lvalues(bb.statements[i], has_cond) )
                    break;
            }
        }
        if( val_known )
        {
            DEBUG("bb" << bbidx << ": Condition known to be " << known_val);
            bb.terminator = ::MIR::Terminator::make_Goto( known_val ? te.bb0 : te.bb1 );
            changed = true;
        }
    }

    return changed;
}

// --------------------------------------------------------------------
// Replace `tmp = RValue::Use()` where the temp is only used once
// --------------------------------------------------------------------
bool MIR_Optimise_PropagateSingleAssignments(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    bool replacement_happend;
    TRACE_FUNCTION_FR("", replacement_happend);

    // TODO: This requires kowing that doing so has no effect.
    // - Can use little heristics like a Call pointing to an assignment of its RV
    // - Count the read/write count of a variable, if it's 1,1 then this optimisation is correct.
    // - If the count is read=*,write=1 and the write is of an argument, replace with the argument.
    struct ValUse {
        unsigned int    read = 0;
        unsigned int    write = 0;
        unsigned int    borrow = 0;
    };
    struct {
        ::std::vector<ValUse> var_uses;
        ::std::vector<ValUse> tmp_uses;

        void use_lvalue(const ::MIR::LValue& lv, ValUsage ut) {
            TU_MATCHA( (lv), (e),
            (Variable,
                auto& vu = var_uses[e];
                switch(ut)
                {
                case ValUsage::Read:    vu.read += 1;   break;
                case ValUsage::Write:   vu.write += 1;  break;
                case ValUsage::Borrow:  vu.borrow += 1; break;
                }
                ),
            (Argument,
                ),
            (Temporary,
                auto& vu = tmp_uses[e.idx];
                switch(ut)
                {
                case ValUsage::Read:    vu.read += 1;   break;
                case ValUsage::Write:   vu.write += 1;  break;
                case ValUsage::Borrow:  vu.borrow += 1; break;
                }
                ),
            (Static,
                ),
            (Return,
                ),
            (Field,
                use_lvalue(*e.val, ut);
                ),
            (Deref,
                use_lvalue(*e.val, ut);
                ),
            (Index,
                use_lvalue(*e.val, ut);
                use_lvalue(*e.idx, ValUsage::Read);
                ),
            (Downcast,
                use_lvalue(*e.val, ut);
                )
            )
        }
    } val_uses = {
        ::std::vector<ValUse>(fcn.named_variables.size()),
        ::std::vector<ValUse>(fcn.temporaries.size())
        };
    visit_mir_lvalues(state, fcn, [&](const auto& lv, auto ut){ val_uses.use_lvalue(lv, ut); return false; });

    // --- Eliminate `tmp = Use(...)` (moves lvalues downwards)
    // > Find an assignment `tmp = Use(...)` where the temporary is only written and read once
    // > Locate the usage of this temporary
    //  - Stop on any conditional terminator
    // > Any lvalues in the source lvalue must not be mutated between the source assignment and the usage.
    //  - This includes mutation, borrowing, or moving.
    // > Replace usage with the inner of the original `Use`
    {
        // 1. Assignments (forward propagate)
        ::std::map< ::MIR::LValue, ::MIR::RValue>    replacements;
        for(const auto& block : fcn.blocks)
        {
            if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;

            for(unsigned int stmt_idx = 0; stmt_idx < block.statements.size(); stmt_idx ++)
            {
                const auto& stmt = block.statements[stmt_idx];
                // > Assignment
                if( ! stmt.is_Assign() )
                    continue ;
                const auto& e = stmt.as_Assign();
                // > Of a temporary from with a RValue::Use
                if( const auto* de = e.dst.opt_Temporary() )
                {
                    const auto& vu = val_uses.tmp_uses[de->idx];
                    DEBUG(e.dst << " - VU " << e.dst << " R:" << vu.read << " W:" << vu.write);
                    // TODO: Allow write many?
                    // > Where the temporary is written once and read once
                    if( !( vu.read == 1 && vu.write == 1 && vu.borrow == 0 ) )
                        continue ;
                }
                else if( const auto* de = e.dst.opt_Variable() )
                {
                    const auto& vu = val_uses.var_uses[*de];
                    DEBUG(e.dst << " - VU " << e.dst << " R:" << vu.read << " W:" << vu.write);
                    // TODO: Allow write many?
                    // > Where the variable is written once and read once
                    if( !( vu.read == 1 && vu.write == 1 && vu.borrow == 0 ) )
                        continue ;
                }
                else
                {
                    continue ;
                }
                DEBUG(e.dst << " = " << e.src);
                if( e.src.is_Use() )
                {
                    // Keep the complexity down
                    const auto* srcp = &e.src.as_Use();
                    while( srcp->is_Field() )
                        srcp = &*srcp->as_Field().val;
                    if( !( srcp->is_Temporary() || srcp->is_Variable() || srcp->is_Argument() ) )
                        continue ;
                }
                // TODO: Allow any rvalue, but that currently breaks due to chaining
                //else if( e.src.is_Borrow() )
                //{
                //}
                else
                {
                    continue ;
                }
                bool src_is_lvalue = e.src.is_Use();

                auto is_lvalue_usage = [&](const auto& lv, auto ){ return lv == e.dst; };

                // Returns `true` if the passed lvalue is used as a part of the source
                auto is_lvalue_in_val = [&](const auto& lv) {
                    return visit_mir_lvalues(e.src, [&](const auto& slv, auto ) { return lv == slv; });
                    };
                // Eligable for replacement
                // Find where this value is used
                // - Stop on a conditional block terminator
                // - Stop if any value mentioned in the source is mutated/invalidated
                bool stop = false;
                bool found = false;
                for(unsigned int si2 = stmt_idx+1; si2 < block.statements.size(); si2 ++)
                {
                    const auto& stmt2 = block.statements[si2];

                    // Usage found.
                    if( visit_mir_lvalues(stmt2, is_lvalue_usage) )
                    {
                        // If the source isn't a Use, ensure that this is a Use
                        if( !src_is_lvalue )
                        {
                            if( stmt2.is_Assign() && stmt2.as_Assign().src.is_Use() ) {
                                // Good
                            }
                            else {
                                // Bad, this has to stay a temporary
                                stop = true;
                                break;
                            }
                        }
                        found = true;
                        stop = true;
                        break;
                    }

                    // Determine if source is mutated.
                    // > Assume that any mutating access of the root value counts (over-cautious)
                    if( visit_mir_lvalues(block.statements[si2], [&](const auto& lv, auto vu){ return /*vu == ValUsage::Write &&*/ is_lvalue_in_val(lv); }) )
                    {
                        stop = true;
                        break;
                    }
                }
                if( !stop )
                {
                    DEBUG(block.terminator);
                    TU_MATCHA( (block.terminator), (e),
                    (Incomplete,
                        ),
                    (Return,
                        ),
                    (Diverge,
                        ),
                    (Goto,
                        DEBUG("TODO: Chain");
                        ),
                    (Panic,
                        ),
                    (If,
                        if( src_is_lvalue && visit_mir_lvalue(e.cond, ValUsage::Read, is_lvalue_usage) )
                            found = true;
                        stop = true;
                        ),
                    (Switch,
                        if( src_is_lvalue && visit_mir_lvalue(e.val, ValUsage::Read, is_lvalue_usage) )
                            found = true;
                        stop = true;
                        ),
                    (Call,
                        if( e.fcn.is_Value() )
                            if( src_is_lvalue && visit_mir_lvalue(e.fcn.as_Value(), ValUsage::Read, is_lvalue_usage) )
                                found = true;
                        for(const auto& v : e.args)
                        {
                            if( src_is_lvalue && visit_mir_lvalue(v, ValUsage::Read, is_lvalue_usage) )
                                found = true;
                        }
                        stop = true;
                        )
                    )
                }
                // Schedule a replacement in a future pass
                if( found )
                {
                    DEBUG("> Replace " << e.dst << " with " << e.src.as_Use());
                    replacements.insert( ::std::make_pair(e.dst.clone(), e.src.clone()) );
                }
                else
                {
                    DEBUG("- Single-write/read " << e.dst << " not replaced - couldn't find usage");
                }
            }   // for(stmt : block.statements)
        }

        for(;;)
        {
            unsigned int inner_replaced_count = 0;
            for(auto& r : replacements)
            {
                visit_mir_lvalues_mut(r.second, [&](auto& lv, auto vu) {
                    if( vu == ValUsage::Read )
                    {
                        auto it = replacements.find(lv);
                        if( it != replacements.end() && it->second.is_Use() )
                        {
                            lv = it->second.as_Use().clone();
                            inner_replaced_count ++;
                        }
                    }
                    return false;
                    });
            }
            if( inner_replaced_count == 0 )
                break;
        }

        // Apply replacements
        unsigned int replaced = 0;
        while( replaced < replacements.size() )
        {
            auto old_replaced = replaced;
            auto cb = [&](auto& lv, auto vu){
                if( vu == ValUsage::Read )
                {
                    auto it = replacements.find(lv);
                    if( it != replacements.end() )
                    {
                        MIR_ASSERT(state, it->second.tag() != ::MIR::RValue::TAGDEAD, "Replacement of  " << lv << " fired twice");
                        MIR_ASSERT(state, it->second.is_Use(), "Replacing a lvalue with a rvalue - " << lv << " with " << it->second);
                        auto rval = ::std::move(it->second);
                        lv = ::std::move(rval.as_Use());
                        replaced += 1;
                    }
                }
                return false;
                };
            for(unsigned int block_idx = 0; block_idx < fcn.blocks.size(); block_idx ++)
            {
                auto& block = fcn.blocks[block_idx];
                if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                    continue ;
                for(auto& stmt : block.statements)
                {
                    state.set_cur_stmt(block_idx, (&stmt - &block.statements.front()));
                    if( stmt.is_Assign() && stmt.as_Assign().src.is_Use() )
                    {
                        auto& e = stmt.as_Assign();
                        auto it = replacements.find(e.src.as_Use());
                        if( it != replacements.end() )
                        {
                            MIR_ASSERT(state, it->second.tag() != ::MIR::RValue::TAGDEAD, "Replacement of  " << it->first << " fired twice");
                            e.src = mv$(it->second);
                            replaced += 1;
                        }
                    }
                    else
                    {
                        visit_mir_lvalues_mut(stmt, cb);
                    }
                }
                state.set_cur_stmt_term(block_idx);
                visit_mir_lvalues_mut(block.terminator, cb);
            }
            MIR_ASSERT(state, replaced > old_replaced, "Temporary eliminations didn't advance");
        }
        // Remove assignments of replaced values
        for(auto& block : fcn.blocks)
        {
            for(auto it = block.statements.begin(); it != block.statements.end(); )
            {
                state.set_cur_stmt(&block - &fcn.blocks.front(), (it - block.statements.begin()));
                // If the statement was an assign of a replaced temporary, remove it.
                if( it->is_Assign() && replacements.count( it->as_Assign().dst ) > 0 )
                    it = block.statements.erase(it);
                else {
                    MIR_ASSERT(state, !( it->is_Assign() && it->as_Assign().src.tag() == ::MIR::RValue::TAGDEAD ), "");
                    ++it;
                }
            }
        }
        replacement_happend = (replaced > 0);
    }
    // --- Eliminate `... = Use(tmp)` (propagate lvalues upwards)
    {
        DEBUG("- Move upwards");
        for(auto& block : fcn.blocks)
        {
            for(auto it = block.statements.begin(); it != block.statements.end(); ++it)
            {
                state.set_cur_stmt(&block - &fcn.blocks.front(), it - block.statements.begin());
                if( !it->is_Assign() )
                    continue;
                if( it->as_Assign().src.tag() == ::MIR::RValue::TAGDEAD )
                    continue ;
                auto& to_replace_lval = it->as_Assign().dst;
                if( const auto* e = to_replace_lval.opt_Temporary() ) {
                    const auto& vu = val_uses.tmp_uses[e->idx];
                    if( !( vu.read == 1 && vu.write == 1 ) )
                        continue ;
                }
                else {
                    continue;
                }
                // ^^^  `tmp[1:1] = some_rvalue`

                // Find where it's used
                for(auto it2 = it+1; it2 != block.statements.end(); ++it2)
                {
                    if( !it2->is_Assign() )
                        continue ;
                    if( it2->as_Assign().src.tag() == ::MIR::RValue::TAGDEAD )
                        continue ;
                    if( !it2->as_Assign().src.is_Use() )
                        continue ;
                    if( it2->as_Assign().src.as_Use() != to_replace_lval )
                        continue ;
                    const auto& new_dst_lval = it2->as_Assign().dst;
                    // `... = Use(to_replace_lval)`

                    // Ensure that the target doesn't change in the intervening time.
                    bool was_invalidated = false;
                    for(auto it3 = it+1; it3 != it2; it3++)
                    {
                        // Closure returns `true` if the passed lvalue is a component of `new_dst_lval`
                        auto is_lvalue_in_val = [&](const auto& lv) {
                            return visit_mir_lvalue(new_dst_lval, ValUsage::Write, [&](const auto& slv, auto ) { return lv == slv; });
                            };
                        if( visit_mir_lvalues(*it3, [&](const auto& lv, auto ){ return is_lvalue_in_val(lv); }) )
                        {
                            was_invalidated = true;
                            break;
                        }
                    }

                    // Replacement is valid.
                    if( ! was_invalidated )
                    {
                        DEBUG(state << "Replace assignment of " << to_replace_lval << " with " << new_dst_lval);
                        it->as_Assign().dst = mv$(it2->as_Assign().dst);
                        block.statements.erase(it2);
                        replacement_happend = true;
                        break;
                    }
                }
            }
        }
    }

    // --- Function returns (reverse propagate)
    // > Find `tmp = <function call>` where the temporary is used 1:1
    // > Search the following block for `<anything> = Use(this_tmp)`
    // > Ensure that the target of the above assignment isn't used in the intervening statements
    // > Replace function call result value with target of assignment
    {
        DEBUG("- Returns");
        for(auto& block : fcn.blocks)
        {
            if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                continue ;

            // If the terminator is a call that writes to a 1:1 value, replace the destination value with the eventual destination (if that value isn't used in the meantime)
            if( block.terminator.is_Call() )
            {
                // TODO: What if the destination located here is a 1:1 and its usage is listed to be replaced by the return value.
                auto& e = block.terminator.as_Call();
                // TODO: Support variables too?
                if( !e.ret_val.is_Temporary() )
                    continue ;
                const auto& vu = val_uses.tmp_uses[e.ret_val.as_Temporary().idx];
                if( !( vu.read == 1 && vu.write == 1 ) )
                    continue ;

                // Iterate the target block, looking for where this value is used.
                const ::MIR::LValue* new_dst = nullptr;
                auto& blk2 = fcn.blocks.at(e.ret_block);
                for(const auto& stmt : blk2.statements)
                { 
                    // Find `RValue::Use( this_lvalue )`
                    if( stmt.is_Assign() && stmt.as_Assign().src.is_Use() && stmt.as_Assign().src.as_Use() == e.ret_val ) {
                        new_dst = &stmt.as_Assign().dst;
                        break;
                    }
                }

                // Ensure that the new destination value isn't used before assignment
                if( new_dst )
                {
                    auto lvalue_impacts_dst = [&](const ::MIR::LValue& lv) {
                        return visit_mir_lvalue(*new_dst, ValUsage::Write, [&](const auto& slv, auto ) { return lv == slv; });
                        };
                    for(auto it = blk2.statements.begin(); it != blk2.statements.end(); ++ it)
                    {
                        const auto& stmt = *it;
                        if( stmt.is_Assign() && stmt.as_Assign().src.is_Use() && stmt.as_Assign().src.as_Use() == e.ret_val )
                        {
                            DEBUG("- Replace function return " << e.ret_val << " with " << *new_dst);
                            e.ret_val = new_dst->clone();
                            it = blk2.statements.erase(it);
                            replacement_happend = true;
                            break;
                        }
                        if( visit_mir_lvalues(stmt, [&](const auto& lv, ValUsage vu){ return lv == *new_dst || (vu == ValUsage::Write && lvalue_impacts_dst(lv)); }) )
                        {
                            break;
                        }
                    }
                }
            }
        }
    }

    // Locate values that are written, but not read or borrowed
    // - Current implementation requires a single write (to avoid issues with drop)
    // - if T: Drop (or T: !Copy) then the write should become a drop
    {
        DEBUG("- Write-only");
        for(auto& block : fcn.blocks)
        {
            for(auto it = block.statements.begin(); it != block.statements.end(); ++it)
            {
                if( const auto& se = it->opt_Assign() )
                {
                    TU_MATCH_DEF( ::MIR::LValue, (se->dst), (de),
                    (
                        ),
                    (Variable,
                        const auto& vu = val_uses.var_uses[de];
                        if( vu.write == 1 && vu.read == 0 && vu.borrow == 0 ) {
                            DEBUG(se->dst << " only written, removing write");
                            it = block.statements.erase(it)-1;
                        }
                        ),
                    (Temporary,
                        const auto& vu = val_uses.tmp_uses[de.idx];
                        if( vu.write == 1 && vu.read == 0 && vu.borrow == 0 ) {
                            DEBUG(se->dst << " only written, removing write with " << se->src);
                            it = block.statements.erase(it)-1;
                        }
                        )
                    )
                }
            }
            // NOTE: Calls can write values, but they also have side-effects
        }
    }

    // TODO: Run special case replacements for when there's `tmp/var = arg` and `rv = tmp/var`

    return replacement_happend;
}


// --------------------------------------------------------------------
// Clear all unused blocks
// --------------------------------------------------------------------
bool MIR_Optimise_GarbageCollect_Partial(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    ::std::vector<bool> visited( fcn.blocks.size() );
    ::std::vector< ::MIR::BasicBlockId> to_visit;
    to_visit.push_back( 0 );
    while( to_visit.size() > 0 )
    {
        auto bb = to_visit.back(); to_visit.pop_back();
        if( visited[bb] )   continue;
        visited[bb] = true;
        const auto& block = fcn.blocks[bb];

        TU_MATCHA( (block.terminator), (e),
        (Incomplete,
            ),
        (Return,
            ),
        (Diverge,
            ),
        (Goto,
            if( !visited[e] )
                to_visit.push_back(e);
            ),
        (Panic,
            ),
        (If,
            if( !visited[e.bb0] )
                to_visit.push_back(e.bb0);
            if( !visited[e.bb1] )
                to_visit.push_back(e.bb1);
            ),
        (Switch,
            for(auto& target : e.targets)
                if( !visited[target] )
                    to_visit.push_back(target);
            ),
        (Call,
            if( !visited[e.ret_block] )
                to_visit.push_back(e.ret_block);
            if( !visited[e.panic_block] )
                to_visit.push_back(e.panic_block);
            )
        )
    }
    bool rv = false;
    for(unsigned int i = 0; i < visited.size(); i ++)
    {
        if( !visited[i] )
        {
            DEBUG("CLEAR bb" << i);
            fcn.blocks[i].statements.clear();
            fcn.blocks[i].terminator = ::MIR::Terminator::make_Incomplete({});
            rv = true;
        }
    }
    return rv;
}
// --------------------------------------------------------------------
// Remove all unused temporaries and blocks
// --------------------------------------------------------------------
bool MIR_Optimise_GarbageCollect(::MIR::TypeResolve& state, ::MIR::Function& fcn)
{
    ::std::vector<bool> used_temps( fcn.temporaries.size() );
    ::std::vector<bool> used_vars( fcn.named_variables.size() );
    ::std::vector<bool> used_dfs( fcn.drop_flags.size() );
    ::std::vector<bool> visited( fcn.blocks.size() );
    ::std::vector< ::MIR::BasicBlockId> to_visit;
    to_visit.push_back( 0 );
    while( to_visit.size() > 0 )
    {
        auto bb = to_visit.back(); to_visit.pop_back();
        visited[bb] = true;
        const auto& block = fcn.blocks[bb];

        auto assigned_lval = [&](const ::MIR::LValue& lv) {
            if(const auto* le = lv.opt_Temporary() )
                used_temps[le->idx] = true;
            if(const auto* le = lv.opt_Variable() )
                used_vars[*le] = true;
            };

        for(const auto& stmt : block.statements)
        {
            TU_IFLET( ::MIR::Statement, stmt, Assign, e,
                assigned_lval(e.dst);
            )
            else if( const auto* e = stmt.opt_Drop() )
            {
                if( e->flag_idx != ~0u )
                    used_dfs.at(e->flag_idx) = true;
            }
            else if( const auto* e = stmt.opt_Asm() )
            {
                for(const auto& val : e->outputs)
                    assigned_lval(val.second);
            }
            else if( const auto* e = stmt.opt_SetDropFlag() )
            {
                if( e->other != ~0u )
                    used_dfs.at(e->other) = true;
            }
        }

        TU_MATCHA( (block.terminator), (e),
        (Incomplete,
            ),
        (Return,
            ),
        (Diverge,
            ),
        (Goto,
            if( !visited[e] )
                to_visit.push_back(e);
            ),
        (Panic,
            ),
        (If,
            if( !visited[e.bb0] )
                to_visit.push_back(e.bb0);
            if( !visited[e.bb1] )
                to_visit.push_back(e.bb1);
            ),
        (Switch,
            for(auto& target : e.targets)
                if( !visited[target] )
                    to_visit.push_back(target);
            ),
        (Call,
            if( !visited[e.ret_block] )
                to_visit.push_back(e.ret_block);
            if( !visited[e.panic_block] )
                to_visit.push_back(e.panic_block);

            assigned_lval(e.ret_val);
            )
        )
    }

    ::std::vector<unsigned int> block_rewrite_table;
    for(unsigned int i = 0, j = 0; i < fcn.blocks.size(); i ++)
    {
        block_rewrite_table.push_back( visited[i] ? j ++ : ~0u );
    }
    ::std::vector<unsigned int> temp_rewrite_table;
    unsigned int n_temp = fcn.temporaries.size();
    for(unsigned int i = 0, j = 0; i < n_temp; i ++)
    {
        if( !used_temps[i] )
        {
            DEBUG("GC Temporary(" << i << ")");
            fcn.temporaries.erase(fcn.temporaries.begin() + j);
        }
        temp_rewrite_table.push_back( used_temps[i] ? j ++ : ~0u );
    }
    ::std::vector<unsigned int> var_rewrite_table;
    unsigned int n_var = fcn.named_variables.size();
    for(unsigned int i = 0, j = 0; i < n_var; i ++)
    {
        if( !used_vars[i] )
        {
            DEBUG("GC Variable(" << i << ")");
            fcn.named_variables.erase(fcn.named_variables.begin() + j);
        }
        var_rewrite_table.push_back( used_vars[i] ? j ++ : ~0u );
    }
    ::std::vector<unsigned int> df_rewrite_table;
    unsigned int n_df = fcn.drop_flags.size();
    for(unsigned int i = 0, j = 0; i < n_df; i ++)
    {
        if( !used_dfs[i] )
        {
            DEBUG("GC df" << i);
            fcn.drop_flags.erase(fcn.drop_flags.begin() + j);
        }
        df_rewrite_table.push_back( used_dfs[i] ? j ++ : ~0u );
    }

    auto it = fcn.blocks.begin();
    for(unsigned int i = 0; i < visited.size(); i ++)
    {
        if( !visited[i] )
        {
            // Delete
            DEBUG("GC bb" << i);
            it = fcn.blocks.erase(it);
        }
        else
        {
            auto lvalue_cb = [&](auto& lv, auto ) {
                if(auto* e = lv.opt_Temporary() ) {
                    MIR_ASSERT(state, e->idx < temp_rewrite_table.size(), "Temporary out of range - " << lv);
                    // If the table entry for this temporary is !0, it wasn't marked as used
                    MIR_ASSERT(state, temp_rewrite_table.at(e->idx) != ~0u, "LValue " << lv << " incorrectly marked as unused");
                    e->idx = temp_rewrite_table.at(e->idx);
                }
                if(auto* e = lv.opt_Variable() ) {
                    MIR_ASSERT(state, *e < var_rewrite_table.size(), "Variable out of range - " << lv);
                    // If the table entry for this variable is !0, it wasn't marked as used
                    MIR_ASSERT(state, var_rewrite_table.at(*e) != ~0u, "LValue " << lv << " incorrectly marked as unused");
                    *e = var_rewrite_table.at(*e);
                }
                return false;
                };
            for(auto stmt_it = it->statements.begin(); stmt_it != it->statements.end(); ++ stmt_it)
            {
                state.set_cur_stmt(i, stmt_it - it->statements.begin());
                visit_mir_lvalues_mut(*stmt_it, lvalue_cb);
                if( auto* se = stmt_it->opt_Drop() )
                {
                    // Rewrite drop flag indexes
                    if( se->flag_idx != ~0u )
                        se->flag_idx = df_rewrite_table[se->flag_idx];
                }
                else if( auto* se = stmt_it->opt_SetDropFlag() )
                {
                    // Rewrite drop flag indexes OR delete
                    if( df_rewrite_table[se->idx] == ~0u ) {
                        stmt_it = it->statements.erase(stmt_it)-1;
                        continue ;
                    }
                    se->idx = df_rewrite_table[se->idx];
                    if( se->other != ~0u )
                        se->other = df_rewrite_table[se->other];
                }
            }
            state.set_cur_stmt_term(i);
            // Rewrite and advance
            TU_MATCHA( (it->terminator), (e),
            (Incomplete,
                ),
            (Return,
                ),
            (Diverge,
                ),
            (Goto,
                e = block_rewrite_table[e];
                ),
            (Panic,
                ),
            (If,
                visit_mir_lvalue_mut(e.cond, ValUsage::Read, lvalue_cb);
                e.bb0 = block_rewrite_table[e.bb0];
                e.bb1 = block_rewrite_table[e.bb1];
                ),
            (Switch,
                visit_mir_lvalue_mut(e.val, ValUsage::Read, lvalue_cb);
                for(auto& target : e.targets)
                    target = block_rewrite_table[target];
                ),
            (Call,
                if( e.fcn.is_Value() ) {
                    visit_mir_lvalue_mut(e.fcn.as_Value(), ValUsage::Read, lvalue_cb);
                }
                for(auto& v : e.args)
                    visit_mir_lvalue_mut(v, ValUsage::Read, lvalue_cb);
                visit_mir_lvalue_mut(e.ret_val, ValUsage::Write, lvalue_cb);
                e.ret_block   = block_rewrite_table[e.ret_block];
                e.panic_block = block_rewrite_table[e.panic_block];
                )
            )

            ++it;
        }
    }

    // TODO: Detect if any optimisations happened, and return true in that case
    return false;
}

void MIR_OptimiseCrate(::HIR::Crate& crate)
{
    ::MIR::OuterVisitor ov { crate, [](const auto& res, const auto& p, auto& expr, const auto& args, const auto& ty)
        {
            MIR_Optimise(res, p, *expr.m_mir, args, ty);
        }
        };
    ov.visit_crate(crate);
}

