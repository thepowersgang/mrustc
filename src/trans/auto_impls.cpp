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
#include <trans/target.hpp>
#include <mir/operations.hpp>
#include <mir/helpers.hpp>

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
            lang_Clone = crate.get_lang_item_path_opt("clone");
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
                    ::MIR::RValue::make_Borrow({ ::HIR::BorrowType::Shared, mv$(fld_lvalue) })
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
        TU_MATCH_HDRA( (ty.data()), {)
        default:
            TODO(sp, "auto Clone for " << ty << " - Unknown and not Copy");
        TU_ARMA(Path, te) {
            if( te.is_closure() ) {
                const auto& gp = te.path.m_data.as_Generic();
                const auto& str = state.resolve.m_crate.get_struct_by_path(sp, gp.m_path);
                auto p = Trans_Params::new_impl(sp, ty.clone(), gp.m_params.clone());
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
                values.push_back( clone_field(state, sp, mir_fcn, te.inner, mv$(fld_lvalue)) );
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
        ::HIR::Function::Receiver::BorrowShared,
        ::HIR::GenericParams {},
        /*m_args=*/::make_vec1(::std::make_pair(
            ::HIR::Pattern( ::HIR::PatternBinding(false, ::HIR::PatternBinding::Type::Move, "self", 0), ::HIR::Pattern::Data::make_Any({}) ),
            ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ty.clone())
            )),
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
    state.crate.m_all_trait_impls[state.lang_Clone].get_list_for_type_mut(list.back()->m_type).push_back( list.back().get() );
}

namespace {

    struct Builder
    {
        const State&    state;
        MIR::Function&  mir;
        const MIR::LValue   self;

        Builder(const State& state, MIR::Function& mir)
            : state(state)
            , mir(mir)
            , self(MIR::LValue::new_Argument(0))
        {
            mir.blocks.push_back(MIR::BasicBlock());
        }

        MIR::LValue add_local(HIR::TypeRef ty) {
            auto rv = mir.locals.size();
            mir.locals.push_back(mv$(ty));
            return MIR::LValue::new_Local(rv);
        }

        void ensure_open() {
            if(!mir.blocks.back().terminator.is_Incomplete()) {
                mir.blocks.push_back(MIR::BasicBlock());
            }
        }
        void push_stmt(MIR::Statement s) {
            ensure_open();
            mir.blocks.back().statements.push_back(mv$(s));
        }
        void push_stmt_assign(MIR::LValue lv, MIR::RValue rv) {
            this->push_stmt(MIR::Statement::make_Assign({ mv$(lv), mv$(rv) }));
        }
        void push_stmt_drop(MIR::LValue lv) {
            this->push_stmt(MIR::Statement::make_Drop({ MIR::eDropKind::DEEP, mv$(lv), ~0u }));
        }

        void terminate_block(MIR::Terminator term) {
            assert(mir.blocks.back().terminator.is_Incomplete());
            mir.blocks.back().terminator = mv$(term);
        }
        void terminate_Call(MIR::LValue rv, MIR::CallTarget tgt, std::vector<MIR::Param> args, MIR::BasicBlockId bb_ret, MIR::BasicBlockId bb_panic)
        {
            this->terminate_block( MIR::Terminator::make_Call({
                bb_ret, bb_panic,
                mv$(rv),
                mv$(tgt),
                mv$(args)
                }) );
        }

        void push_CallDrop(const HIR::TypeRef& ty) {
            // Get a `&mut *self`
            auto borrow_lv = this->add_local( HIR::TypeRef::new_borrow(HIR::BorrowType::Unique, ty.clone()) );
            this->push_stmt_assign( borrow_lv.clone(), MIR::RValue::make_Borrow({ HIR::BorrowType::Unique, ::MIR::LValue::new_Deref(this->self.clone()) }) );

            this->terminate_Call(
                MIR::LValue::new_Return(), ::HIR::Path(ty.clone(), state.resolve.m_lang_Drop, "drop"), make_vec1<MIR::Param>(mv$(borrow_lv)),
                /*bb_ret=*/mir.blocks.size()+1,
                /*bb_panic=*/mir.blocks.size()
            );
            // In panic block
            this->ensure_open();
            this->terminate_block( MIR::Terminator::make_Diverge({}) );
            // In continue block
            this->ensure_open();
        }
    };
}

void Trans_AutoImpls(::HIR::Crate& crate, TransList& trans_list)
{

    State   state { crate, trans_list };

    if( TARGETVER_LEAST_1_29 )
    {
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

    // Trait object methods
    {
        TRACE_FUNCTION_F("Trait object methods");
        trans_list.m_auto_functions.reserve(trans_list.m_auto_functions.size() + trans_list.trait_object_methods.size());
        for(const auto& path : trans_list.trait_object_methods)
        {
            DEBUG(path);
            static Span sp;
            const auto& pe = path.m_data.as_UfcsKnown();
            const auto& trait_path = pe.trait;
            const auto& name = pe.item;
            const auto& ty_dyn = pe.type.data().as_TraitObject();

            const auto& trait = crate.get_trait_by_path(sp, trait_path.m_path);
            const auto& fcn_def = trait.m_values.at(name).as_Function();

            // Get the vtable index for this function
            unsigned vtable_idx = ty_dyn.m_trait.m_trait_ptr->get_vtable_value_index(trait_path, name);
            ASSERT_BUG(sp, vtable_idx > 0, "Calling method '" << name << "' from " << trait_path << " through " << pe.type << " which isn't in the vtable");

            MonomorphStatePtr   ms(&pe.type, &trait_path.m_params, nullptr);

            HIR::Function   new_fcn;
            new_fcn.m_return = ms.monomorph_type(sp, fcn_def.m_return);
            state.resolve.expand_associated_types(sp, new_fcn.m_return);
            for(const auto& arg : fcn_def.m_args)
            {
                new_fcn.m_args.push_back(std::make_pair( HIR::Pattern(), ms.monomorph_type(sp, arg.second) ));
                state.resolve.expand_associated_types(sp, new_fcn.m_args.back().second);
            }
            HIR::BorrowType bt;
            if( fcn_def.m_receiver == HIR::Function::Receiver::Value )
            {
                // By-value trait object dispatch
                // - Receiver should be a `&move` (BUT, does the caller know this?)
                // - MIR Cleanup should fix that (after monomoprh)
                auto& self_ty = new_fcn.m_args.front().second;
                bt = HIR::BorrowType::Owned;
                self_ty = ::HIR::TypeRef::new_borrow(bt, mv$(self_ty));
                DEBUG("<dyn " << trait_path << ">::" << name << " - By-Value");
            }
            else
            {
                bt = new_fcn.m_args.front().second.data().as_Borrow().type;
            }

            new_fcn.m_code.m_mir = MIR::FunctionPointer(new MIR::Function());
            Builder builder(state, *new_fcn.m_code.m_mir);

            // ---
            // bb0:
            //   _1 = DstPtr a1
            auto lv_ptr = builder.add_local(::HIR::TypeRef::new_borrow(bt, ::HIR::TypeRef::new_unit()));
            builder.push_stmt_assign(lv_ptr.clone(), MIR::RValue::make_DstPtr({ MIR::LValue::new_Argument(0) }));
            //   _2 = DstMeta a1
            auto lv_vtable = builder.add_local(::HIR::TypeRef::new_borrow(
                HIR::BorrowType::Shared, ty_dyn.m_trait.m_trait_ptr->get_vtable_type(sp, crate, ty_dyn)
                ));
            builder.push_stmt_assign(lv_vtable.clone(), MIR::RValue::make_DstMeta({ MIR::LValue::new_Argument(0) }));
            //   rv = _2*.{idx}(a2, ...) goto bb2 else bb3
            std::vector<MIR::Param> call_args;
            call_args.push_back(mv$(lv_ptr));
            for(size_t i = 1; i < fcn_def.m_args.size(); i ++)
            {
                call_args.push_back(MIR::LValue::new_Argument(i));
            }
            builder.terminate_Call(
                MIR::LValue::new_Return(),
                MIR::LValue::new_Field( MIR::LValue::new_Deref(mv$(lv_vtable)), vtable_idx ),
                mv$(call_args),
                1, 2
                );
            // bb1:
            //   RETURN
            builder.ensure_open();
            builder.terminate_block(MIR::Terminator::make_Return({}));
            // bb2:
            //   UNWIND
            builder.ensure_open();
            builder.terminate_block(MIR::Terminator::make_Diverge({}));
            // ---

            MIR_Validate(state.resolve, HIR::ItemPath(path), *new_fcn.m_code.m_mir, new_fcn.m_args, new_fcn.m_return);
            trans_list.m_auto_functions.push_back(box$(new_fcn));
            auto* e = trans_list.add_function(path.clone());
            e->ptr = trans_list.m_auto_functions.back().get();
        }
    }

    // Create VTable instances
    {
        TRACE_FUNCTION_F("VTables");
        trans_list.m_auto_statics.reserve( trans_list.m_vtables.size() );
        for(const auto& ent : trans_list.m_vtables)
        {
            const auto& path = ent.first;
            const auto& trait_path = path.m_data.as_UfcsKnown().trait;
            const auto& type = path.m_data.as_UfcsKnown().type;

            if( const auto* te = type.data().opt_Function() )
            {
                struct {
                    const char* fcn_name;
                    const HIR::SimplePath* trait_path;
                    HIR::BorrowType bt;
                } const entries[3] = {
                    { "call", &state.resolve.m_lang_Fn, HIR::BorrowType::Shared },
                    { "call_mut", &state.resolve.m_lang_FnMut, HIR::BorrowType::Unique },
                    { "call_once", &state.resolve.m_lang_FnOnce, HIR::BorrowType::Owned }
                };

                size_t  offset;
                if( trait_path.m_path == state.resolve.m_lang_Fn )
                    offset = 0;
                else if( trait_path.m_path == state.resolve.m_lang_FnMut )
                    offset = 1;
                else if( TARGETVER_LEAST_1_39 && trait_path.m_path == state.resolve.m_lang_FnOnce )
                    offset = 2;
                else
                    offset = 3; // Wait, is this reachable?

                for(; offset < sizeof(entries)/sizeof(entries[0]); offset ++)
                {
                    bool is_by_value = (offset == 2);
                    const auto& ent = entries[offset];

                    auto fcn_p = path.clone();
                    fcn_p.m_data.as_UfcsKnown().item = ent.fcn_name;
                    fcn_p.m_data.as_UfcsKnown().trait.m_path = ent.trait_path->clone();

                    ::std::vector<HIR::TypeRef> arg_tys;
                    for(const auto& ty : te->m_arg_types)
                        arg_tys.push_back( ty.clone() );
                    auto arg_ty = ::HIR::TypeRef(mv$(arg_tys));


                    HIR::Function   fcn;
                    fcn.m_return = te->m_rettype.clone();
                    fcn.m_args.push_back(std::make_pair( HIR::Pattern(), !is_by_value ? HIR::TypeRef::new_borrow(ent.bt, type.clone()) : type.clone() ));
                    fcn.m_args.push_back(std::make_pair( HIR::Pattern(), mv$(arg_ty) ));

                    fcn.m_code.m_mir = MIR::FunctionPointer(new MIR::Function());
                    Builder builder(state, *fcn.m_code.m_mir);

                    std::vector<MIR::Param> arg_params;
                    for(size_t i = 0; i < te->m_arg_types.size(); i ++)
                    {
                        arg_params.push_back(MIR::LValue::new_Field(MIR::LValue::new_Argument(1), i));
                    }
                    builder.terminate_Call(MIR::LValue::new_Return(),
                        !is_by_value ? MIR::LValue::new_Deref(MIR::LValue::new_Argument(0)) : MIR::LValue::new_Argument(0),
                        mv$(arg_params),
                        1, 2
                        );
                    // BB1: Return
                    builder.ensure_open();
                    builder.terminate_block(MIR::Terminator::make_Return({}));
                    // BB1: Diverge
                    builder.ensure_open();
                    builder.terminate_block(MIR::Terminator::make_Diverge({}));

                    MIR_Validate(state.resolve, HIR::ItemPath(path), *fcn.m_code.m_mir, fcn.m_args, fcn.m_return);
                    trans_list.m_auto_functions.push_back(box$(fcn));
                    auto* e = trans_list.add_function(mv$(fcn_p));
                    e->ptr = trans_list.m_auto_functions.back().get();
                }
            }
        }
        for(const auto& ent : trans_list.m_vtables)
        {
            Span    sp;
            const auto& trait_path = ent.first.m_data.as_UfcsKnown().trait;
            const auto& type = ent.first.m_data.as_UfcsKnown().type;
            DEBUG("VTABLE " << trait_path << " for " << type);
            // TODO: What's the use of `ent.second` here? (it's a `Trans_Params`)

            // Get the vtable type
            const auto& trait = crate.get_trait_by_path(sp, trait_path.m_path);
            const auto& vtable_sp = trait.m_vtable_path;
            ASSERT_BUG(sp, vtable_sp != HIR::SimplePath(), "Trait " << trait_path.m_path << " doesn't have a vtable");
            auto vtable_params = trait_path.m_params.clone();
            for(const auto& ty : trait.m_type_indexes) {
                auto aty = ::HIR::TypeRef::new_path( ::HIR::Path( type.clone(), trait_path.clone(), ty.first ), {} );
                state.resolve.expand_associated_types(sp, aty);
                vtable_params.m_types.push_back( mv$(aty) );
            }
            const auto& vtable_ref = crate.get_struct_by_path(sp, vtable_sp);
            auto vtable_ty = ::HIR::TypeRef::new_path( ::HIR::GenericPath(mv$(vtable_sp), mv$(vtable_params)), &vtable_ref );

            // Create vtable contents
            auto monomorph_cb_trait = MonomorphStatePtr(&type, &trait_path.m_params, nullptr);


            HIR::Linkage linkage;
            linkage.type = HIR::Linkage::Type::Weak;
            HIR::Static vtable_static( ::std::move(linkage), /*is_mut*/false, mv$(vtable_ty), {} );
            auto& vtable_data = vtable_static.m_value_res;
            const auto ptr_bytes = Target_GetPointerBits()/8;
            vtable_data.bytes.resize( (3+trait.m_value_indexes.size()) * ptr_bytes );
            size_t ofs = 0;
            auto push_ptr = [&vtable_data,&ofs,ptr_bytes](HIR::Path p) {
                assert(ofs + ptr_bytes <= vtable_data.bytes.size());
                vtable_data.relocations.push_back(Reloc::new_named( ofs, ptr_bytes, mv$(p) ));
                vtable_data.write_uint(ofs, ptr_bytes, EncodedLiteral::PTR_BASE);
                ofs += ptr_bytes;
                assert(ofs <= vtable_data.bytes.size());
            };
            // Drop glue
            trans_list.m_drop_glue.insert( type.clone() );
            push_ptr(::HIR::Path(type.clone(), "#drop_glue"));
            // Size & align
            {
                size_t  size, align;
                // NOTE: Uses the Size+Align version because that doesn't panic on unsized
                ASSERT_BUG(sp, Target_GetSizeAndAlignOf(sp, state.resolve, type, size, align), "Unexpected generic? " << type);
                vtable_data.write_uint(ofs, ptr_bytes, size ); ofs += ptr_bytes;
                vtable_data.write_uint(ofs, ptr_bytes, align); ofs += ptr_bytes;
            }

            // Methods
            // - The `m_value_indexes` list isn't sorted (well, it's sorted differently) so we need an `O(n^2)` search

            for(unsigned int i = 0; i < trait.m_value_indexes.size(); i ++ )
            {
                // Find the corresponding vtable entry
                for(const auto& m : trait.m_value_indexes)
                {
                    // NOTE: The "3" is the number of non-method vtable entries
                    if( m.second.first != 3+i )
                        continue ;

                    DEBUG("- " << m.second.first << " = " << m.second.second << " :: " << m.first);

                    auto trait_gpath = monomorph_cb_trait.monomorph_genericpath(sp, m.second.second, false);
                    auto item_path = ::HIR::Path(type.clone(), mv$(trait_gpath), m.first);

                    auto src_trait_ms = MonomorphStatePtr(&type, &item_path.m_data.as_UfcsKnown().trait.m_params, nullptr);
                    const auto& src_trait = state.resolve.m_crate.get_trait_by_path(sp, m.second.second.m_path);
                    const auto& item = src_trait.m_values.at(m.first);
                    // If the entry is a by-value function, then emit a reference to a shim
                    if( item.is_Function() )
                    {
                        const auto& tpl_fcn = item.as_Function();
                        if( tpl_fcn.m_receiver == HIR::Function::Receiver::Value )
                        {
                            auto call_path = item_path.clone();
                            item_path.m_data.as_UfcsKnown().item = RcString::new_interned(FMT(m.first << "#ptr"));
                            auto* e = trans_list.add_function(item_path.clone());
                            if(e)
                            {
                                // Create the shim (forward to the true call, dereferencing the first argument)
                                HIR::Function   new_fcn;
                                new_fcn.m_return = src_trait_ms.monomorph_type(sp, tpl_fcn.m_return);
                                state.resolve.expand_associated_types(sp, new_fcn.m_return);
                                new_fcn.m_args.push_back(std::make_pair( HIR::Pattern(), HIR::TypeRef::new_borrow(HIR::BorrowType::Owned, type.clone()) ));
                                for(size_t i = 1; i < tpl_fcn.m_args.size(); i ++ ) {
                                    new_fcn.m_args.push_back(std::make_pair( HIR::Pattern(), src_trait_ms.monomorph_type(sp, tpl_fcn.m_args[i].second) ));
                                }
                                for(size_t i = 0; i < new_fcn.m_args.size(); i ++ ) {
                                    state.resolve.expand_associated_types(sp, new_fcn.m_args[i].second);
                                }

                                DEBUG("> Generate shim: " << item_path);

                                new_fcn.m_code.m_mir = MIR::FunctionPointer(new MIR::Function());
                                ::MIR::TypeResolve  mir_res { sp, state.resolve, FMT_CB(ss, ss << item_path), new_fcn.m_return, new_fcn.m_args, *new_fcn.m_code.m_mir };
                                Builder builder(state, *new_fcn.m_code.m_mir);
                                // bb0:
                                //   rv = CALL ...
                                ::std::vector<::MIR::Param> call_args;
                                call_args.push_back( ::MIR::LValue::new_Deref(::MIR::LValue::new_Argument(0)) );
                                for(size_t i = 1; i < tpl_fcn.m_args.size(); i ++ ) {
                                    call_args.push_back( ::MIR::LValue::new_Argument(i) );
                                }
                                builder.terminate_Call(::MIR::LValue::new_Return(), mv$(call_path), std::move(call_args), 1, 2);
                                // bb1:
                                //   RETURN
                                builder.ensure_open();
                                builder.terminate_block(MIR::Terminator::make_Return({}));
                                // bb2:
                                //   UNWIND
                                builder.ensure_open();
                                builder.terminate_block(MIR::Terminator::make_Diverge({}));
                                // ---

                                MIR_Validate(state.resolve, HIR::ItemPath(item_path), *new_fcn.m_code.m_mir, new_fcn.m_args, new_fcn.m_return);
                                trans_list.m_auto_functions.push_back(box$(new_fcn));
                                e->ptr = trans_list.m_auto_functions.back().get();
                            }
                        }
                    }
                    //MIR_ASSERT(*m_mir_res, tr.m_values.at(m.first).is_Function(), "TODO: Handle generating vtables with non-function items");
                    push_ptr(mv$(item_path));
                }
            }
            assert(ofs == vtable_data.bytes.size());
            vtable_static.m_value_generated = true;

            // Add to list
            trans_list.m_auto_statics.push_back( box$(vtable_static) );
            auto* e = trans_list.add_static(ent.first.clone());
            e->ptr = trans_list.m_auto_statics.back().get();
        }
        trans_list.m_vtables.clear();
    }

    // Create drop glue implementations
    {
        TRACE_FUNCTION_F("Drop Glue");
        for(const auto& ty : trans_list.m_types)
        {
            Span    sp;
            if( ty.second )
                continue;
            if(!state.resolve.type_needs_drop_glue(sp, ty.first))
                continue ;

            if(ty.first.data().is_TraitObject()) {
                continue ;
            }
            if(ty.first.data().is_Slice()) {
                continue ;
            }
            trans_list.m_drop_glue.insert( ty.first.clone() );
        }

        for(const auto& ty : trans_list.m_drop_glue)
        {
            Span    sp;
            auto path = ::HIR::Path(ty.clone(), "#drop_glue");

            HIR::Function   fcn;
            fcn.m_return = HIR::TypeRef::new_unit();
            fcn.m_args.push_back(std::make_pair( HIR::Pattern(), HIR::TypeRef::new_borrow(HIR::BorrowType::Owned, ty.clone()) ));

            fcn.m_code.m_mir = MIR::FunctionPointer(new MIR::Function());
            ::MIR::TypeResolve  mir_res { sp, state.resolve, FMT_CB(ss, ss << path), fcn.m_return, fcn.m_args, *fcn.m_code.m_mir };
            Builder builder(state, *fcn.m_code.m_mir);
            builder.push_stmt_assign( MIR::LValue::new_Return(), MIR::RValue::make_Tuple({}) );
            if( const auto* ity = state.resolve.is_type_owned_box(ty) )
            {
                // Call inner destructor
                auto inner_ptr =
                    ::MIR::LValue::new_Field(
                        ::MIR::LValue::new_Field(
                            ::MIR::LValue::new_Deref( builder.self.clone() )
                            ,0)
                        ,0)
                    ;
                if(TARGETVER_MOST_1_29) {
                    inner_ptr = ::MIR::LValue::new_Field(std::move(inner_ptr), 0);
                }
                auto inner_val = ::MIR::LValue::new_Deref(std::move(inner_ptr));
                HIR::TypeRef    tmp;
                ASSERT_BUG(sp, mir_res.get_lvalue_type(tmp, inner_val) == *ity, "Hard-coded box pointer path didn't result in the inner type");
                builder.push_stmt_drop(std::move(inner_val));
                // Shallow drop the box (triggering a free call in the backend)
                builder.push_stmt(MIR::Statement::make_Drop({ MIR::eDropKind::SHALLOW, ::MIR::LValue::new_Deref(builder.self.clone()), ~0u }));
            }
            else if( state.resolve.type_needs_drop_glue(sp, ty) )
            {
                TU_MATCH_HDRA( (ty.data()), {)
                TU_ARMA(Infer, _te)
                    throw "";
                TU_ARMA(Generic, _te)
                    throw "";
                TU_ARMA(ErasedType, _te)
                    throw "";
                TU_ARMA(TraitObject, _te)
                    TODO(sp, "Drop glue for TraitObject? " << ty);
                TU_ARMA(Slice, _te)
                    TODO(sp, "Drop glue for Slice? " << ty);
                TU_ARMA(Closure, _te)
                    TODO(sp, "Drop glue for Closure? " << ty);  // Should this be dead already?
                TU_ARMA(Generator, _te)
                    TODO(sp, "Drop glue for Generator? " << ty);  // Should this be dead already?
                TU_ARMA(Diverge, te) {
                    // Exists for reasons...
                    builder.terminate_block( MIR::Terminator::make_Diverge({}) );
                    }
                TU_ARMA(Primitive, te) {
                    // Nothing to do
                    }
                TU_ARMA(Function, te) {
                    // Nothing to do
                    }
                TU_ARMA(Pointer, te) {
                    // Nothing to do
                    }
                TU_ARMA(Borrow, te) {
                    if(te.type == HIR::BorrowType::Owned)
                    {
                        // `drop a0**`
                        builder.push_stmt_drop(
                            ::MIR::LValue::new_Deref(
                                ::MIR::LValue::new_Deref(builder.self.clone())
                                )
                            );
                    }
                    }
                TU_ARMA(Tuple, te) {
                    auto self = ::MIR::LValue::new_Deref(builder.self.clone());
                    auto fld_lv = ::MIR::LValue::new_Field(mv$(self), 0);
                    for(size_t i = 0; i < te.size(); i++)
                    {
                        if( state.resolve.type_needs_drop_glue(sp, te[i]) )
                        {
                            builder.push_stmt_drop(fld_lv.clone());
                        }
                        fld_lv.inc_Field();
                    }
                    }
                TU_ARMA(Array, te) {
                    auto size = te.size.as_Known();
                    auto self = ::MIR::LValue::new_Deref(builder.self.clone());
                    if( size > 0 && state.resolve.type_needs_drop_glue(sp, te.inner) )
                    {
                        if(size <= 6)
                        {
                            auto fld_lv = ::MIR::LValue::new_Field(mv$(self), 0);
                            for(size_t i = 0; i < size; i ++)
                            {
                                builder.push_stmt_drop(fld_lv.clone());
                                fld_lv.inc_Field();
                            }
                        }
                        else
                        {
                            auto idx = builder.add_local(HIR::CoreType::Usize);
                            auto fld_lv = ::MIR::LValue::new_Index(mv$(self), idx.as_Local());
                            auto cmp = builder.add_local(HIR::CoreType::Bool);
                            builder.push_stmt_assign(idx.clone(), MIR::Constant::make_Uint({0, HIR::CoreType::Usize}));
                            builder.terminate_block(MIR::Terminator::make_Goto(1));
                            builder.push_stmt_drop(fld_lv.clone());
                            builder.push_stmt_assign(idx.clone(), MIR::RValue::make_BinOp({ idx.clone(), MIR::eBinOp::ADD, MIR::Constant::make_Uint({1, HIR::CoreType::Usize}) }));
                            builder.push_stmt_assign(cmp.clone(), MIR::RValue::make_BinOp({ idx.clone(), MIR::eBinOp::EQ, MIR::Constant::make_Uint({size, HIR::CoreType::Usize}) }));
                            builder.terminate_block(MIR::Terminator::make_If({ cmp.clone(), 1, 2 }));
                            builder.ensure_open();
                        }
                    }
                    }
                TU_ARMA(Path, te) {
                    bool has_drop = false;
                    TU_MATCH_HDRA( (te.binding), {)
                    TU_ARMA(Unbound, pbe) throw "";
                    TU_ARMA(Opaque, pbe) throw "";
                    TU_ARMA(ExternType, pbe) {
                        // Why is this trying to be dropped?
                        }

                    TU_ARMA(Struct, pbe) {
                        if( pbe->m_markings.has_drop_impl ) {
                            builder.push_CallDrop(ty);
                            has_drop = true;
                        }

                        if( ty.data().is_Path() && ty.data().as_Path().is_generator() ) {
                            ASSERT_BUG(sp, has_drop, "");
                            // Generators use a custom Drop impl that handles dropping values
                        }
                        else {
                            // NOTE: Lazy option of monomorphising and handling the two classes
                            const auto* repr = Target_GetTypeRepr(sp, state.resolve, ty);
                            ASSERT_BUG(sp, repr, "No repr for struct " << ty);

                            auto self = ::MIR::LValue::new_Deref(builder.self.clone());
                            auto fld_lv = ::MIR::LValue::new_Field(mv$(self), 0);
                            for(size_t i = 0; i < repr->fields.size(); i++)
                            {
                                if( state.resolve.type_needs_drop_glue(sp, repr->fields[i].ty) )
                                {
                                    builder.push_stmt_drop(fld_lv.clone());
                                }
                                fld_lv.inc_Field();
                            }
                        }
                        }
                    TU_ARMA(Union, pbe) {
                        if( pbe->m_markings.has_drop_impl ) {
                            builder.push_CallDrop(ty);
                            has_drop = true;
                        }
                        // Union requires no internal drop glue
                        }
                    TU_ARMA(Enum, pbe) {
                        if( pbe->m_markings.has_drop_impl ) {
                            builder.push_CallDrop(ty);
                            has_drop = true;
                        }
                        const HIR::Enum& enm = *pbe;
                        TU_MATCH_HDRA( (enm.m_data), {)
                        TU_ARMA(Value, ee) {
                            builder.terminate_block( MIR::Terminator::make_Return({}) );
                            }
                        TU_ARMA(Data, variants) {
                            auto self = ::MIR::LValue::new_Deref(builder.self.clone());
                            MIR::Terminator::Data_Switch sw;
                            sw.val = self.clone();
                            for(size_t idx = 0; idx < variants.size(); idx ++) {
                                sw.targets.push_back(builder.mir.blocks.size() + idx);
                            }
                            builder.terminate_block(MIR::Terminator::make_Switch(mv$(sw)));

                            auto fld_lv = ::MIR::LValue::new_Downcast(mv$(self), 0);
                            for(size_t idx = 0; idx < variants.size(); idx ++)
                            {
                                // TODO: Monomorphise and check
                                //if( state.resolve.type_needs_drop_glue(sp, repr->fields[i].ty) )
                                {
                                    builder.push_stmt_drop(fld_lv.clone());
                                }
                                fld_lv.inc_Downcast();
                                builder.ensure_open();
                                builder.terminate_block( MIR::Terminator::make_Return({}) );
                            }
                            }
                        }
                        }
                    }
                    if( has_drop ) {
                        if( auto* e = trans_list.add_function( ::HIR::Path(ty.clone(), state.resolve.m_lang_Drop, "drop") ) )
                        {
                            MonomorphState  params;
                            auto p = ::HIR::Path(ty.clone(), state.resolve.m_lang_Drop, "drop");
                            auto fcn_e = state.resolve.get_value(sp, p, /*out*/params, /*signature_only=*/false);
                            ASSERT_BUG(sp, fcn_e.is_Function(), "Drop didn't point to a function! " << fcn_e.tag_str() << " " << p);
                            ASSERT_BUG(sp, !params.has_types(), "Generic drop impl encountered during auto_impls (should have been populated during enum)");
                            e->force_prototype = true;
                            e->ptr = fcn_e.as_Function();
                            //e->pp = mv$(params);
                        }
                    }
                    }
                }
            }
            if(builder.mir.blocks.back().terminator.is_Incomplete())
            {
                builder.terminate_block( MIR::Terminator::make_Return({}) );
            }

            MIR_Validate(state.resolve, HIR::ItemPath(path), *fcn.m_code.m_mir, fcn.m_args, fcn.m_return);
            trans_list.m_auto_functions.push_back(box$(fcn));
            auto* e = trans_list.add_function(mv$(path));
            e->ptr = trans_list.m_auto_functions.back().get();

        }
    }
}

