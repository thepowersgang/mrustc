/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/cleanup.cpp
 * - MIR Cleanup
 *
 * Removes artefacts left after monomorphisation
 * - Converts <Trait as Trait>::method() into a vtable call
 * - Replaces constants by their value
 */
#include "main_bindings.hpp"
#include "mir.hpp"
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>
#include <mir/helpers.hpp>
#include <mir/operations.hpp>
#include <mir/visit_crate_mir.hpp>

struct MirMutator
{
    ::MIR::Function& m_fcn;
    unsigned int    cur_block;
    unsigned int    cur_stmt;
    mutable ::std::vector< ::MIR::Statement>    new_statements;

    MirMutator(::MIR::Function& fcn, unsigned int bb, unsigned int stmt):
        m_fcn(fcn),
        cur_block(bb), cur_stmt(stmt)
    {
    }

    ::MIR::LValue new_temporary(::HIR::TypeRef ty)
    {
        auto rv = ::MIR::LValue::make_Temporary({ static_cast<unsigned int>(m_fcn.temporaries.size()) });
        m_fcn.temporaries.push_back( mv$(ty) );
        return rv;
    }

    void push_statement(::MIR::Statement stmt)
    {
        new_statements.push_back( mv$(stmt) );
    }

    ::MIR::LValue in_temporary(::HIR::TypeRef ty, ::MIR::RValue val)
    {
        auto rv = this->new_temporary( mv$(ty) );
        push_statement( ::MIR::Statement::make_Assign({ rv.clone(), mv$(val) }) );
        return rv;
    }

    decltype(new_statements.begin()) flush()
    {
        DEBUG("flush - " << cur_block << "/" << cur_stmt);
        auto& block = m_fcn.blocks.at(cur_block);
        assert( cur_stmt <= block.statements.size() );
        auto it = block.statements.begin() + cur_stmt;
        for(auto& stmt : new_statements)
        {
            DEBUG("- Push stmt @" << cur_stmt << " (size=" << block.statements.size() + 1 << ")");
            it = block.statements.insert(it, mv$(stmt));
            ++ it;
            cur_stmt += 1;
        }
        new_statements.clear();
        return it;
    }
};

const ::HIR::Literal* MIR_Cleanup_GetConstant(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::Path& path,  ::HIR::TypeRef& out_ty)
{
    TU_MATCHA( (path.m_data), (pe),
    (Generic,
        const auto& constant = resolve.m_crate.get_constant_by_path(sp, pe.m_path);
        if( pe.m_params.m_types.size() != 0 )
            TODO(sp, "Generic constants - " << path);
        out_ty = constant.m_type.clone();
        if( constant.m_value_res.is_Invalid() )
            return nullptr;
        return &constant.m_value_res;
        ),
    (UfcsUnknown,
        ),
    (UfcsKnown,
        ),
    (UfcsInherent,
        )
    )
    return nullptr;
}

::MIR::RValue MIR_Cleanup_LiteralToRValue(const ::MIR::TypeResolve& state, MirMutator& mutator, const ::HIR::Literal& lit, ::HIR::TypeRef ty, ::HIR::Path path)
{
    TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
    (
        if( path == ::HIR::GenericPath() )
            MIR_TODO(state, "Literal of type " << ty << " - " << path << " - " << lit);
        return ::MIR::Constant( mv$(path) );
        ),
    (Tuple,
        MIR_ASSERT(state, lit.is_List(), "Non-list literal for Tuple - " << lit);
        const auto& vals = lit.as_List();
        MIR_ASSERT(state, vals.size() == te.size(), "Literal size mismatched with tuple size");

        ::std::vector< ::MIR::LValue>   lvals;
        lvals.reserve( vals.size() );

        for(unsigned int i = 0; i < vals.size(); i ++)
        {
            auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, vals[i], te[i].clone(), ::HIR::GenericPath());
            lvals.push_back( mutator.in_temporary( mv$(te[i]), mv$(rval)) );
        }

        return ::MIR::RValue::make_Tuple({ mv$(lvals) });
        ),
    (Array,
        MIR_ASSERT(state, lit.is_List(), "Non-list literal for Array - " << lit);
        const auto& vals = lit.as_List();

        MIR_ASSERT(state, vals.size() == te.size_val, "Literal size mismatched with array size");

        bool is_all_same = false;
        if( vals.size() > 1 )
        {
            is_all_same = true;
            for(unsigned int i = 1; i < vals.size(); i ++) {

                if( vals[i] != vals[0] ) {
                    is_all_same = false;
                    break ;
                }
            }
        }

        if( is_all_same )
        {
            auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, vals[0], te.inner->clone(), ::HIR::GenericPath());
            auto data_lval = mutator.in_temporary(te.inner->clone(), mv$(rval));
            return ::MIR::RValue::make_SizedArray({ mv$(data_lval), static_cast<unsigned int>(te.size_val) });
        }
        else
        {
            ::std::vector< ::MIR::LValue>   lvals;
            lvals.reserve( vals.size() );

            for(const auto& val: vals)
            {
                auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, val, te.inner->clone(), ::HIR::GenericPath());
                lvals.push_back( mutator.in_temporary(te.inner->clone(), mv$(rval)) );
            }

            return ::MIR::RValue::make_Array({ mv$(lvals) });
        }
        ),
    (Path,
        if( te.binding.is_Struct() )
        {
            const auto& str = *te.binding.as_Struct();
            const auto& vals = lit.as_List();

            auto monomorph = [&](const auto& tpl) { return monomorphise_type(state.sp, str.m_params, te.path.m_data.as_Generic().m_params, tpl); };

            ::std::vector< ::MIR::LValue>   lvals;
            TU_MATCHA( (str.m_data), (se),
            (Unit,
                MIR_ASSERT(state, vals.size() == 0, "Values passed for unit struct");
                ),
            (Tuple,
                MIR_ASSERT(state, vals.size() == se.size(), "Value count mismatch in literal for " << ty << " - exp " << se.size() << ", " << lit);
                for(unsigned int i = 0; i < se.size(); i ++)
                {
                    auto ent_ty = monomorph(se[i].ent);
                    auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, vals[i], ent_ty.clone(), ::HIR::GenericPath());
                    lvals.push_back( mutator.in_temporary(mv$(ent_ty), mv$(rval)) );
                }
                ),
            (Named,
                MIR_ASSERT(state, vals.size() == se.size(), "Value count mismatch in literal for " << ty << " - exp " << se.size() << ", " << lit);
                for(unsigned int i = 0; i < se.size(); i ++)
                {
                    auto ent_ty = monomorph(se[i].second.ent);
                    auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, vals[i], ent_ty.clone(), ::HIR::GenericPath());
                    lvals.push_back( mutator.in_temporary(mv$(ent_ty), mv$(rval)) );
                }
                )
            )
            return ::MIR::RValue::make_Struct({ te.path.m_data.as_Generic().clone(), ~0u, mv$(lvals) });
        }
        else if( te.binding.is_Enum() )
        {
            const auto& enm = *te.binding.as_Enum();
            const auto& lit_var = lit.as_Variant();

            auto monomorph = [&](const auto& tpl) { return monomorphise_type(state.sp, enm.m_params, te.path.m_data.as_Generic().m_params, tpl); };

            ::std::vector< ::MIR::LValue>   lvals;
            MIR_ASSERT(state, lit_var.idx < enm.m_variants.size(), "Variant index out of range");
            TU_MATCHA( (enm.m_variants[lit_var.idx].second), (ve),
            (Unit,
                ),
            (Value,
                ),
            (Tuple,
                MIR_ASSERT(state, lit_var.vals.size() == ve.size(), "Value count mismatch in literal for " << ty << " #" << lit_var.idx << " - exp " << ve.size() << ", " << lit);
                for(unsigned int i = 0; i < ve.size(); i ++)
                {
                    auto ent_ty = monomorph(ve[i].ent);
                    auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, lit_var.vals[i], ent_ty.clone(), ::HIR::GenericPath());
                    lvals.push_back( mutator.in_temporary(mv$(ent_ty), mv$(rval)) );
                }
                ),
            (Struct,
                MIR_ASSERT(state, lit_var.vals.size() == ve.size(), "Value count mismatch in literal for " << ty << " #" << lit_var.idx << " - exp " << ve.size() << ", " << lit);
                for(unsigned int i = 0; i < ve.size(); i ++)
                {
                    auto ent_ty = monomorph(ve[i].second.ent);
                    auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, lit_var.vals[i], ent_ty.clone(), ::HIR::GenericPath());
                    lvals.push_back( mutator.in_temporary(mv$(ent_ty), mv$(rval)) );
                }
                )
            )
            return ::MIR::RValue::make_Struct({ te.path.m_data.as_Generic().clone(), lit_var.idx, mv$(lvals) });
        }
        else
        {
            MIR_BUG(state, "Unexpected type - " << ty);
        }
        ),
    (Primitive,
        switch(te)
        {
        case ::HIR::CoreType::Char:
        case ::HIR::CoreType::Usize:
        case ::HIR::CoreType::U64:
        case ::HIR::CoreType::U32:
        case ::HIR::CoreType::U16:
        case ::HIR::CoreType::U8:
            return ::MIR::Constant::make_Uint( lit.as_Integer() );
        case ::HIR::CoreType::Isize:
        case ::HIR::CoreType::I64:
        case ::HIR::CoreType::I32:
        case ::HIR::CoreType::I16:
        case ::HIR::CoreType::I8:
            return ::MIR::Constant::make_Int( lit.as_Integer() );
        case ::HIR::CoreType::F64:
        case ::HIR::CoreType::F32:
            return ::MIR::Constant::make_Float( lit.as_Float() );
        case ::HIR::CoreType::Bool:
            return ::MIR::Constant::make_Bool( !!lit.as_Integer() );
        case ::HIR::CoreType::Str:
            MIR_BUG(state, "Const of type `str` - " << path);
        }
        throw "";
        ),
    (Pointer,
        if( lit.is_BorrowOf() ) {
            // TODO:
            MIR_TODO(state, "BorrowOf into pointer - " << lit << " into " << ty);
        }
        else {
            auto lval = mutator.in_temporary( ::HIR::CoreType::Usize, ::MIR::RValue( ::MIR::Constant::make_Uint( lit.as_Integer() ) ) );
            return ::MIR::RValue::make_Cast({ mv$(lval), mv$(ty) });
        }
        ),
    (Borrow,
        if( lit.is_BorrowOf() )
        {
            const auto& path = lit.as_BorrowOf();
            if( te.inner->m_data.is_Slice() )
            {
                ::HIR::TypeRef tmp;
                const auto& ty = state.get_static_type(tmp, path);
                MIR_ASSERT(state, ty.m_data.is_Array(), "BorrowOf returning slice not of an array, instead " << ty);
                unsigned int size = ty.m_data.as_Array().size_val;

                auto ptr_type = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared,
                    (&ty == &tmp ? mv$(tmp) : ty.clone())
                    );

                auto ptr_lval = mutator.in_temporary( mv$(ptr_type), ::MIR::Constant::make_ItemAddr(path.clone()) );
                auto size_lval = mutator.in_temporary( ::HIR::CoreType::Usize, ::MIR::Constant::make_Uint(size) );
                return ::MIR::RValue::make_MakeDst({ mv$(ptr_lval), mv$(size_lval) });
            }
            else
            {
                return ::MIR::Constant::make_ItemAddr( path.clone() );
            }
        }
        else if( te.inner->m_data.is_Slice() && *te.inner->m_data.as_Slice().inner == ::HIR::CoreType::U8 ) {
            ::std::vector<uint8_t>  bytestr;
            for(auto v : lit.as_String())
                bytestr.push_back( static_cast<uint8_t>(v) );
            return ::MIR::Constant::make_Bytes( mv$(bytestr) );
        }
        else if( te.inner->m_data.is_Array() && *te.inner->m_data.as_Array().inner == ::HIR::CoreType::U8 ) {
            // TODO: How does this differ at codegen to the above?
            ::std::vector<uint8_t>  bytestr;
            for(auto v : lit.as_String())
                bytestr.push_back( static_cast<uint8_t>(v) );
            return ::MIR::Constant::make_Bytes( mv$(bytestr) );
        }
        else if( *te.inner == ::HIR::CoreType::Str ) {
            return ::MIR::Constant::make_StaticString( lit.as_String() );
        }
        else {
            MIR_TODO(state, "Const with type " << ty);
        }
        )
    )
}

::MIR::LValue MIR_Cleanup_Virtualize(
    const Span& sp, const ::MIR::TypeResolve& state, MirMutator& mutator,
    ::MIR::LValue& receiver_lvp,
    const ::HIR::TypeRef::Data::Data_TraitObject& te, const ::HIR::Path::Data::Data_UfcsKnown& pe
    )
{
    assert( te.m_trait.m_trait_ptr );
    const auto& trait = *te.m_trait.m_trait_ptr;

    // 1. Get the vtable index for this function
    auto it = trait.m_value_indexes.find( pe.item );
    while( it != trait.m_value_indexes.end() )
    {
        DEBUG("- " << it->second.second);
        if( it->second.second.m_path == pe.trait.m_path )
        {
            // TODO: Match generics using match_test_generics comparing to the trait args
            break ;
        }
        ++ it;
    }
    if( it == trait.m_value_indexes.end() || it->first != pe.item )
        BUG(sp, "Calling method '" << pe.item << "' from " << pe.trait << " through " << te.m_trait.m_path << " which isn't in the vtable");
    unsigned int vtable_idx = it->second.first;

    // 2. Load from the vtable
    auto vtable_ty_spath = te.m_trait.m_path.m_path;
    vtable_ty_spath.m_components.back() += "#vtable";
    const auto& vtable_ref = state.m_resolve.m_crate.get_struct_by_path(sp, vtable_ty_spath);
    // Copy the param set from the trait in the trait object
    ::HIR::PathParams   vtable_params = te.m_trait.m_path.m_params.clone();
    // - Include associated types on bound
    for(const auto& ty_b : te.m_trait.m_type_bounds) {
        auto idx = trait.m_type_indexes.at(ty_b.first);
        if(vtable_params.m_types.size() <= idx)
            vtable_params.m_types.resize(idx+1);
        vtable_params.m_types[idx] = ty_b.second.clone();
    }
    auto vtable_ty = ::HIR::TypeRef::new_pointer(
        ::HIR::BorrowType::Shared,
        ::HIR::TypeRef( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref )
        );

    // Allocate a temporary for the vtable pointer itself
    auto vtable_lv = mutator.new_temporary( mv$(vtable_ty) );
    // - Load the vtable and store it
    auto vtable_rval = ::MIR::RValue::make_DstMeta({ ::MIR::LValue::make_Deref({ box$(receiver_lvp.clone()) }) });
    mutator.push_statement( ::MIR::Statement::make_Assign({ vtable_lv.clone(), mv$(vtable_rval) }) );

    auto fcn_lval = ::MIR::LValue::make_Field({ box$(::MIR::LValue::make_Deref({ box$(vtable_lv) })), vtable_idx });

    ::HIR::TypeRef  tmp;
    const auto& ty = state.get_lvalue_type(tmp, fcn_lval);
    const auto& receiver = ty.m_data.as_Function().m_arg_types.at(0);
    if( state.is_type_owned_box(receiver) )
    {
        // TODO: If the receiver is Box, create a Box<()> as the value.
        // - Requires de/restructuring the Box same as CoerceUnsized
        // - Can use the `coerce_unsized_index` field too

        struct H {
            static ::MIR::LValue get_unit_ptr(const ::MIR::TypeResolve& state, MirMutator& mutator, ::HIR::TypeRef ty, ::MIR::LValue lv)
            {
                if( ty.m_data.is_Path() )
                {
                    const auto& te = ty.m_data.as_Path();
                    MIR_ASSERT(state, te.binding.is_Struct(), "");
                    const auto& ty_path = te.path.m_data.as_Generic();
                    const auto& str = *te.binding.as_Struct();
                    ::HIR::TypeRef  tmp;
                    auto monomorph = [&](const auto& t) { return monomorphise_type(Span(), str.m_params, ty_path.m_params, t); };
                    ::std::vector< ::MIR::LValue>   vals;
                    TU_MATCHA( (str.m_data), (se),
                    (Unit,
                        ),
                    (Tuple,
                        for(unsigned int i = 0; i < se.size(); i ++ ) {
                            auto val = (i == se.size() - 1 ? mv$(lv) : lv.clone());
                            if( i == str.m_markings.coerce_unsized_index ) {
                                vals.push_back( H::get_unit_ptr(state, mutator, monomorph(se[i].ent), ::MIR::LValue::make_Field({ box$(val), i }) ) );
                            }
                            else {
                                vals.push_back( ::MIR::LValue::make_Field({ box$(val), i }) );
                            }
                        }
                        ),
                    (Named,
                        for(unsigned int i = 0; i < se.size(); i ++ ) {
                            auto val = (i == se.size() - 1 ? mv$(lv) : lv.clone());
                            if( i == str.m_markings.coerce_unsized_index ) {
                                vals.push_back( H::get_unit_ptr(state, mutator, monomorph(se[i].second.ent), ::MIR::LValue::make_Field({ box$(val), i }) ) );
                            }
                            else {
                                vals.push_back( ::MIR::LValue::make_Field({ box$(val), i }) );
                            }
                        }
                        )
                    )

                    auto new_path = ty_path.clone();
                    return mutator.in_temporary( mv$(ty), ::MIR::RValue::make_Struct({ mv$(new_path), ~0u, mv$(vals) }) );
                }
                else if( ty.m_data.is_Pointer() )
                {
                    return mutator.in_temporary(
                        ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, ::HIR::TypeRef::new_unit()),
                        ::MIR::RValue::make_DstPtr({ ::MIR::LValue::make_Deref({ box$(lv) }) })
                        );
                }
                else
                {
                    MIR_BUG(state, "Unexpected type coerce_unsize in Box - " << ty);
                }
            }
        };

        receiver_lvp = H::get_unit_ptr(state,mutator, receiver.clone(), receiver_lvp.clone());
    }
    else
    {
        auto ptr_rval = ::MIR::RValue::make_DstPtr({ ::MIR::LValue::make_Deref({ box$(receiver_lvp.clone()) }) });

        auto ptr_lv = mutator.new_temporary( ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, ::HIR::TypeRef::new_unit()) );
        mutator.push_statement( ::MIR::Statement::make_Assign({ ptr_lv.clone(), mv$(ptr_rval) }) );
        receiver_lvp = mv$(ptr_lv);
    }

    // Update the terminator with the new information.
    return fcn_lval;
}

bool MIR_Cleanup_Unsize_GetMetadata(const ::MIR::TypeResolve& state, MirMutator& mutator,
        const ::HIR::TypeRef& dst_ty, const ::HIR::TypeRef& src_ty, const ::MIR::LValue& ptr_value,
        ::MIR::RValue& out_meta_val, ::HIR::TypeRef& out_meta_ty, bool& out_src_is_dst
        )
{
    TU_MATCH_DEF(::HIR::TypeRef::Data, (dst_ty.m_data), (de),
    (
        MIR_TODO(state, "Obtain metadata converting to " << dst_ty);
        ),
    (Generic,
        // TODO: What should be returned to indicate "no conversion"
        return false;
        ),
    (Path,
        // Source must be Path and Unsize
        if( de.binding.is_Opaque() )
            return false;

        MIR_ASSERT(state, src_ty.m_data.is_Path(), "Unsize to path from non-path - " << src_ty);
        const auto& se = src_ty.m_data.as_Path();
        MIR_ASSERT(state, de.binding.tag() == se.binding.tag(), "Unsize between mismatched types - " << dst_ty << " and " << src_ty);
        MIR_ASSERT(state, de.binding.is_Struct(), "Unsize to non-struct - " << dst_ty);
        MIR_ASSERT(state, de.binding.as_Struct() == se.binding.as_Struct(), "Unsize between mismatched types - " << dst_ty << " and " << src_ty);
        const auto& str = *de.binding.as_Struct();
        MIR_ASSERT(state, str.m_markings.unsized_field != ~0u, "Unsize on type that doesn't implement have a ?Sized field - " << dst_ty);

        auto monomorph_cb_d = monomorphise_type_get_cb(state.sp, nullptr, &de.path.m_data.as_Generic().m_params, nullptr);
        auto monomorph_cb_s = monomorphise_type_get_cb(state.sp, nullptr, &se.path.m_data.as_Generic().m_params, nullptr);

        // Return GetMetadata on the inner type
        TU_MATCHA( (str.m_data), (se),
        (Unit,
            MIR_BUG(state, "Unit-like struct Unsize is impossible - " << src_ty);
            ),
        (Tuple,
            const auto& ty_tpl = se.at( str.m_markings.unsized_field ).ent;
            auto ty_d = monomorphise_type_with(state.sp, ty_tpl, monomorph_cb_d, false);
            auto ty_s = monomorphise_type_with(state.sp, ty_tpl, monomorph_cb_s, false);

            return MIR_Cleanup_Unsize_GetMetadata(state, mutator,  ty_d, ty_s, ptr_value,  out_meta_val,out_meta_ty,out_src_is_dst);
            ),
        (Named,
            const auto& ty_tpl = se.at( str.m_markings.unsized_field ).second.ent;
            auto ty_d = monomorphise_type_with(state.sp, ty_tpl, monomorph_cb_d, false);
            auto ty_s = monomorphise_type_with(state.sp, ty_tpl, monomorph_cb_s, false);

            return MIR_Cleanup_Unsize_GetMetadata(state, mutator,  ty_d, ty_s, ptr_value,  out_meta_val,out_meta_ty,out_src_is_dst);
            )
        )
        throw "";
        ),
    (Slice,
        // Source must be an array (or generic)
        if( src_ty.m_data.is_Array() )
        {
            const auto& in_array = src_ty.m_data.as_Array();
            out_meta_ty = ::HIR::CoreType::Usize;
            out_meta_val = ::MIR::Constant( static_cast<uint64_t>(in_array.size_val) );
            return true;
        }
        else if( src_ty.m_data.is_Generic() || (src_ty.m_data.is_Path() && src_ty.m_data.as_Path().binding.is_Opaque()) )
        {
            // HACK: FixedSizeArray uses `A: Unsize<[T]>` which will lead to the above code not working (as the size isn't known).
            // - Maybe _Meta on the `&A` would work as a stopgap (since A: Sized, it won't collide with &[T] or similar)

            return false;

            //out_meta_ty = ::HIR::CoreType::Usize;
            //out_meta_val = ::MIR::RValue::make_DstMeta({ ptr_value.clone() });
            //return true;
        }
        else
        {
            MIR_BUG(state, "Unsize to slice from non-array - " << src_ty);
        }
        ),
    (TraitObject,

        auto ty_unit_ptr = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, ::HIR::TypeRef::new_unit());

        // No data trait, vtable is a null unit pointer.
        // - Shouldn't the vtable be just unit?
        // - Codegen assumes it's a pointer.
        if( de.m_trait.m_path.m_path == ::HIR::SimplePath() )
        {
            auto null_lval = mutator.in_temporary( ::HIR::CoreType::Usize, ::MIR::Constant::make_Uint(0u) );
            out_meta_ty = ty_unit_ptr.clone();
            out_meta_val = ::MIR::RValue::make_Cast({ mv$(null_lval), mv$(ty_unit_ptr) });
        }
        else
        {
            const auto& trait_path = de.m_trait;
            const auto& trait = *de.m_trait.m_trait_ptr;

            // Obtain vtable type `::"path"::to::Trait#vtable`
            auto vtable_ty_spath = trait_path.m_path.m_path;
            vtable_ty_spath.m_components.back() += "#vtable";
            const auto& vtable_ref = state.m_crate.get_struct_by_path(state.sp, vtable_ty_spath);
            // Copy the param set from the trait in the trait object
            ::HIR::PathParams   vtable_params = trait_path.m_path.m_params.clone();
            // - Include associated types
            for(const auto& ty_b : trait_path.m_type_bounds) {
                auto idx = trait.m_type_indexes.at(ty_b.first);
                if(vtable_params.m_types.size() <= idx)
                    vtable_params.m_types.resize(idx+1);
                vtable_params.m_types[idx] = ty_b.second.clone();
            }
            auto vtable_type = ::HIR::TypeRef( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref );

            out_meta_ty = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, mv$(vtable_type));

            // If the data trait hasn't changed, return the vtable pointer
            if( src_ty.m_data.is_TraitObject() )
            {
                out_src_is_dst = true;
                auto deref_ptr_val = ::MIR::LValue::make_Deref({ box$(ptr_value.clone()) });
                out_meta_val = ::MIR::RValue::make_DstMeta({ mv$(deref_ptr_val) });
            }
            else
            {
                ::HIR::Path vtable { src_ty.clone(), trait_path.m_path.clone(), "#vtable" };
                out_meta_val = ::MIR::RValue( ::MIR::Constant::make_ItemAddr(mv$(vtable)) );
            }
        }
        return true;
        )
    )
}

::MIR::RValue MIR_Cleanup_Unsize(const ::MIR::TypeResolve& state, MirMutator& mutator, const ::HIR::TypeRef& dst_ty, const ::HIR::TypeRef& src_ty_inner, ::MIR::LValue ptr_value)
{
    const auto& dst_ty_inner = (dst_ty.m_data.is_Borrow() ? *dst_ty.m_data.as_Borrow().inner : *dst_ty.m_data.as_Pointer().inner);

    ::HIR::TypeRef  meta_type;
    ::MIR::RValue   meta_value;
    bool source_is_dst = false;
    if( MIR_Cleanup_Unsize_GetMetadata(state, mutator, dst_ty_inner, src_ty_inner, ptr_value,  meta_value, meta_type, source_is_dst) )
    {
        // TODO: There is a case where the source is already a fat pointer. In that case the pointer of the new DST must be the source DST pointer
        auto meta_lval = mutator.in_temporary( mv$(meta_type), mv$(meta_value) );
        if( source_is_dst )
        {
            auto ty_unit_ptr = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, ::HIR::TypeRef::new_unit());
            auto deref_ptr_val = ::MIR::LValue::make_Deref({ box$(ptr_value) });
            auto thin_ptr_lval = mutator.in_temporary( mv$(ty_unit_ptr), ::MIR::RValue::make_DstPtr({ mv$(deref_ptr_val) }) );

            return ::MIR::RValue::make_MakeDst({ mv$(thin_ptr_lval), mv$(meta_lval) });
        }
        else
        {
            return ::MIR::RValue::make_MakeDst({ mv$(ptr_value), mv$(meta_lval) });
        }
    }
    else
    {
        // Emit a cast rvalue, as something is still generic.
        return ::MIR::RValue::make_Cast({ mv$(ptr_value), dst_ty.clone() });
    }
}

::MIR::RValue MIR_Cleanup_CoerceUnsized(const ::MIR::TypeResolve& state, MirMutator& mutator, const ::HIR::TypeRef& dst_ty, const ::HIR::TypeRef& src_ty, ::MIR::LValue value)
{
    TRACE_FUNCTION_F(dst_ty << " <- " << src_ty << " ( " << value << " )");
    //  > Path -> Path = Unsize
    // (path being destination is otherwise invalid)
    if( dst_ty.m_data.is_Path() )
    {
        MIR_ASSERT(state, src_ty.m_data.is_Path(), "CoerceUnsized to Path must have a Path source - " << src_ty << " to " << dst_ty);
        const auto& dte = dst_ty.m_data.as_Path();
        const auto& ste = src_ty.m_data.as_Path();

        // - Types must differ only by a single field, and be from the same definition
        MIR_ASSERT(state, dte.binding.is_Struct(), "Note, can't CoerceUnsized non-structs");
        MIR_ASSERT(state, dte.binding.tag() == ste.binding.tag(),
            "Note, can't CoerceUnsized mismatched structs - " << src_ty << " to " << dst_ty);
        MIR_ASSERT(state, dte.binding.as_Struct() == ste.binding.as_Struct(),
            "Note, can't CoerceUnsized mismatched structs - " << src_ty << " to " << dst_ty);
        const auto& str = *dte.binding.as_Struct();
        MIR_ASSERT(state, str.m_markings.coerce_unsized_index != ~0u,
            "Struct " << src_ty << " doesn't impl CoerceUnsized");

        auto monomorph_cb_d = monomorphise_type_get_cb(state.sp, nullptr, &dte.path.m_data.as_Generic().m_params, nullptr);
        auto monomorph_cb_s = monomorphise_type_get_cb(state.sp, nullptr, &ste.path.m_data.as_Generic().m_params, nullptr);

        // - Destructure and restrucure with the unsized fields
        ::std::vector<::MIR::LValue>    ents;
        TU_MATCHA( (str.m_data), (se),
        (Unit,
            MIR_BUG(state, "Unit-like struct CoerceUnsized is impossible - " << src_ty);
            ),
        (Tuple,
            ents.reserve( se.size() );
            for(unsigned int i = 0; i < se.size(); i++)
            {
                if( i == str.m_markings.coerce_unsized_index )
                {
                    auto ty_d = monomorphise_type_with(state.sp, se[i].ent, monomorph_cb_d, false);
                    auto ty_s = monomorphise_type_with(state.sp, se[i].ent, monomorph_cb_s, false);

                    auto new_rval = MIR_Cleanup_CoerceUnsized(state, mutator, ty_d, ty_s,  ::MIR::LValue::make_Field({ box$(value.clone()), i }));
                    auto new_lval = mutator.in_temporary( mv$(ty_d), mv$(new_rval) );

                    ents.push_back( mv$(new_lval) );
                }
                else if( state.m_resolve.is_type_phantom_data( se[i].ent ) )
                {
                    auto ty_d = monomorphise_type_with(state.sp, se[i].ent, monomorph_cb_d, false);

                    auto new_rval = ::MIR::RValue::make_Cast({ ::MIR::LValue::make_Field({ box$(value.clone()), i }), ty_d.clone() });
                    auto new_lval = mutator.in_temporary( mv$(ty_d), mv$(new_rval) );

                    ents.push_back( mv$(new_lval) );
                }
                else
                {
                    ents.push_back( ::MIR::LValue::make_Field({ box$(value.clone()), i}) );
                }
            }
            ),
        (Named,
            ents.reserve( se.size() );
            for(unsigned int i = 0; i < se.size(); i++)
            {
                if( i == str.m_markings.coerce_unsized_index )
                {
                    auto ty_d = monomorphise_type_with(state.sp, se[i].second.ent, monomorph_cb_d, false);
                    auto ty_s = monomorphise_type_with(state.sp, se[i].second.ent, monomorph_cb_s, false);

                    auto new_rval = MIR_Cleanup_CoerceUnsized(state, mutator, ty_d, ty_s,  ::MIR::LValue::make_Field({ box$(value.clone()), i }));
                    auto new_lval = mutator.new_temporary( mv$(ty_d) );
                    mutator.push_statement( ::MIR::Statement::make_Assign({ new_lval.clone(), mv$(new_rval) }) );

                    ents.push_back( mv$(new_lval) );
                }
                else if( state.m_resolve.is_type_phantom_data( se[i].second.ent ) )
                {
                    auto ty_d = monomorphise_type_with(state.sp, se[i].second.ent, monomorph_cb_d, false);

                    auto new_rval = ::MIR::RValue::make_Cast({ ::MIR::LValue::make_Field({ box$(value.clone()), i }), ty_d.clone() });
                    auto new_lval = mutator.in_temporary( mv$(ty_d), mv$(new_rval) );

                    ents.push_back( mv$(new_lval) );
                }
                else
                {
                    ents.push_back( ::MIR::LValue::make_Field({ box$(value.clone()), i}) );
                }
            }
            )
        )
        return ::MIR::RValue::make_Struct({ dte.path.m_data.as_Generic().clone(), ~0u, mv$(ents) });
    }

    if( dst_ty.m_data.is_Borrow() )
    {
        MIR_ASSERT(state, src_ty.m_data.is_Borrow(), "CoerceUnsized to Borrow must have a Borrow source - " << src_ty << " to " << dst_ty);
        const auto& ste = src_ty.m_data.as_Borrow();

        return MIR_Cleanup_Unsize(state, mutator, dst_ty, *ste.inner, mv$(value));
    }

    // Pointer Coercion - Downcast and unsize
    if( dst_ty.m_data.is_Pointer() )
    {
        MIR_ASSERT(state, src_ty.m_data.is_Pointer(), "CoerceUnsized to Pointer must have a Pointer source - " << src_ty << " to " << dst_ty);
        const auto& dte = dst_ty.m_data.as_Pointer();
        const auto& ste = src_ty.m_data.as_Pointer();

        if( dte.type == ste.type )
        {
            // TODO: Use unsize code above
            return MIR_Cleanup_Unsize(state, mutator, dst_ty, *ste.inner, mv$(value));
        }
        else
        {
            MIR_ASSERT(state, *dte.inner == *ste.inner, "TODO: Can pointer CoerceUnsized unsize? " << src_ty << " to " << dst_ty);
            MIR_ASSERT(state, dte.type < ste.type, "CoerceUnsize attempting to raise pointer type");

            return ::MIR::RValue::make_Cast({ mv$(value), dst_ty.clone() });
        }
    }

    MIR_BUG(state, "Unknown CoerceUnsized target " << dst_ty << " from " << src_ty);
    throw "";
}

void MIR_Cleanup_LValue(const ::MIR::TypeResolve& state, MirMutator& mutator, ::MIR::LValue& lval)
{
    TU_MATCHA( (lval), (le),
    (Variable,
        ),
    (Temporary,
        ),
    (Argument,
        ),
    (Static,
        ),
    (Return,
        ),
    (Field,
        MIR_Cleanup_LValue(state, mutator,  *le.val);
        ),
    (Deref,
        MIR_Cleanup_LValue(state, mutator,  *le.val);
        ),
    (Index,
        MIR_Cleanup_LValue(state, mutator,  *le.val);
        MIR_Cleanup_LValue(state, mutator,  *le.idx);
        ),
    (Downcast,
        MIR_Cleanup_LValue(state, mutator,  *le.val);
        )
    )

    // If this is a deref of Box, unpack and deref the inner pointer
    if( lval.is_Deref() )
    {
        auto& le = lval.as_Deref();
        ::HIR::TypeRef  tmp;
        const auto& ty = state.get_lvalue_type(tmp, *le.val);
        if( state.m_resolve.is_type_owned_box(ty) )
        {
            // Handle Box by extracting it to its pointer.
            // - Locate (or remember) which field in Box is the pointer, and replace the inner by that field
            // > Dumb idea, assume it's always the first field. Keep accessing until located.

            const auto* typ = &ty;
            while( typ->m_data.is_Path() )
            {
                const auto& te = typ->m_data.as_Path();
                MIR_ASSERT(state, te.binding.is_Struct(), "Box contained a non-struct");
                const auto& str = *te.binding.as_Struct();
                const ::HIR::TypeRef* ty_tpl = nullptr;
                TU_MATCHA( (str.m_data), (se),
                (Unit,
                    MIR_BUG(state, "Box contained a unit-like struct");
                    ),
                (Tuple,
                    MIR_ASSERT(state, se.size() > 0, "Box contained an empty tuple struct");
                    ty_tpl = &se[0].ent;
                    ),
                (Named,
                    MIR_ASSERT(state, se.size() > 0, "Box contained an empty named struct");
                    ty_tpl = &se[0].second.ent;
                    )
                )
                tmp = monomorphise_type(state.sp, str.m_params, te.path.m_data.as_Generic().m_params, *ty_tpl);
                typ = &tmp;

                auto new_lval = ::MIR::LValue::make_Field({ mv$(le.val), 0 });
                le.val = box$(new_lval);
            }
            MIR_ASSERT(state, typ->m_data.is_Pointer(), "First non-path field in Box wasn't a pointer - " << *typ);
            // We have reached the pointer. Good.
        }
    }
}

void MIR_Cleanup(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    Span    sp;
    TRACE_FUNCTION_F(path);
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };

    MirMutator  mutator { fcn, 0, 0 };
    for(auto& block : fcn.blocks)
    {
        for(auto it = block.statements.begin(); it != block.statements.end(); ++ it)
        {
            state.set_cur_stmt( mutator.cur_block, mutator.cur_stmt );
            auto& stmt = *it;

            // 1. Visit all LValues for box deref hackery
            TU_MATCHA( (stmt), (se),
            (Drop,
                MIR_Cleanup_LValue(state, mutator,  se.slot);
                ),
            (Asm,
                for(auto& v : se.inputs)
                    MIR_Cleanup_LValue(state, mutator,  v.second);
                for(auto& v : se.outputs)
                    MIR_Cleanup_LValue(state, mutator,  v.second);
                ),
            (Assign,
                MIR_Cleanup_LValue(state, mutator,  se.dst);
                TU_MATCHA( (se.src), (re),
                (Use,
                    MIR_Cleanup_LValue(state, mutator,  re);
                    ),
                (Constant,
                    ),
                (SizedArray,
                    MIR_Cleanup_LValue(state, mutator,  re.val);
                    ),
                (Borrow,
                    MIR_Cleanup_LValue(state, mutator,  re.val);
                    ),
                (Cast,
                    MIR_Cleanup_LValue(state, mutator,  re.val);
                    ),
                (BinOp,
                    MIR_Cleanup_LValue(state, mutator,  re.val_l);
                    MIR_Cleanup_LValue(state, mutator,  re.val_r);
                    ),
                (UniOp,
                    MIR_Cleanup_LValue(state, mutator,  re.val);
                    ),
                (DstMeta,
                    MIR_Cleanup_LValue(state, mutator,  re.val);
                    ),
                (DstPtr,
                    MIR_Cleanup_LValue(state, mutator,  re.val);
                    ),
                (MakeDst,
                    MIR_Cleanup_LValue(state, mutator,  re.ptr_val);
                    MIR_Cleanup_LValue(state, mutator,  re.meta_val);
                    ),
                (Tuple,
                    for(auto& lv : re.vals)
                        MIR_Cleanup_LValue(state, mutator,  lv);
                    ),
                (Array,
                    for(auto& lv : re.vals)
                        MIR_Cleanup_LValue(state, mutator,  lv);
                    ),
                (Variant,
                    MIR_Cleanup_LValue(state, mutator,  re.val);
                    ),
                (Struct,
                    for(auto& lv : re.vals)
                        MIR_Cleanup_LValue(state, mutator,  lv);
                    )
                )
                )
            )

            // 2. RValue conversions
            if( stmt.is_Assign() )
            {
                auto& se = stmt.as_Assign();

                TU_IFLET( ::MIR::RValue, se.src, Constant, e,
                    // TODO: Replace `Const` with actual values
                    TU_IFLET( ::MIR::Constant, e, Const, ce,
                        // 1. Find the constant
                        ::HIR::TypeRef  ty;
                        const auto* lit_ptr = MIR_Cleanup_GetConstant(sp, resolve, ce.p, ty);
                        if( lit_ptr )
                        {
                            se.src = MIR_Cleanup_LiteralToRValue(state, mutator, *lit_ptr, mv$(ty), mv$(ce.p));
                        }
                    )
                )

                if( se.src.is_Borrow() && se.src.as_Borrow().val.is_Field() )
                {
                    auto& e = se.src.as_Borrow();
                    // TODO: If borrowing a !Sized value via a field access, create the DST
                    ::HIR::TypeRef  tmp;
                    const auto& src_ty = state.get_lvalue_type(tmp, e.val);

                    if( !resolve.type_is_sized(sp, src_ty) && !src_ty.m_data.is_Generic() )
                    {
                        auto ty_unit_ptr = ::HIR::TypeRef::new_borrow( e.type, ::HIR::TypeRef::new_unit() );
                        if( se.dst.is_Temporary() && state.get_lvalue_type(tmp, se.dst) == ty_unit_ptr )
                        {
                            // Assigning fat pointer to a &() temporary, only happens if this has already happend
                        }
                        else
                        {
                            // Unwrap field accesses, next lvalue should be a deref
                            const ::MIR::LValue* lv = &e.val;
                            while( lv->is_Field() )
                                lv = &*lv->as_Field().val;
                            MIR_ASSERT(state, lv->is_Deref(), "Access of !Sized field not via a deref");

                            const auto& dst_val_lval = *lv;

                            auto meta_rval = ::MIR::RValue::make_DstMeta({ dst_val_lval.clone() });
                            // TODO: How can the thin pointer to the field be obtained without tripping this twice?
                            auto ptr_rval = mv$( se.src );

                            // TODO: Get the metadata type.
                            auto meta_ty = ::HIR::TypeRef( ::HIR::CoreType::Usize );
                            auto meta_lval = mutator.in_temporary( mv$(meta_ty), mv$(meta_rval) );

                            // HACK: Store the pointer as &()
                            auto ptr_lval = mutator.in_temporary( mv$(ty_unit_ptr), mv$(ptr_rval) );
                            se.src = ::MIR::RValue::make_MakeDst({ mv$(ptr_lval), mv$(meta_lval) });
                        }
                    }
                }

                if( se.src.is_Cast() )
                {
                    auto& e = se.src.as_Cast();
                    ::HIR::TypeRef  tmp;
                    const auto& src_ty = state.get_lvalue_type(tmp, e.val);
                    // TODO: Unsize and CoerceUnsized operations
                    // - Unsize should create a fat pointer if the pointer class is known (vtable or len)
                    TU_IFLET( ::HIR::TypeRef::Data, e.type.m_data, Borrow, te,
                        //  > & -> & = Unsize, create DST based on the pointer class of the destination.
                        // (&-ptr being destination is otherwise invalid)
                        // TODO Share with the CoerceUnsized handling?
                        se.src = MIR_Cleanup_CoerceUnsized(state, mutator, e.type, src_ty, mv$(e.val));
                    )
                    // Casts to PhantomData are only valid from PhandomData, and are added by _CoerceUnsized
                    else if( state.m_resolve.is_type_phantom_data(e.type) )
                    {
                        // Leave
                        MIR_ASSERT(state, state.m_resolve.is_type_phantom_data(src_ty) != nullptr, "PhandomData can only cast from PhantomData");
                    }
                    // - CoerceUnsized should re-create the inner type if known.
                    else TU_IFLET( ::HIR::TypeRef::Data, e.type.m_data, Path, te,
                        TU_IFLET( ::HIR::TypeRef::Data, src_ty.m_data, Path, ste,
                            ASSERT_BUG( sp, ! te.binding.is_Unbound(), "" );
                            ASSERT_BUG( sp, !ste.binding.is_Unbound(), "" );
                            if( te.binding.is_Opaque() || ste.binding.is_Opaque() ) {
                                // Either side is opaque, leave for now
                            }
                            else {
                                se.src = MIR_Cleanup_CoerceUnsized(state, mutator, e.type, src_ty, mv$(e.val));
                            }
                        )
                        else {
                            ASSERT_BUG( sp, src_ty.m_data.is_Generic(), "Cast to Path from " << src_ty );
                        }
                    )
                    else {
                    }
                }
            }

            DEBUG(it - block.statements.begin());
            it = mutator.flush();
            DEBUG(it - block.statements.begin());
            mutator.cur_stmt += 1;
        }

        state.set_cur_stmt_term( mutator.cur_block );

        TU_MATCHA( (block.terminator), (e),
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
            MIR_Cleanup_LValue(state, mutator, e.cond);
            ),
        (Switch,
            MIR_Cleanup_LValue(state, mutator, e.val);
            ),
        (Call,
            MIR_Cleanup_LValue(state, mutator, e.ret_val);
            if( e.fcn.is_Value() ) {
                MIR_Cleanup_LValue(state, mutator, e.fcn.as_Value());
            }
            for(auto& lv : e.args)
                MIR_Cleanup_LValue(state, mutator, lv);
            )
        )

        TU_IFLET( ::MIR::Terminator, block.terminator, Call, e,

            TU_IFLET( ::MIR::CallTarget, e.fcn, Path, path,
                // Detect calling `<Trait as Trait>::method()` and replace with vtable call
                if( path.m_data.is_UfcsKnown() && path.m_data.as_UfcsKnown().type->m_data.is_TraitObject() )
                {
                    const auto& pe = path.m_data.as_UfcsKnown();
                    const auto& te = pe.type->m_data.as_TraitObject();
                    // TODO: What if the method is from a supertrait?

                    if( te.m_trait.m_path == pe.trait || resolve.find_named_trait_in_trait(
                            sp, pe.trait.m_path, pe.trait.m_params,
                            *te.m_trait.m_trait_ptr, te.m_trait.m_path.m_path, te.m_trait.m_path.m_params,
                            *pe.type,
                            [](const auto&, auto){}
                            )
                        )
                    {
                        auto tgt_lvalue = MIR_Cleanup_Virtualize(sp, state, mutator, e.args.front(), te, pe);
                        e.fcn = mv$(tgt_lvalue);
                    }
                }

                if( path.m_data.is_UfcsKnown() && path.m_data.as_UfcsKnown().type->m_data.is_Function() )
                {
                    const auto& pe = path.m_data.as_UfcsKnown();
                    const auto& fcn_ty = pe.type->m_data.as_Function();
                    if( pe.trait.m_path == resolve.m_lang_Fn || pe.trait.m_path == resolve.m_lang_FnMut || pe.trait.m_path == resolve.m_lang_FnOnce )
                    {
                        MIR_ASSERT(state, e.args.size() == 2, "Fn* call requires two arguments");
                        auto fcn_lvalue = mv$(e.args[0]);
                        auto args_lvalue = mv$(e.args[1]);

                        DEBUG("Convert function pointer call");

                        e.args.clear();
                        e.args.reserve( fcn_ty.m_arg_types.size() );
                        for(unsigned int i = 0; i < fcn_ty.m_arg_types.size(); i ++)
                        {
                            e.args.push_back( ::MIR::LValue::make_Field({ box$(args_lvalue.clone()), i }) );
                        }
                        // If the trait is Fn/FnMut, dereference the input value.
                        if( pe.trait.m_path == resolve.m_lang_FnOnce )
                            e.fcn = mv$(fcn_lvalue);
                        else
                            e.fcn = ::MIR::LValue::make_Deref({ box$(fcn_lvalue) });
                    }
                }
            )
        )

        mutator.flush();
        mutator.cur_block += 1;
        mutator.cur_stmt = 0;
    }
}

void MIR_CleanupCrate(::HIR::Crate& crate)
{
    ::MIR::OuterVisitor    ov { crate, [&](const auto& res, const auto& p, auto& expr_ptr, const auto& args, const auto& ty){
            MIR_Cleanup(res, p, *expr_ptr.m_mir, args, ty);
        } };
    ov.visit_crate(crate);
}

