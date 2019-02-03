/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/auto_impls.cpp
 * - Automatic trait/method impls
 *
 * Handles implementing Clone (when in 1.29 mode)
 */
#include "main_bindings.hpp"
#include "trans_list.hpp"
#include <hir/hir.hpp>
#include <mir/mir.hpp>
#include <hir_typeck/common.hpp>    // monomorph
#include <hir_typeck/static.hpp>    // StaticTraitResolve
#include <deque>
#include <algorithm>    // find_if

namespace {
    struct State
    {
        ::HIR::Crate&   crate;
        StaticTraitResolve resolve;
        const TransList& trans_list;
        ::std::deque<::HIR::TypeRef>    todo_list;
        ::std::set<::HIR::TypeRef>  done_list;

        ::HIR::SimplePath  lang_Clone;

        State(::HIR::Crate& crate, const TransList& trans_list):
            crate(crate),
            resolve( crate ),
            trans_list(trans_list)
        {
            lang_Clone = crate.get_lang_item_path(Span(), "clone");
        }

        void enqueue_type(const ::HIR::TypeRef& ty) {
            if( this->trans_list.auto_clone_impls.count(ty) == 0 && this->done_list.count(ty) == 0 ) {
                this->done_list.insert( ty.clone() );
                this->todo_list.push_back( ty.clone() );
            }
        }
    };
}

void Trans_AutoImpl_Clone(State& state, ::HIR::TypeRef ty)
{
    Span    sp;
    TRACE_FUNCTION_F(ty);

    // Create MIR
    ::MIR::Function mir_fcn;
    if( state.resolve.type_is_copy(sp, ty) )
    {
        ::MIR::BasicBlock   bb;
        bb.statements.push_back(::MIR::Statement::make_Assign({
            ::MIR::LValue::make_Return({}),
            ::MIR::RValue::make_Use( ::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Argument({ 0 })) }) )
            }));
        bb.terminator = ::MIR::Terminator::make_Return({});
        mir_fcn.blocks.push_back(::std::move( bb ));
    }
    else
    {
        const auto& lang_Clone = state.resolve.m_crate.get_lang_item_path(sp, "clone");
        TU_MATCH_HDRA( (ty.m_data), {)
        default:
            TODO(sp, "auto Clone for " << ty << " - Not Copy");
        TU_ARMA(Tuple, te) {
            assert(te.size() > 0);

            ::std::vector< ::MIR::Param>   values;
            // For each field of the tuple, create a clone (either using Copy if posible, or calling Clone::clone)
            for(const auto& subty : te)
            {
                auto fld_lvalue = ::MIR::LValue::make_Field({ box$(::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Argument({ 0 })) })), static_cast<unsigned>(values.size()) });
                if( state.resolve.type_is_copy(sp, subty) )
                {
                    values.push_back( ::std::move(fld_lvalue) );
                }
                else
                {
                    // Allocate to locals (one for the `&T`, the other for the cloned `T`)
                    auto borrow_lv = ::MIR::LValue::make_Local( mir_fcn.locals.size() );
                    mir_fcn.locals.push_back(::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, subty.clone()));
                    auto res_lv = ::MIR::LValue::make_Local( mir_fcn.locals.size() );
                    mir_fcn.locals.push_back(subty.clone());

                    // Call `<T as Clone>::clone`, passing a borrow of the field
                    ::MIR::BasicBlock   bb;
                    bb.statements.push_back(::MIR::Statement::make_Assign({
                            borrow_lv.clone(),
                            ::MIR::RValue::make_Borrow({ 0, ::HIR::BorrowType::Shared, mv$(fld_lvalue) })
                            }));
                    bb.terminator = ::MIR::Terminator::make_Call({
                            static_cast<unsigned>(mir_fcn.blocks.size() + 2),  // return block (after the panic block below)
                            static_cast<unsigned>(mir_fcn.blocks.size() + 1),  // panic block (next block)
                            res_lv.clone(),
                            ::MIR::CallTarget( ::HIR::Path(subty.clone(), lang_Clone, "clone") ),
                            ::make_vec1<::MIR::Param>( ::std::move(borrow_lv) )
                            });
                    mir_fcn.blocks.push_back(::std::move( bb ));

                    // Stub panic handling (TODO: Make this iterate `values` and drop all of them)
                    ::MIR::BasicBlock   panic_bb;
                    bb.terminator = ::MIR::Terminator::make_Diverge({});
                    mir_fcn.blocks.push_back(::std::move( panic_bb ));

                    // Save the output of the `clone` call
                    values.push_back( ::std::move(res_lv) );
                }
            }

            // Construct the result tuple
            ::MIR::BasicBlock   bb;
            bb.statements.push_back(::MIR::Statement::make_Assign({
                ::MIR::LValue::make_Return({}),
                ::MIR::RValue::make_Tuple({ mv$(values) })
                }));
            bb.terminator = ::MIR::Terminator::make_Return({});
            mir_fcn.blocks.push_back(::std::move( bb ));
            }
        }
    }

    // Function
    ::HIR::Function fcn {
        /*m_save_code=*/false,
        ::HIR::Linkage {},
        ::HIR::Function::Receiver::BorrowShared,
        /*m_abi=*/ABI_RUST,
        /*m_unsafe =*/false,
        /*m_const=*/false,
        ::HIR::GenericParams {},
        /*m_args=*/::make_vec1(::std::make_pair(
            ::HIR::Pattern( ::HIR::PatternBinding(false, ::HIR::PatternBinding::Type::Move, "self", 0), ::HIR::Pattern::Data::make_Any({}) ),
            ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ty.clone())
            )),
        /*m_variadic=*/false,
        /*m_return=*/ty.clone(),
        ::HIR::ExprPtr {}
        };
    fcn.m_code.m_mir = ::MIR::FunctionPointer( new ::MIR::Function(mv$(mir_fcn)) );

    // Impl
    ::HIR::TraitImpl    impl;
    impl.m_type = mv$(ty);
    impl.m_methods.insert(::std::make_pair( ::std::string("clone"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false, ::std::move(fcn) } ));

    // Add impl to the crate
    state.crate.m_trait_impls.insert(::std::make_pair( state.lang_Clone, ::std::move(impl) ));
}

void Trans_AutoImpls(::HIR::Crate& crate, TransList& trans_list)
{
    if( TARGETVER_1_19 )
        return ;

    State   state { crate, trans_list };

    // Generate for all 
    for(const auto& ty : trans_list.auto_clone_impls)
    {
        state.done_list.insert( ty.clone() );
        Trans_AutoImpl_Clone(state, ty.clone());
    }

    while( !state.todo_list.empty() )
    {
        auto ty = ::std::move(state.todo_list.front());
        state.todo_list.pop_back();

        Trans_AutoImpl_Clone(state, mv$(ty));
    }

    const auto impl_range = crate.m_trait_impls.equal_range( state.lang_Clone );
    for(const auto& ty : state.done_list)
    {
        // TODO: Find a way of turning a set into a vector so items can be erased.

        auto p = ::HIR::Path(ty.clone(), ::HIR::GenericPath(state.lang_Clone), "clone");
        //DEBUG("add_function(" << p << ")");
        auto e = trans_list.add_function(::std::move(p));

        auto it = ::std::find_if( impl_range.first, impl_range.second, [&](const auto& i){ return i.second.m_type == ty; });
        assert( it->second.m_methods.size() == 1 );
        e->ptr = &it->second.m_methods.begin()->second.data;
    }
}

