/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/monomorphise.hpp
 * - MIR monomorphisation
 */
#include "monomorphise.hpp"
#include <mir/mir.hpp>
#include <hir/hir.hpp>

namespace {
    ::MIR::LValue monomorph_LValue(const ::StaticTraitResolve& resolve, const Trans_Params& params, const ::MIR::LValue& tpl)
    {
        TU_MATCHA( (tpl), (e),
        (Variable,  return e; ),
        (Temporary, return e; ),
        (Argument,  return e; ),
        (Return, return e; ),
        (Static,
            return params.monomorph(resolve, e);
            ),
        (Field,
            return ::MIR::LValue::make_Field({
                box$(monomorph_LValue(resolve, params, *e.val)),
                e.field_index
                });
            ),
        (Deref,
            return ::MIR::LValue::make_Deref({
                box$(monomorph_LValue(resolve, params, *e.val))
                });
            ),
        (Index,
            return ::MIR::LValue::make_Index({
                box$(monomorph_LValue(resolve, params, *e.val)),
                box$(monomorph_LValue(resolve, params, *e.idx))
                });
            ),
        (Downcast,
            return ::MIR::LValue::make_Downcast({
                box$(monomorph_LValue(resolve, params, *e.val)),
                e.variant_index
                });
            )
        )
        throw "";
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
                params.monomorph(resolve, ce.p)
                });
            ),
        (ItemAddr,
            auto p = params.monomorph(resolve, ce);
            // TODO: If this is a pointer to a function on a trait object, replace with the address loaded from the vtable.
            // - Requires creating a new temporary for the vtable pointer.
            // - Also requires knowing what the receiver is.
            return ::MIR::Constant( mv$(p) );
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

    ::MIR::Function output;

    // 1. Monomorphise locals and temporaries
    output.named_variables.reserve( tpl->named_variables.size() );
    for(const auto& var : tpl->named_variables)
    {
        DEBUG("- var" << output.named_variables.size());
        output.named_variables.push_back( params.monomorph(resolve, var) );
    }
    output.temporaries.reserve( tpl->temporaries.size() );
    for(const auto& ty : tpl->temporaries)
    {
        DEBUG("- tmp" << output.temporaries.size());
        output.temporaries.push_back( params.monomorph(resolve, ty) );
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
                        se.variant_idx,
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
