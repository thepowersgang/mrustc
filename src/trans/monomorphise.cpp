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
    ::MIR::LValue monomorph_LValue(const ::HIR::Crate& crate, const Trans_Params& params, const ::MIR::LValue& tpl)
    {
        TU_MATCHA( (tpl), (e),
        (Variable,  return e; ),
        (Temporary, return e; ),
        (Argument,  return e; ),
        (Return, return e; ),
        (Static,
            return params.monomorph(crate, e);
            ),
        (Field,
            return ::MIR::LValue::make_Field({
                box$(monomorph_LValue(crate, params, *e.val)),
                e.field_index
                });
            ),
        (Deref,
            return ::MIR::LValue::make_Deref({
                box$(monomorph_LValue(crate, params, *e.val))
                });
            ),
        (Index,
            return ::MIR::LValue::make_Index({
                box$(monomorph_LValue(crate, params, *e.val)),
                box$(monomorph_LValue(crate, params, *e.idx))
                });
            ),
        (Downcast,
            return ::MIR::LValue::make_Downcast({
                box$(monomorph_LValue(crate, params, *e.val)),
                e.variant_index
                });
            )
        )
        throw "";
    }
    ::std::vector<::MIR::LValue> monomorph_LValue_list(const ::HIR::Crate& crate, const Trans_Params& params, const ::std::vector<::MIR::LValue>& tpl)
    {
        ::std::vector<::MIR::LValue>    rv;
        rv.reserve( tpl.size() );
        for(const auto& v : tpl)
            rv.push_back( monomorph_LValue(crate, params, v) );
        return rv;
    }
}

::MIR::FunctionPointer Trans_Monomorphise(const ::HIR::Crate& crate, const Trans_Params& params, const ::MIR::FunctionPointer& tpl)
{
    static Span sp;
    TRACE_FUNCTION;

    ::MIR::Function output;

    // 1. Monomorphise locals and temporaries
    output.named_variables.reserve( tpl->named_variables.size() );
    for(const auto& var : tpl->named_variables)
    {
        DEBUG("- var" << output.named_variables.size());
        output.named_variables.push_back( params.monomorph(crate, var) );
    }
    output.temporaries.reserve( tpl->temporaries.size() );
    for(const auto& ty : tpl->temporaries)
    {
        DEBUG("- var" << output.temporaries.size());
        output.temporaries.push_back( params.monomorph(crate, ty) );
    }

    // 2. Monomorphise all paths
    output.blocks.reserve( tpl->blocks.size() );
    for(const auto& block : tpl->blocks)
    {
        ::std::vector< ::MIR::Statement>    statements;

        TRACE_FUNCTION_F("bb" << output.blocks.size());
        statements.reserve( block.statements.size() );
        for(const auto& stmt : block.statements)
        {
            assert( stmt.is_Drop() || stmt.is_Assign() );
            if( stmt.is_Drop() )
            {
                const auto& e = stmt.as_Drop();
                DEBUG("- DROP " << e.slot);
                statements.push_back( ::MIR::Statement::make_Drop({
                    e.kind,
                    monomorph_LValue(crate, params, e.slot)
                    }) );
            }
            else
            {
                const auto& e = stmt.as_Assign();
                DEBUG("- " << e.dst << " = " << e.src);

                ::MIR::RValue   rval;
                TU_MATCHA( (e.src), (se),
                (Use,
                    rval = ::MIR::RValue( monomorph_LValue(crate, params, se) );
                    ),
                (Constant,
                    TU_MATCHA( (se), (ce),
                    (Int,
                        rval = ::MIR::Constant::make_Int(ce);
                        ),
                    (Uint,
                        rval = ::MIR::Constant::make_Uint(ce);
                        ),
                    (Float,
                        rval = ::MIR::Constant::make_Float(ce);
                        ),
                    (Bool,
                        rval = ::MIR::Constant::make_Bool(ce);
                        ),
                    (Bytes,
                        rval = ::MIR::Constant(ce);
                        ),
                    (StaticString,
                        rval = ::MIR::Constant(ce);
                        ),
                    (Const,
                        rval = ::MIR::Constant::make_Const({
                            params.monomorph(crate, ce.p)
                            });
                        ),
                    (ItemAddr,
                        auto p = params.monomorph(crate, ce);
                        // TODO: If this is a pointer to a function on a trait object, replace with the address loaded from the vtable.
                        // - Requires creating a new temporary for the vtable pointer.
                        // - Also requires knowing what the receiver is.
                        rval = ::MIR::Constant( mv$(p) );
                        )
                    )
                    ),
                (SizedArray,
                    rval = ::MIR::RValue::make_SizedArray({
                        monomorph_LValue(crate, params, se.val),
                        se.count
                        });
                    ),
                (Borrow,
                    rval = ::MIR::RValue::make_Borrow({
                        se.region, se.type,
                        monomorph_LValue(crate, params, se.val)
                        });
                    ),
                (Cast,
                    rval = ::MIR::RValue::make_Cast({
                        monomorph_LValue(crate, params, se.val),
                        params.monomorph(crate, se.type)
                        });
                    ),
                (BinOp,
                    rval = ::MIR::RValue::make_BinOp({
                        monomorph_LValue(crate, params, se.val_l),
                        se.op,
                        monomorph_LValue(crate, params, se.val_r)
                        });
                    ),
                (UniOp,
                    rval = ::MIR::RValue::make_UniOp({
                        monomorph_LValue(crate, params, se.val),
                        se.op
                        });
                    ),
                (DstMeta,
                    auto lv = monomorph_LValue(crate, params, se.val);
                    // TODO: Get the type of this, and if it's an array - replace with the size
                    rval = ::MIR::RValue::make_DstMeta({ mv$(lv) });
                    ),
                (DstPtr,
                    rval = ::MIR::RValue::make_DstPtr({ monomorph_LValue(crate, params, se.val) });
                    ),
                (MakeDst,
                    rval = ::MIR::RValue::make_MakeDst({
                        monomorph_LValue(crate, params, se.ptr_val),
                        monomorph_LValue(crate, params, se.meta_val)
                        });
                    ),
                (Tuple,
                    rval = ::MIR::RValue::make_Tuple({
                        monomorph_LValue_list(crate, params, se.vals)
                        });
                    ),
                (Array,
                    rval = ::MIR::RValue::make_Array({
                        monomorph_LValue_list(crate, params, se.vals)
                        });
                    ),
                // Create a new instance of a union (and eventually enum)
                (Variant,
                    rval = ::MIR::RValue::make_Variant({
                        params.monomorph(crate, se.path),
                        se.index,
                        monomorph_LValue(crate, params, se.val)
                        });
                    ),
                // Create a new instance of a struct (or enum)
                (Struct,
                    rval = ::MIR::RValue::make_Struct({
                        params.monomorph(crate, se.path),
                        se.variant_idx,
                        monomorph_LValue_list(crate, params, se.vals)
                        });
                    )
                )

                statements.push_back( ::MIR::Statement::make_Assign({
                    monomorph_LValue(crate, params, e.dst),
                    mv$(rval)
                    }) );
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
                monomorph_LValue(crate, params, e.cond),
                e.bb0, e.bb1
                });
            ),
        (Switch,
            terminator = ::MIR::Terminator::make_Switch({
                monomorph_LValue(crate, params, e.val),
                e.targets
                });
            ),
        (Call,
            struct H {
                static ::MIR::CallTarget monomorph_calltarget(const ::HIR::Crate& crate, const Trans_Params& params, const ::MIR::CallTarget& ct) {
                    TU_MATCHA( (ct), (e),
                    (Value,
                        return monomorph_LValue(crate, params, e);
                        ),
                    (Path,
                        return params.monomorph(crate, e);
                        ),
                    (Intrinsic,
                        return ::MIR::CallTarget::make_Intrinsic({ e.name, params.monomorph(crate, e.params) });
                        )
                    )
                    throw "";
                }
            };
            terminator = ::MIR::Terminator::make_Call({
                e.ret_block, e.panic_block,
                monomorph_LValue(crate, params, e.ret_val),
                H::monomorph_calltarget(crate, params, e.fcn),
                monomorph_LValue_list(crate, params, e.args)
                });
            )
        )

        output.blocks.push_back( ::MIR::BasicBlock { mv$(statements), mv$(terminator) } );
    }

    return ::MIR::FunctionPointer( box$(output).release() );
}
