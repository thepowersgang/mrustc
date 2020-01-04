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

namespace {
    ::MIR::Param clone_field(const State& state, const Span& sp, ::MIR::Function& mir_fcn, const ::HIR::TypeRef& subty, ::MIR::LValue fld_lvalue)
    {
        if( state.resolve.type_is_copy(sp, subty) )
        {
            return ::std::move(fld_lvalue);
        }
        else
        {
            const auto& lang_Clone = state.resolve.m_crate.get_lang_item_path(sp, "clone");
            // Allocate to locals (one for the `&T`, the other for the cloned `T`)
            auto borrow_lv = ::MIR::LValue::new_Local( mir_fcn.locals.size() );
            mir_fcn.locals.push_back(::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, subty.clone()));
            auto res_lv = ::MIR::LValue::new_Local( mir_fcn.locals.size() );
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
            panic_bb.terminator = ::MIR::Terminator::make_Diverge({});
            mir_fcn.blocks.push_back(::std::move( panic_bb ));

            // Save the output of the `clone` call
            return ::std::move(res_lv);
        }
    }
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
            ::MIR::LValue::new_Return(),
            ::MIR::RValue::make_Use( ::MIR::LValue::new_Deref( ::MIR::LValue::new_Argument(0) ) )
            }));
        bb.terminator = ::MIR::Terminator::make_Return({});
        mir_fcn.blocks.push_back(::std::move( bb ));
    }
    else
    {
        TU_MATCH_HDRA( (ty.m_data), {)
        default:
            TODO(sp, "auto Clone for " << ty << " - Unknown and not Copy");
        TU_ARMA(Path, te) {
            if( te.is_closure() ) {
                const auto& gp = te.path.m_data.as_Generic();
                const auto& str = state.resolve.m_crate.get_struct_by_path(sp, gp.m_path);
                Trans_Params p;
                p.sp = sp;
                p.pp_impl = gp.m_params.clone();
                ::std::vector< ::MIR::Param>   values; values.reserve( str.m_data.as_Tuple().size() );
                for(const auto& fld : str.m_data.as_Tuple())
                {
                    ::HIR::TypeRef  tmp;
                    const auto& ty_m = monomorphise_type_needed(fld.ent) ? (tmp = p.monomorph(state.resolve, fld.ent)) : fld.ent;
                    auto fld_lvalue = ::MIR::LValue::new_Field( ::MIR::LValue::new_Deref(::MIR::LValue::new_Argument(0)), static_cast<unsigned>(values.size()) );
                    values.push_back( clone_field(state, sp, mir_fcn, ty_m, mv$(fld_lvalue)) );
                }
                // Construct the result value
                ::MIR::BasicBlock   bb;
                bb.statements.push_back(::MIR::Statement::make_Assign({
                    ::MIR::LValue::new_Return(),
                    ::MIR::RValue::make_Struct({ gp.clone(), mv$(values) })
                    }));
                bb.terminator = ::MIR::Terminator::make_Return({});
                mir_fcn.blocks.push_back(::std::move( bb ));
            }
            else {
                TODO(sp, "auto Clone for " << ty << " - Unknown and not Copy");
            }
            }
        TU_ARMA(Array, te) {
            ASSERT_BUG(sp, te.size.as_Known() < 256, "TODO: Is more than 256 elements sane for auto-generated non-Copy Clone impl? " << ty);
            ::std::vector< ::MIR::Param>   values; values.reserve(te.size.as_Known());
            for(size_t i = 0; i < te.size.as_Known(); i ++)
            {
                auto fld_lvalue = ::MIR::LValue::new_Field( ::MIR::LValue::new_Deref(::MIR::LValue::new_Argument(0)), static_cast<unsigned>(values.size()) );
                values.push_back( clone_field(state, sp, mir_fcn, *te.inner, mv$(fld_lvalue)) );
            }
            // Construct the result
            ::MIR::BasicBlock   bb;
            bb.statements.push_back(::MIR::Statement::make_Assign({
                ::MIR::LValue::new_Return(),
                ::MIR::RValue::make_Array({ mv$(values) })
                }));
            bb.terminator = ::MIR::Terminator::make_Return({});
            mir_fcn.blocks.push_back(::std::move( bb ));
            }
        TU_ARMA(Tuple, te) {
            assert(te.size() > 0);

            ::std::vector< ::MIR::Param>   values; values.reserve(te.size());
            // For each field of the tuple, create a clone (either using Copy if posible, or calling Clone::clone)
            for(const auto& subty : te)
            {
                auto fld_lvalue = ::MIR::LValue::new_Field( ::MIR::LValue::new_Deref(::MIR::LValue::new_Argument(0)), static_cast<unsigned>(values.size()) );
                values.push_back( clone_field(state, sp, mir_fcn, subty, mv$(fld_lvalue)) );
            }

            // Construct the result tuple
            ::MIR::BasicBlock   bb;
            bb.statements.push_back(::MIR::Statement::make_Assign({
                ::MIR::LValue::new_Return(),
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
    impl.m_methods.insert(::std::make_pair( RcString::new_interned("clone"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false, ::std::move(fcn) } ));

    // Add impl to the crate
    auto& list = state.crate.m_trait_impls[state.lang_Clone].get_list_for_type_mut(impl.m_type);
    list.push_back( box$(impl) );
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

    auto impl_list_it = crate.m_trait_impls.find(state.lang_Clone);
    for(const auto& ty : state.done_list)
    {
        assert(impl_list_it != crate.m_trait_impls.end());
        // TODO: Find a way of turning a set into a vector so items can be erased.

        auto p = ::HIR::Path(ty.clone(), ::HIR::GenericPath(state.lang_Clone), "clone");
        //DEBUG("add_function(" << p << ")");
        auto e = trans_list.add_function(::std::move(p));

        const auto* impl_list = impl_list_it->second.get_list_for_type(ty);
        ASSERT_BUG(Span(), impl_list, "No impl list of Clone for " << ty);
        auto& impl = **::std::find_if( impl_list->begin(), impl_list->end(), [&](const auto& i){ return i->m_type == ty; });
        assert( impl.m_methods.size() == 1 );
        e->ptr = &impl.m_methods.begin()->second.data;
    }
}

