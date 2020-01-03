/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/monomorphise.hpp
 * - MIR monomorphisation
 */
#include "monomorphise.hpp"
#include "hir_typeck/static.hpp"
#include <mir/mir.hpp>
#include <hir/hir.hpp>
#include <mir/operations.hpp>   // Needed for post-monomorph checks and optimisations
#include <hir_conv/constant_evaluation.hpp>

namespace {
    ::MIR::LValue monomorph_LValue(const ::StaticTraitResolve& resolve, const Trans_Params& params, const ::MIR::LValue& tpl)
    {
        if( tpl.m_root.is_Static() )
        {
            return ::MIR::LValue( ::MIR::LValue::Storage::new_Static(params.monomorph(resolve, tpl.m_root.as_Static())), tpl.m_wrappers );
        }
        else
        {
            return tpl.clone();
        }
    }
    ::MIR::Constant monomorph_Constant(const ::StaticTraitResolve& resolve, const Trans_Params& params, const ::MIR::Constant& tpl)
    {
        TU_MATCHA( (tpl), (ce),
        (Int,
            return ::MIR::Constant::make_Int(ce);
            ),
        (Uint,
            return ::MIR::Constant::make_Uint(ce);
            ),
        (Float,
            return ::MIR::Constant::make_Float(ce);
            ),
        (Bool,
            return ::MIR::Constant::make_Bool(ce);
            ),
        (Bytes,
            return ::MIR::Constant(ce);
            ),
        (StaticString,
            return ::MIR::Constant(ce);
            ),
        (Const,
            return ::MIR::Constant::make_Const({
                box$(params.monomorph(resolve, *ce.p))
                });
            ),
        (Generic,
            TODO(Span(), "Monomorphise MIR generic constant");
            ),
        (ItemAddr,
            auto p = params.monomorph(resolve, *ce);
            // TODO: If this is a pointer to a function on a trait object, replace with the address loaded from the vtable.
            // - Requires creating a new temporary for the vtable pointer.
            // - Also requires knowing what the receiver is.
            return ::MIR::Constant( box$(p) );
            )
        )
        throw "";
    }
    ::MIR::Param monomorph_Param(const ::StaticTraitResolve& resolve, const Trans_Params& params, const ::MIR::Param& tpl)
    {
        TU_MATCHA( (tpl), (e),
        (LValue,
            return monomorph_LValue(resolve, params, e);
            ),
        (Constant,
            return monomorph_Constant(resolve, params, e);
            )
        )
        throw "";
    }
    //::std::vector<::MIR::LValue> monomorph_LValue_list(const ::StaticTraitResolve& resolve, const Trans_Params& params, const ::std::vector<::MIR::LValue>& tpl)
    //{
    //    ::std::vector<::MIR::LValue>    rv;
    //    rv.reserve( tpl.size() );
    //    for(const auto& v : tpl)
    //        rv.push_back( monomorph_LValue(resolve, params, v) );
    //    return rv;
    //}
    ::std::vector<::MIR::Param> monomorph_Param_list(const ::StaticTraitResolve& resolve, const Trans_Params& params, const ::std::vector<::MIR::Param>& tpl)
    {
        ::std::vector<::MIR::Param>    rv;
        rv.reserve( tpl.size() );
        for(const auto& v : tpl)
            rv.push_back( monomorph_Param(resolve, params, v) );
        return rv;
    }
}

::MIR::FunctionPointer Trans_Monomorphise(const ::StaticTraitResolve& resolve, const Trans_Params& params, const ::MIR::FunctionPointer& tpl)
{
    static Span sp;
    TRACE_FUNCTION;
    assert(tpl);

    ::MIR::Function output;

    // 1. Monomorphise locals and temporaries
    output.locals.reserve( tpl->locals.size() );
    for(const auto& var : tpl->locals)
    {
        DEBUG("- _" << output.locals.size());
        output.locals.push_back( params.monomorph(resolve, var) );
    }
    output.drop_flags = tpl->drop_flags;

    // 2. Monomorphise all paths
    output.blocks.reserve( tpl->blocks.size() );
    for(const auto& block : tpl->blocks)
    {
        ::std::vector< ::MIR::Statement>    statements;

        TRACE_FUNCTION_F("bb" << output.blocks.size());
        statements.reserve( block.statements.size() );
        for(const auto& stmt : block.statements)
        {
            switch( stmt.tag() )
            {
            case ::MIR::Statement::TAGDEAD: throw "";
            case ::MIR::Statement::TAG_SetDropFlag:
                statements.push_back( ::MIR::Statement( stmt.as_SetDropFlag() ) );
                break;
            case ::MIR::Statement::TAG_ScopeEnd:
                statements.push_back( ::MIR::Statement( stmt.as_ScopeEnd() ) );
                break;
            case ::MIR::Statement::TAG_Drop: {
                const auto& e = stmt.as_Drop();
                DEBUG("- DROP " << e.slot);
                statements.push_back( ::MIR::Statement::make_Drop({
                    e.kind,
                    monomorph_LValue(resolve, params, e.slot),
                    e.flag_idx
                    }) );
                } break;
            case ::MIR::Statement::TAG_Assign: {
                const auto& e = stmt.as_Assign();
                DEBUG("- " << e.dst << " = " << e.src);

                ::MIR::RValue   rval;
                TU_MATCHA( (e.src), (se),
                (Use,
                    rval = ::MIR::RValue( monomorph_LValue(resolve, params, se) );
                    ),
                (Constant,
                    rval = monomorph_Constant(resolve, params, se);
                    ),
                (SizedArray,
                    rval = ::MIR::RValue::make_SizedArray({
                        monomorph_Param(resolve, params, se.val),
                        se.count
                        });
                    ),
                (Borrow,
                    rval = ::MIR::RValue::make_Borrow({
                        se.region, se.type,
                        monomorph_LValue(resolve, params, se.val)
                        });
                    ),
                (Cast,
                    rval = ::MIR::RValue::make_Cast({
                        monomorph_LValue(resolve, params, se.val),
                        params.monomorph(resolve, se.type)
                        });
                    ),
                (BinOp,
                    rval = ::MIR::RValue::make_BinOp({
                        monomorph_Param(resolve, params, se.val_l),
                        se.op,
                        monomorph_Param(resolve, params, se.val_r)
                        });
                    ),
                (UniOp,
                    rval = ::MIR::RValue::make_UniOp({
                        monomorph_LValue(resolve, params, se.val),
                        se.op
                        });
                    ),
                (DstMeta,
                    auto lv = monomorph_LValue(resolve, params, se.val);
                    // TODO: Get the type of this, and if it's an array - replace with the size
                    rval = ::MIR::RValue::make_DstMeta({ mv$(lv) });
                    ),
                (DstPtr,
                    rval = ::MIR::RValue::make_DstPtr({ monomorph_LValue(resolve, params, se.val) });
                    ),
                (MakeDst,
                    rval = ::MIR::RValue::make_MakeDst({
                        monomorph_Param(resolve, params, se.ptr_val),
                        monomorph_Param(resolve, params, se.meta_val)
                        });
                    ),
                (Tuple,
                    rval = ::MIR::RValue::make_Tuple({
                        monomorph_Param_list(resolve, params, se.vals)
                        });
                    ),
                (Array,
                    rval = ::MIR::RValue::make_Array({
                        monomorph_Param_list(resolve, params, se.vals)
                        });
                    ),
                // Create a new instance of a union (and eventually enum)
                (Variant,
                    rval = ::MIR::RValue::make_Variant({
                        params.monomorph(resolve, se.path),
                        se.index,
                        monomorph_Param(resolve, params, se.val)
                        });
                    ),
                // Create a new instance of a struct (or enum)
                (Struct,
                    rval = ::MIR::RValue::make_Struct({
                        params.monomorph(resolve, se.path),
                        monomorph_Param_list(resolve, params, se.vals)
                        });
                    )
                )

                statements.push_back( ::MIR::Statement::make_Assign({
                    monomorph_LValue(resolve, params, e.dst),
                    mv$(rval)
                    }) );
                } break;
            case ::MIR::Statement::TAG_Asm: {
                const auto& e = stmt.as_Asm();
                DEBUG("- asm! \"" << e.tpl << "\"");
                ::std::vector< ::std::pair<::std::string, ::MIR::LValue>>   new_out, new_in;
                new_out.reserve( e.outputs.size() );
                for(auto& ent : e.outputs)
                    new_out.push_back(::std::make_pair( ent.first, monomorph_LValue(resolve, params, ent.second) ));
                new_in.reserve( e.inputs.size() );
                for(auto& ent : e.inputs)
                    new_in.push_back(::std::make_pair( ent.first, monomorph_LValue(resolve, params, ent.second) ));

                statements.push_back( ::MIR::Statement::make_Asm({
                    e.tpl, mv$(new_out), mv$(new_in), e.clobbers, e.flags
                    }) );
                } break;
            }
        }

        ::MIR::Terminator   terminator;

        DEBUG("> " << block.terminator);
        TU_MATCHA( (block.terminator), (e),
        (Incomplete,
            //BUG(sp, "Incomplete block");
            terminator = e;
            ),
        (Return,
            terminator = e;
            ),
        (Diverge,
            terminator = e;
            ),
        (Goto,
            terminator = e;
            ),
        (Panic,
            terminator = e;
            ),
        (If,
            terminator = ::MIR::Terminator::make_If({
                monomorph_LValue(resolve, params, e.cond),
                e.bb0, e.bb1
                });
            ),
        (Switch,
            terminator = ::MIR::Terminator::make_Switch({
                monomorph_LValue(resolve, params, e.val),
                e.targets
                });
            ),
        (SwitchValue,
            terminator = ::MIR::Terminator::make_SwitchValue({
                monomorph_LValue(resolve, params, e.val),
                e.def_target,
                e.targets,
                e.values.clone()
                });
            ),
        (Call,
            struct H {
                static ::MIR::CallTarget monomorph_calltarget(const ::StaticTraitResolve& resolve, const Trans_Params& params, const ::MIR::CallTarget& ct) {
                    TU_MATCHA( (ct), (e),
                    (Value,
                        return monomorph_LValue(resolve, params, e);
                        ),
                    (Path,
                        return params.monomorph(resolve, e);
                        ),
                    (Intrinsic,
                        return ::MIR::CallTarget::make_Intrinsic({ e.name, params.monomorph(resolve, e.params) });
                        )
                    )
                    throw "";
                }
            };
            terminator = ::MIR::Terminator::make_Call({
                e.ret_block, e.panic_block,
                monomorph_LValue(resolve, params, e.ret_val),
                H::monomorph_calltarget(resolve, params, e.fcn),
                monomorph_Param_list(resolve, params, e.args)
                });
            )
        )

        output.blocks.push_back( ::MIR::BasicBlock { mv$(statements), mv$(terminator) } );
    }

    return ::MIR::FunctionPointer( box$(output).release() );
}

/// Monomorphise all functions in a TransList
void Trans_Monomorphise_List(const ::HIR::Crate& crate, TransList& list)
{
    ::StaticTraitResolve    resolve { crate };
    for(auto& fcn_ent : list.m_functions)
    {
        const auto& fcn = *fcn_ent.second->ptr;
        // Trait methods (which are the only case where `Self` can exist in the argument list at this stage) always need to be monomorphised.
        bool is_method = ( fcn.m_args.size() > 0 && visit_ty_with(fcn.m_args[0].second, [&](const auto& x){return x == ::HIR::TypeRef("Self",0xFFFF);}) );
        if(fcn_ent.second->pp.has_types() || is_method)
        {
            const auto& path = fcn_ent.first;
            const auto& pp = fcn_ent.second->pp;
            TRACE_FUNCTION_FR(path, path);
            ASSERT_BUG(Span(), fcn.m_code.m_mir, "No code for " << path);

            auto mir = Trans_Monomorphise(resolve, fcn_ent.second->pp, fcn.m_code.m_mir);

            // TODO: Should these be moved to their own pass? Potentially not, the extra pass should just be an inlining optimise pass
            auto ret_type = pp.monomorph(resolve, fcn.m_return);
            ::HIR::Function::args_t args;
            for(const auto& a : fcn.m_args)
                args.push_back(::std::make_pair( ::HIR::Pattern{}, pp.monomorph(resolve, a.second) ));

            //::std::string s = FMT(path);
            ::HIR::ItemPath ip(path);
            MIR_Validate(resolve, ip, *mir, args, ret_type);
            MIR_Cleanup(resolve, ip, *mir, args, ret_type);
            MIR_Optimise(resolve, ip, *mir, args, ret_type);
            MIR_Validate(resolve, ip, *mir, args, ret_type);

            fcn_ent.second->monomorphised.ret_ty = ::std::move(ret_type);
            fcn_ent.second->monomorphised.arg_tys = ::std::move(args);
            fcn_ent.second->monomorphised.code = ::std::move(mir);
        }
    }

    // Also do constants and statics (stored in where?)
    // - NOTE: Done in reverse order, because consteval needs used constants to be evaluated
    for(auto& ent : reverse(list.m_constants))
    {
        const auto& path = ent.first;
        const auto& pp = ent.second->pp;
        const auto& c = *ent.second->ptr;
        TRACE_FUNCTION_FR(path, path);
        auto ty = pp.monomorph(resolve, c.m_type);
        // 1. Evaluate the constant
        struct Nvs: public ::HIR::Evaluator::Newval
        {
            ::HIR::Path new_static(::HIR::TypeRef type, ::HIR::Literal value) override {
                TODO(Span(), "Create new static in monomorph pass - " << value << " : " << type);
            }
        } nvs;
        auto eval = ::HIR::Evaluator { pp.sp, crate, nvs };
        MonomorphState   ms;
        ms.self_ty = &pp.self_type;
        ms.pp_impl = &pp.pp_impl;
        ms.pp_method = &pp.pp_method;
        auto new_lit = eval.evaluate_constant(path, c.m_value, ::std::move(ty), ::std::move(ms));
        ASSERT_BUG(Span(), !new_lit.is_Defer(), "Result of evaluating " << path << " was still Defer");
        // 2. Store evaluated HIR::Literal in c.m_monomorph_cache
        c.m_monomorph_cache.insert(::std::make_pair( path.clone(), ::std::move(new_lit) ));
    }
}

