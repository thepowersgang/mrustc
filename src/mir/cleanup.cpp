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
#include <trans/target.hpp>

class MirMutator
{
    ::MIR::Function& m_fcn;
    unsigned int    cur_block;
    unsigned int    cur_stmt;
    mutable ::std::vector< ::MIR::Statement>    new_statements;

public:
    MirMutator(::MIR::Function& fcn, unsigned int bb, unsigned int stmt):
        m_fcn(fcn),
        cur_block(bb), cur_stmt(stmt)
    {
    }

    void update_state(::MIR::TypeResolve& state)
    {
        if( this->cur_stmt == m_fcn.blocks[this->cur_block].statements.size() )
        {
            state.set_cur_stmt_term(this->cur_block);
        }
        else
        {
            state.set_cur_stmt( this->cur_block, this->cur_stmt );
        }
    }

    ::MIR::LValue new_temporary(::HIR::TypeRef ty)
    {
        auto rv = ::MIR::LValue::new_Local( static_cast<unsigned int>(m_fcn.locals.size()) );
        m_fcn.locals.push_back( mv$(ty) );
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

    decltype(new_statements.begin()) flush_stmt()
    {
        auto rv = flush();
        this->cur_stmt += 1;
        return rv;
    }

    void flush_block()
    {
        auto rv = flush();
        this->cur_stmt = 0;
        this->cur_block += 1;
    }

private:
    decltype(new_statements.begin()) flush()
    {
        auto& block = m_fcn.blocks.at(cur_block);
        assert( cur_stmt <= block.statements.size() );
        auto it = block.statements.begin() + cur_stmt;
        if( new_statements.size() > 0 )
        {
            DEBUG("flush - BB" << cur_block << "/" << cur_stmt);
            for(auto& stmt : new_statements)
            {
                DEBUG("- Push stmt @" << cur_stmt << ": " << stmt);
                it = block.statements.insert(it, mv$(stmt));
                ++ it;
                cur_stmt += 1;
            }
            new_statements.clear();
        }
        return it;
    }
};

void MIR_Cleanup_LValue(const ::MIR::TypeResolve& state, MirMutator& mutator, ::MIR::LValue& lval);

namespace {
    ::HIR::TypeRef get_vtable_type(const Span& sp, const ::StaticTraitResolve& resolve, const ::HIR::TypeData::Data_TraitObject& te)
    {
        return te.m_trait.m_trait_ptr->get_vtable_type(sp, resolve.m_crate, te);
    }
}

const ::HIR::Literal* MIR_Cleanup_GetConstant(const MIR::TypeResolve& state, const ::HIR::Path& path,  ::HIR::TypeRef& out_ty)
{
    TRACE_FUNCTION_F(path);

    MonomorphState  params;
    auto v = state.m_resolve.get_value(state.sp, path, params);
    if( const auto* e = v.opt_Constant() )
    {
        const auto& hir_const = **e;
        out_ty = params.monomorph_type(state.sp, hir_const.m_type);
        if( hir_const.m_value_res.is_Defer() )
        {
            // Do some form of lookup of a pre-cached evaluated monomorphised constant
            // - Maybe on the `Constant` entry there can be a list of pre-monomorphised values
            auto it = hir_const.m_monomorph_cache.find(path);
            if( it == hir_const.m_monomorph_cache.end() )
            {
                // TODO: Emit a bug if the cache is empty? (or if this is in the post-monomorph pass)
                //MIR_BUG(state, "Constant with Defer literal and no cached monomorphisation - " << path);
                return nullptr;
            }
            MIR_ASSERT(state, !it->second.is_Defer(), "get_literal_for_const - Cached literal was Defer - " << path);
            return &it->second;
        }
        return &hir_const.m_value_res;
    }
    else if( v.is_NotYetKnown() )
    {
        auto v = state.m_resolve.get_value(state.sp, path, params, /*signature_only=*/true);
        if( const auto* e = v.opt_Constant() )
        {
            const auto& hir_const = **e;
            out_ty = params.monomorph_type(state.sp, hir_const.m_type);
        }
        else
        {
            MIR_BUG(state, "get_literal_for_const - Not a constant - " << path);
        }
        return nullptr;
    }
    else
    {
        MIR_BUG(state, "get_literal_for_const - Not a constant - " << path);
        return nullptr;
    }
}

::MIR::RValue MIR_Cleanup_LiteralToRValue(const ::MIR::TypeResolve& state, MirMutator& mutator, const ::HIR::Literal& lit, ::HIR::TypeRef ty, ::HIR::Path path)
{
    TU_MATCH_HDRA( (ty.data()), {)
    default:
        if( path == ::HIR::GenericPath() )
            MIR_TODO(state, "Literal of type " << ty << " - " << lit);
        DEBUG("Unknown type " << ty << ", but a path was provided - Return ItemAddr " << path);
        return ::MIR::Constant::make_ItemAddr( box$(path) );
    TU_ARMA(Tuple, te) {
        MIR_ASSERT(state, lit.is_List(), "Non-list literal for Tuple - " << lit);
        const auto& vals = lit.as_List();
        MIR_ASSERT(state, vals.size() == te.size(), "Literal size mismatched with tuple size");

        ::std::vector< ::MIR::Param>   lvals;
        lvals.reserve( vals.size() );

        for(size_t i = 0; i < vals.size(); i ++)
        {
            auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, vals[i], te[i].clone(), ::HIR::GenericPath());
            lvals.push_back( mutator.in_temporary( HIR::TypeRef(te[i]), mv$(rval)) );
        }

        return ::MIR::RValue::make_Tuple({ mv$(lvals) });
        }
    TU_ARMA(Array, te) {
        MIR_ASSERT(state, lit.is_List(), "Non-list literal for Array - " << lit);
        const auto& vals = lit.as_List();

        MIR_ASSERT(state, TU_TEST1(te.size, Known, == vals.size()), "Literal size mismatched with array size - [_; " << vals.size() << "] != " << ty);

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

        // If all of the literals are the same value, then optimise into a count-based initialisation
        if( is_all_same )
        {
            auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, vals[0], te.inner.clone(), ::HIR::GenericPath());
            auto data_lval = mutator.in_temporary(te.inner.clone(), mv$(rval));
            return ::MIR::RValue::make_SizedArray({ mv$(data_lval), static_cast<unsigned int>(vals.size()) });
        }
        else
        {
            ::std::vector< ::MIR::Param>   lvals;
            lvals.reserve( vals.size() );

            for(const auto& val: vals)
            {
                auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, val, te.inner.clone(), ::HIR::GenericPath());
                lvals.push_back( mutator.in_temporary(te.inner.clone(), mv$(rval)) );
            }

            return ::MIR::RValue::make_Array({ mv$(lvals) });
        }
        }
    TU_ARMA(Path, te) {
        if( te.binding.is_Struct() )
        {
            const auto& str = *te.binding.as_Struct();
            const auto& vals = lit.as_List();

            auto monomorph = [&](const auto& tpl) { return MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr).monomorph_type(state.sp, tpl); };

            ::std::vector< ::MIR::Param>   lvals;
            TU_MATCH_HDRA( (str.m_data), { )
            TU_ARMA(Unit, se) {
                MIR_ASSERT(state, vals.size() == 0, "Values passed for unit struct");
                }
            TU_ARMA(Tuple, se) {
                MIR_ASSERT(state, vals.size() == se.size(), "Value count mismatch in literal for " << ty << " - exp " << se.size() << ", " << lit);
                for(unsigned int i = 0; i < se.size(); i ++)
                {
                    auto ent_ty = monomorph(se[i].ent);
                    auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, vals[i], ent_ty.clone(), ::HIR::GenericPath());
                    lvals.push_back( mutator.in_temporary(mv$(ent_ty), mv$(rval)) );
                }
                }
            TU_ARMA(Named, se) {
                MIR_ASSERT(state, vals.size() == se.size(), "Value count mismatch in literal for " << ty << " - exp " << se.size() << ", " << lit);
                for(unsigned int i = 0; i < se.size(); i ++)
                {
                    auto ent_ty = monomorph(se[i].second.ent);
                    auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, vals[i], ent_ty.clone(), ::HIR::GenericPath());
                    lvals.push_back( mutator.in_temporary(mv$(ent_ty), mv$(rval)) );
                }
                }
            }
            return ::MIR::RValue::make_Struct({ te.path.m_data.as_Generic().clone(), mv$(lvals) });
        }
        else if( te.binding.is_Enum() )
        {
            const auto& enm = *te.binding.as_Enum();
            const auto& lit_var = lit.as_Variant();

            auto monomorph = [&](const auto& tpl) { return MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr).monomorph_type(state.sp, tpl); };

            MIR_ASSERT(state, lit_var.idx < enm.num_variants(), "Variant index out of range");
            std::vector<::MIR::Param>  vals;
            if( const auto* e = enm.m_data.opt_Data() )
            {
                auto ty = monomorph( e->at(lit_var.idx).type );
                MIR_ASSERT(state, lit_var.val->is_List(), "");
                auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, *lit_var.val, ty.clone(), ::HIR::GenericPath());
                vals = rval.is_Struct() ? std::move(rval.as_Struct().vals) : std::move(rval.as_Tuple().vals);
            }
            else
            {
                // Leave empty
            }
            return ::MIR::RValue::make_EnumVariant({ te.path.m_data.as_Generic().clone(), lit_var.idx, mv$(vals) });
        }
        else if( te.binding.is_Union() )
        {
            const auto& un = *te.binding.as_Union();
            const auto& lit_var = lit.as_Variant();

            auto monomorph = [&](const auto& tpl) { return MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr).monomorph_type(state.sp, tpl); };

            ::MIR::Param    p;
            auto ty = monomorph( un.m_variants.at(lit_var.idx).second.ent );
            auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, *lit_var.val, ty.clone(), ::HIR::GenericPath());
            p = mutator.in_temporary(mv$(ty), mv$(rval));
            return ::MIR::RValue::make_UnionVariant({ te.path.m_data.as_Generic().clone(), lit_var.idx, mv$(p) });
        }
        else
        {
            MIR_BUG(state, "Unexpected type for literal from " << path << " - " << ty << " (lit = " << lit << ")");
        }
        }
    TU_ARMA(Primitive, te) {
        switch(te)
        {
        case ::HIR::CoreType::Char:
        case ::HIR::CoreType::Usize:
        case ::HIR::CoreType::U128:
        case ::HIR::CoreType::U64:
        case ::HIR::CoreType::U32:
        case ::HIR::CoreType::U16:
        case ::HIR::CoreType::U8:
            MIR_ASSERT(state, lit.is_Integer(), "Literal for " << path << ": " << ty << " not an integer, instead " << lit);
            return ::MIR::Constant::make_Uint({ lit.as_Integer(), te });
        case ::HIR::CoreType::Isize:
        case ::HIR::CoreType::I128:
        case ::HIR::CoreType::I64:
        case ::HIR::CoreType::I32:
        case ::HIR::CoreType::I16:
        case ::HIR::CoreType::I8:
            MIR_ASSERT(state, lit.is_Integer(), "Literal for " << path << ": " << ty << " not an integer, instead " << lit);
            return ::MIR::Constant::make_Int({ static_cast<int64_t>(lit.as_Integer()), te });
        case ::HIR::CoreType::F64:
        case ::HIR::CoreType::F32:
            MIR_ASSERT(state, lit.is_Float(), "Literal for " << path << ": " << ty << " not a float, instead " << lit);
            return ::MIR::Constant::make_Float({ lit.as_Float(), te });
        case ::HIR::CoreType::Bool:
            MIR_ASSERT(state, lit.is_Integer(), "Literal for " << path << ": " << ty << " not an integer, instead " << lit);
            return ::MIR::Constant::make_Bool({ !!lit.as_Integer() });
        case ::HIR::CoreType::Str:
            MIR_BUG(state, "Const of type `str` - " << path);
        }
        throw "";
        }
    TU_ARMA(Pointer, te) {
        if( lit.is_BorrowPath() || lit.is_BorrowData() ) {
            MIR_TODO(state, "BorrowOf into pointer - " << lit << " into " << ty);
        }
        else {
            auto lval = mutator.in_temporary( ::HIR::CoreType::Usize, ::MIR::RValue( ::MIR::Constant::make_Uint({ lit.as_Integer(), ::HIR::CoreType::Usize }) ) );
            return ::MIR::RValue::make_Cast({ mv$(lval), mv$(ty) });
        }
        }
    TU_ARMA(Borrow, te) {
        if( const auto* pp = lit.opt_BorrowPath() )
        {
            const auto& path = *pp;
            auto ptr_val = ::MIR::Constant::make_ItemAddr(box$(path.clone()));
            // TODO: Get the metadata type (for !Sized wrapper types)
            if( te.inner.data().is_Slice() )
            {
                ::HIR::TypeRef tmp;
                const auto& ty = state.get_static_type(tmp, path);
                MIR_ASSERT(state, ty.data().is_Array(), "BorrowOf returning slice not of an array, instead " << ty);
                const auto& te = ty.data().as_Array();
                MIR_ASSERT(state, te.size.is_Known(), "BorrowOf returning slice of unknown-sized array - " << ty);
                unsigned int size = te.size.as_Known();

                auto size_val = ::MIR::Param( ::MIR::Constant::make_Uint({ size, ::HIR::CoreType::Usize }) );
                return ::MIR::RValue::make_MakeDst({ ::MIR::Param(mv$(ptr_val)), mv$(size_val) });
            }
            else if( const auto* tep = te.inner.data().opt_TraitObject() )
            {
                ::HIR::TypeRef tmp;
                const auto& ty = state.get_static_type(tmp, path);

                auto vtable_path = ::HIR::Path(&ty == &tmp ? mv$(tmp) : ty.clone(), tep->m_trait.m_path.clone(), "vtable#");

                auto vtable_val = ::MIR::Param( ::MIR::Constant::make_ItemAddr(box$(vtable_path)) );

                return ::MIR::RValue::make_MakeDst({ ::MIR::Param(mv$(ptr_val)), mv$(vtable_val) });
            }
            else
            {
                return mv$(ptr_val);
            }
        }
        else if( const auto* e = lit.opt_BorrowData() ) {
            const auto& inner_lit = *e->val;
            // 1. Make a new lvalue for the inner data
            // 2. Borrow that slot
            if( const auto* tie = te.inner.data().opt_Slice() )
            {
                MIR_ASSERT(state, inner_lit.is_List(), "BorrowData of non-list resulting in &[T]");
                auto size = inner_lit.as_List().size();
                auto inner_ty = ::HIR::TypeRef::new_array(tie->inner.clone(), size);
                auto size_val = ::MIR::Param( ::MIR::Constant::make_Uint({ size, ::HIR::CoreType::Usize }) );
                auto ptr_ty = ::HIR::TypeRef::new_borrow(te.type, inner_ty.clone());

                auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, inner_lit, inner_ty.clone(), ::HIR::GenericPath());

                auto lval = mutator.in_temporary( mv$(inner_ty), mv$(rval) );
                auto ptr_val = mutator.in_temporary( mv$(ptr_ty), ::MIR::RValue::make_Borrow({ te.type, mv$(lval) }));
                return ::MIR::RValue::make_MakeDst({ ::MIR::Param(mv$(ptr_val)), mv$(size_val) });
            }
            else if( te.inner.data().is_TraitObject() )
            {
                MIR_BUG(state, "BorrowData returning TraitObject shouldn't be allowed - " << ty << " from " << inner_lit);
            }
            else
            {
                auto rval = MIR_Cleanup_LiteralToRValue(state, mutator, inner_lit, te.inner.clone(), ::HIR::GenericPath());
                auto lval = mutator.in_temporary( te.inner.clone(), mv$(rval) );
                return ::MIR::RValue::make_Borrow({ te.type, mv$(lval) });
            }
        }
        else if( te.inner.data().is_Slice() && te.inner.data().as_Slice().inner == ::HIR::CoreType::U8 ) {
            ::std::vector<uint8_t>  bytestr;
            for(auto v : lit.as_String())
                bytestr.push_back( static_cast<uint8_t>(v) );
            return ::MIR::RValue::make_MakeDst({ ::MIR::Constant(mv$(bytestr)), ::MIR::Constant::make_Uint({ lit.as_String().size(), ::HIR::CoreType::Usize }) });
        }
        else if( te.inner.data().is_Array() && te.inner.data().as_Array().inner == ::HIR::CoreType::U8 ) {
            // TODO: How does this differ at codegen to the above?
            ::std::vector<uint8_t>  bytestr;
            for(auto v : lit.as_String())
                bytestr.push_back( static_cast<uint8_t>(v) );
            return ::MIR::Constant::make_Bytes( mv$(bytestr) );
        }
        else if( te.inner == ::HIR::CoreType::Str ) {
            return ::MIR::Constant::make_StaticString( lit.as_String() );
        }
        else {
            MIR_TODO(state, "Const with type " << ty);
        }
        }
    TU_ARMA(Function, te) {
        //MIR_TODO(state, "Const function pointer " << lit << " w/ type " << ty);
        MIR_ASSERT(state, lit.is_BorrowPath(), "");
        return ::MIR::Constant::make_ItemAddr( box$( lit.as_BorrowPath().clone() ) );
        }
    }
    throw "";
}

::MIR::LValue MIR_Cleanup_Virtualize(
    const Span& sp, const ::MIR::TypeResolve& state, MirMutator& mutator,
    ::MIR::LValue& receiver_lvp,
    const ::HIR::TypeData::Data_TraitObject& te, const ::HIR::Path::Data::Data_UfcsKnown& pe
    )
{
    TRACE_FUNCTION_F("<" << pe.type << " as " << pe.trait << ">::" << pe.item << pe.params);

    assert( te.m_trait.m_trait_ptr );
    assert( pe.type.data().is_TraitObject() );
    assert( &te == &pe.type.data().as_TraitObject() );
    const auto& trait = *te.m_trait.m_trait_ptr;

    // 1. Get the vtable index for this function
    unsigned int vtable_idx = trait.get_vtable_value_index(pe.trait.m_path, pe.item);
    if( vtable_idx == 0 )
        BUG(sp, "Calling method '" << pe.item << "' from " << pe.trait << " through " << te.m_trait.m_path << " which isn't in the vtable");

    // 2. Load from the vtable
    auto vtable_ty = ::HIR::TypeRef::new_pointer( ::HIR::BorrowType::Shared, get_vtable_type(sp, state.m_resolve, te) );

    // If the method is a by-value method, add a `&move`
    const auto& fn_def = state.m_crate.get_trait_by_path(sp, pe.trait.m_path).m_values.at(pe.item).as_Function();
    if( fn_def.m_receiver == HIR::Function::Receiver::Value )
    {
        receiver_lvp = mutator.in_temporary(
            HIR::TypeRef::new_borrow(HIR::BorrowType::Owned, pe.type.clone()),
            MIR::RValue::make_Borrow({ HIR::BorrowType::Owned, mv$(receiver_lvp) })
            );
    }

    // Allocate a temporary for the vtable pointer itself
    auto vtable_lv = mutator.new_temporary( mv$(vtable_ty) );
    auto fcn_lval = ::MIR::LValue::new_Field( ::MIR::LValue::new_Deref( vtable_lv.clone() ), vtable_idx );
    ::HIR::TypeRef  tmp;
    const auto& ty = state.get_lvalue_type(tmp, fcn_lval);
    const auto& receiver = ty.data().as_Function().m_arg_types.at(0);
    
    struct H {
        static ::MIR::LValue get_unit_ptr(
            const ::MIR::TypeResolve& state, MirMutator& mutator,
            ::HIR::TypeRef ty, ::MIR::LValue lv,
            ::MIR::LValue& out_inner_ptr
            )
        {
            if( ty.data().is_Path() )
            {
                const auto& te = ty.data().as_Path();
                MIR_ASSERT(state, te.binding.is_Struct(), "");
                const auto& ty_path = te.path.m_data.as_Generic();
                const auto& str = *te.binding.as_Struct();
                ::HIR::TypeRef  tmp;
                auto monomorph = [&](const auto& t) { return MonomorphStatePtr(nullptr, &ty_path.m_params, nullptr).monomorph_type(state.sp, t); };
                ::std::vector< ::MIR::Param>   vals;
                TU_MATCH_HDRA( (str.m_data), {)
                TU_ARMA(Unit, se) {
                    }
                TU_ARMA(Tuple, se) {
                    for(unsigned int i = 0; i < se.size(); i ++ ) {
                        auto val = ::MIR::LValue::new_Field( (i == se.size() - 1 ? mv$(lv) : lv.clone()), i );
                        if( i == str.m_struct_markings.coerce_unsized_index ) {
                            vals.push_back( H::get_unit_ptr(state, mutator, monomorph(se[i].ent), mv$(val), out_inner_ptr) );
                        }
                        else {
                            vals.push_back( mv$(val) );
                        }
                    }
                    }
                TU_ARMA(Named, se) {
                    for(unsigned int i = 0; i < se.size(); i ++ ) {
                        auto val = ::MIR::LValue::new_Field( (i == se.size() - 1 ? mv$(lv) : lv.clone()), i );
                        if( i == str.m_struct_markings.coerce_unsized_index ) {
                            vals.push_back( H::get_unit_ptr(state, mutator, monomorph(se[i].second.ent), mv$(val), out_inner_ptr ) );
                        }
                        else {
                            vals.push_back( mv$(val) );
                        }
                    }
                    }
                }

                auto new_path = ty_path.clone();
                return mutator.in_temporary( mv$(ty), ::MIR::RValue::make_Struct({ mv$(new_path), mv$(vals) }) );
            }
            else if( ty.data().is_Borrow() || ty.data().is_Pointer() )
            {
                out_inner_ptr = lv.clone();
                return mutator.in_temporary(
                    ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, ::HIR::TypeRef::new_unit()),
                    ::MIR::RValue::make_DstPtr({ mv$(lv) })
                    );
            }
            else
            {
                MIR_BUG(state, "Unexpected type coerce_unsize in receiver - " << ty);
            }
        }
    };

    ::MIR::LValue   receiver_ptr;
    ::MIR::LValue   inner_dyn_ptr;

    if( receiver.data().is_Path()
        && receiver.data().as_Path().binding.is_Struct()
        && receiver.data().as_Path().binding.as_Struct()->m_struct_markings.coerce_unsized != ::HIR::StructMarkings::Coerce::None
        )
    {
        // If the receiver is Box (or anything that implements CoerceUnsized), create a Foo<()> as the value.
        // - Requires de/restructuring the Box same as CoerceUnsized
        // - Can use the `coerce_unsized_index` field too
        receiver_lvp = H::get_unit_ptr(state,mutator, receiver.clone(), receiver_lvp.clone(), inner_dyn_ptr);
    }
    else if( receiver.data().is_Borrow() || receiver.data().is_Pointer() )
    {
        inner_dyn_ptr = receiver_lvp.clone();
        auto ptr_rval = ::MIR::RValue::make_DstPtr({ receiver_lvp.clone() });

        auto ptr_lv = mutator.new_temporary( ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, ::HIR::TypeRef::new_unit()) );
        mutator.push_statement( ::MIR::Statement::make_Assign({ ptr_lv.clone(), mv$(ptr_rval) }) );
        receiver_lvp = mv$(ptr_lv);
    }
    else
    {
        // TODO: How to handle `Pin`?
        // - Locate the pointer (similar to unsized path?)
        MIR_TODO(state, "Handle virtual call through " << receiver);
    }

    // - Load the vtable and store it
    auto vtable_rval = ::MIR::RValue::make_DstMeta({ mv$(inner_dyn_ptr) });
    mutator.push_statement( ::MIR::Statement::make_Assign({ vtable_lv.clone(), mv$(vtable_rval) }) );

    // Update the terminator with the new information.
    return fcn_lval;
}

bool MIR_Cleanup_Unsize_GetMetadata(const ::MIR::TypeResolve& state, MirMutator& mutator,
        const ::HIR::TypeRef& dst_ty, const ::HIR::TypeRef& src_ty, const ::MIR::LValue& ptr_value,
        ::MIR::Param& out_meta_val, ::HIR::TypeRef& out_meta_ty, bool& out_src_is_dst
        )
{
    TU_MATCH_HDRA( (dst_ty.data()), { )
    default:
        MIR_TODO(state, "Obtain metadata converting to " << dst_ty);
    TU_ARMA(Generic, de) {
        // TODO: What should be returned to indicate "no conversion"
        return false;
        }
    TU_ARMA(Path, de) {
        // Source must be Path and Unsize
        if( de.binding.is_Opaque() )
            return false;

        MIR_ASSERT(state, src_ty.data().is_Path(), "Unsize to path from non-path - " << src_ty);
        const auto& se = src_ty.data().as_Path();
        MIR_ASSERT(state, de.binding.tag() == se.binding.tag(), "Unsize between mismatched types - " << dst_ty << " and " << src_ty);
        MIR_ASSERT(state, de.binding.is_Struct(), "Unsize to non-struct - " << dst_ty);
        MIR_ASSERT(state, de.binding.as_Struct() == se.binding.as_Struct(), "Unsize between mismatched types - " << dst_ty << " and " << src_ty);
        const auto& str = *de.binding.as_Struct();
        MIR_ASSERT(state, str.m_struct_markings.unsized_field != ~0u, "Unsize on type that doesn't implement have a ?Sized field - " << dst_ty);

        auto monomorph_cb_d = MonomorphStatePtr(nullptr, &de.path.m_data.as_Generic().m_params, nullptr);
        auto monomorph_cb_s = MonomorphStatePtr(nullptr, &se.path.m_data.as_Generic().m_params, nullptr);

        // Return GetMetadata on the inner type
        TU_MATCH_HDRA( (str.m_data), {)
        TU_ARMA(Unit, se) {
            MIR_BUG(state, "Unit-like struct Unsize is impossible - " << src_ty);
            }
        TU_ARMA(Tuple, se) {
            const auto& ty_tpl = se.at( str.m_struct_markings.unsized_field ).ent;
            auto ty_d = monomorph_cb_d.monomorph_type(state.sp, ty_tpl, false);
            auto ty_s = monomorph_cb_s.monomorph_type(state.sp, ty_tpl, false);

            return MIR_Cleanup_Unsize_GetMetadata(state, mutator,  ty_d, ty_s, ptr_value,  out_meta_val,out_meta_ty,out_src_is_dst);
            }
        TU_ARMA(Named, se) {
            const auto& ty_tpl = se.at( str.m_struct_markings.unsized_field ).second.ent;
            auto ty_d = monomorph_cb_d.monomorph_type(state.sp, ty_tpl, false);
            auto ty_s = monomorph_cb_s.monomorph_type(state.sp, ty_tpl, false);

            return MIR_Cleanup_Unsize_GetMetadata(state, mutator,  ty_d, ty_s, ptr_value,  out_meta_val,out_meta_ty,out_src_is_dst);
            }
        }
        throw "";
        }
    TU_ARMA(Slice, de) {
        // Source must be an array (or generic)
        if( src_ty.data().is_Array() )
        {
            const auto& in_array = src_ty.data().as_Array();
            out_meta_ty = ::HIR::CoreType::Usize;
            out_meta_val = ::MIR::Constant::make_Uint({ in_array.size.as_Known(), ::HIR::CoreType::Usize });
            return true;
        }
        else if( src_ty.data().is_Generic() || (src_ty.data().is_Path() && src_ty.data().as_Path().binding.is_Opaque()) )
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
        }
    TU_ARMA(TraitObject, de) {

        auto ty_unit_ptr = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, ::HIR::TypeRef::new_unit());

        // No data trait, vtable is a null unit pointer.
        // - Shouldn't the vtable be just unit?
        // - Codegen assumes it's a pointer.
        if( de.m_trait.m_path.m_path == ::HIR::SimplePath() )
        {
            auto null_lval = mutator.in_temporary( ::HIR::CoreType::Usize, ::MIR::Constant::make_Uint({ 0u, ::HIR::CoreType::Usize }) );
            out_meta_ty = ty_unit_ptr.clone();
            out_meta_val = mutator.in_temporary( out_meta_ty.clone(), ::MIR::RValue::make_Cast({ mv$(null_lval), mv$(ty_unit_ptr) }) );
        }
        else
        {
            const auto& trait_path = de.m_trait;
            const auto& trait = *de.m_trait.m_trait_ptr;

            // Obtain vtable type `::"path"::to::Trait#vtable`
            const auto& vtable_ty_spath = trait.m_vtable_path;
            MIR_ASSERT(state, vtable_ty_spath != HIR::SimplePath(), "Trait " << de.m_trait.m_path << " does not have a vtable");
            const auto& vtable_ref = state.m_crate.get_struct_by_path(state.sp, vtable_ty_spath);
            // Copy the param set from the trait in the trait object
            ::HIR::PathParams   vtable_params = trait_path.m_path.m_params.clone();
            // - Include associated types
            for(const auto& ty_b : trait_path.m_type_bounds) {
                auto idx = trait.m_type_indexes.at(ty_b.first);
                if(vtable_params.m_types.size() <= idx)
                    vtable_params.m_types.resize(idx+1);
                vtable_params.m_types[idx] = ty_b.second.type.clone();
            }
            auto vtable_type = ::HIR::TypeRef::new_path( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref );

            out_meta_ty = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, mv$(vtable_type));

            // If the data trait hasn't changed, return the vtable pointer
            if( src_ty.data().is_TraitObject() )
            {
                out_src_is_dst = true;
                out_meta_val = mutator.in_temporary( out_meta_ty.clone(), ::MIR::RValue::make_DstMeta({ ptr_value.clone() }) );
            }
            else
            {
                MIR_ASSERT(state, state.m_resolve.type_is_sized(state.sp, src_ty), "Attempting to get vtable for unsized type - " << src_ty);

                ::HIR::Path vtable { src_ty.clone(), trait_path.m_path.clone(), "vtable#" };
                out_meta_val = ::MIR::Constant::make_ItemAddr(box$(vtable));
            }
        }
        return true;
        }
    }
    throw "";
}

::MIR::RValue MIR_Cleanup_Unsize(const ::MIR::TypeResolve& state, MirMutator& mutator, const ::HIR::TypeRef& dst_ty, const ::HIR::TypeRef& src_ty_inner, ::MIR::LValue ptr_value)
{
    const auto& dst_ty_inner = (dst_ty.data().is_Borrow() ? dst_ty.data().as_Borrow().inner : dst_ty.data().as_Pointer().inner);

    ::HIR::TypeRef  meta_type;
    ::MIR::Param   meta_value;
    bool source_is_dst = false;
    if( MIR_Cleanup_Unsize_GetMetadata(state, mutator, dst_ty_inner, src_ty_inner, ptr_value,  meta_value, meta_type, source_is_dst) )
    {
        // There is a case where the source is already a fat pointer. In that case the pointer of the new DST must be the source DST pointer
        if( source_is_dst )
        {
            auto ty_unit_ptr = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, ::HIR::TypeRef::new_unit());
            auto thin_ptr_lval = mutator.in_temporary( mv$(ty_unit_ptr), ::MIR::RValue::make_DstPtr({ mv$(ptr_value) }) );

            return ::MIR::RValue::make_MakeDst({ mv$(thin_ptr_lval), mv$(meta_value) });
        }
        else
        {
            return ::MIR::RValue::make_MakeDst({ mv$(ptr_value), mv$(meta_value) });
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
    if( dst_ty.data().is_Path() )
    {
        MIR_ASSERT(state, src_ty.data().is_Path(), "CoerceUnsized to Path must have a Path source - " << src_ty << " to " << dst_ty);
        const auto& dte = dst_ty.data().as_Path();
        const auto& ste = src_ty.data().as_Path();

        // - Types must differ only by a single field, and be from the same definition
        MIR_ASSERT(state, dte.binding.is_Struct(), "Note, can't CoerceUnsized non-structs");
        MIR_ASSERT(state, dte.binding.tag() == ste.binding.tag(),
            "Note, can't CoerceUnsized mismatched structs - " << src_ty << " to " << dst_ty);
        MIR_ASSERT(state, dte.binding.as_Struct() == ste.binding.as_Struct(),
            "Note, can't CoerceUnsized mismatched structs - " << src_ty << " to " << dst_ty);
        const auto& str = *dte.binding.as_Struct();
        MIR_ASSERT(state, str.m_struct_markings.coerce_unsized_index != ~0u,
            "Struct " << src_ty << " doesn't impl CoerceUnsized");


        auto monomorph_cb_d = MonomorphStatePtr(nullptr, &dte.path.m_data.as_Generic().m_params, nullptr);
        auto monomorph_cb_s = MonomorphStatePtr(nullptr, &ste.path.m_data.as_Generic().m_params, nullptr);

        // - Destructure and restrucure with the unsized fields
        ::std::vector<::MIR::Param>    ents;
        TU_MATCH_HDRA( (str.m_data), {)
        TU_ARMA(Unit, se) {
            MIR_BUG(state, "Unit-like struct CoerceUnsized is impossible - " << src_ty);
            }
        TU_ARMA(Tuple, se) {
            ents.reserve( se.size() );
            for(unsigned int i = 0; i < se.size(); i++)
            {
                if( i == str.m_struct_markings.coerce_unsized_index )
                {
                    auto ty_d = monomorph_cb_d.monomorph_type(state.sp, se[i].ent, false);
                    auto ty_s = monomorph_cb_s.monomorph_type(state.sp, se[i].ent, false);

                    auto new_rval = MIR_Cleanup_CoerceUnsized(state, mutator, ty_d, ty_s,  ::MIR::LValue::new_Field(value.clone(), i));
                    auto new_lval = mutator.in_temporary( mv$(ty_d), mv$(new_rval) );

                    ents.push_back( mv$(new_lval) );
                }
                else if( state.m_resolve.is_type_phantom_data( se[i].ent ) )
                {
                    auto ty_d = monomorph_cb_d.monomorph_type(state.sp, se[i].ent, false);

                    auto new_rval = ::MIR::RValue::make_Struct({ ty_d.data().as_Path().path.m_data.as_Generic().clone(), {} });
                    auto new_lval = mutator.in_temporary( mv$(ty_d), mv$(new_rval) );

                    ents.push_back( mv$(new_lval) );
                }
                else
                {
                    ents.push_back( ::MIR::LValue::new_Field(value.clone(), i) );
                }
            }
            }
        TU_ARMA(Named, se) {
            ents.reserve( se.size() );
            for(unsigned int i = 0; i < se.size(); i++)
            {
                if( i == str.m_struct_markings.coerce_unsized_index )
                {
                    auto ty_d = monomorph_cb_d.monomorph_type(state.sp, se[i].second.ent, false);
                    auto ty_s = monomorph_cb_s.monomorph_type(state.sp, se[i].second.ent, false);

                    auto new_rval = MIR_Cleanup_CoerceUnsized(state, mutator, ty_d, ty_s,  ::MIR::LValue::new_Field(value.clone(), i));
                    auto new_lval = mutator.new_temporary( mv$(ty_d) );
                    mutator.push_statement( ::MIR::Statement::make_Assign({ new_lval.clone(), mv$(new_rval) }) );

                    ents.push_back( mv$(new_lval) );
                }
                else if( state.m_resolve.is_type_phantom_data( se[i].second.ent ) )
                {
                    auto ty_d = monomorph_cb_d.monomorph_type(state.sp, se[i].second.ent, false);

                    auto new_rval = ::MIR::RValue::make_Struct({ ty_d.data().as_Path().path.m_data.as_Generic().clone(), {} });
                    auto new_lval = mutator.in_temporary( mv$(ty_d), mv$(new_rval) );

                    ents.push_back( mv$(new_lval) );
                }
                else
                {
                    ents.push_back( ::MIR::LValue::new_Field(value.clone(), i) );
                }
            }
            }
        }
        return ::MIR::RValue::make_Struct({ dte.path.m_data.as_Generic().clone(), mv$(ents) });
    }

    if( dst_ty.data().is_Borrow() )
    {
        MIR_ASSERT(state, src_ty.data().is_Borrow(), "CoerceUnsized to Borrow must have a Borrow source - " << src_ty << " to " << dst_ty);
        const auto& ste = src_ty.data().as_Borrow();

        return MIR_Cleanup_Unsize(state, mutator, dst_ty, ste.inner, mv$(value));
    }

    // Pointer Coercion - Downcast and unsize
    if( dst_ty.data().is_Pointer() )
    {
        MIR_ASSERT(state, src_ty.data().is_Pointer(), "CoerceUnsized to Pointer must have a Pointer source - " << src_ty << " to " << dst_ty);
        const auto& dte = dst_ty.data().as_Pointer();
        const auto& ste = src_ty.data().as_Pointer();

        if( dte.type == ste.type )
        {
            return MIR_Cleanup_Unsize(state, mutator, dst_ty, ste.inner, mv$(value));
        }
        else
        {
            MIR_ASSERT(state, dte.inner == ste.inner, "TODO: Can pointer CoerceUnsized unsize? " << src_ty << " to " << dst_ty);
            MIR_ASSERT(state, dte.type < ste.type, "CoerceUnsize attempting to raise pointer type");

            return ::MIR::RValue::make_Cast({ mv$(value), dst_ty.clone() });
        }
    }

    MIR_BUG(state, "Unknown CoerceUnsized target " << dst_ty << " from " << src_ty);
    throw "";
}

void MIR_Cleanup_LValue(const ::MIR::TypeResolve& state, MirMutator& mutator, ::MIR::LValue& lval)
{
    TU_MATCH_HDRA( (lval.m_root), {)
    TU_ARMA(Return, le) {
        }
    TU_ARMA(Argument, le) {
        }
    TU_ARMA(Local, le) {
        }
    TU_ARMA(Static, le) {
        }
    }

    for(size_t i = 0; i < lval.m_wrappers.size(); i ++)
    {
        if( !lval.m_wrappers[i].is_Deref() ) {
            continue ;
        }

        // If this is a deref of Box, unpack and deref the inner pointer
        ::HIR::TypeRef  tmp;
        const auto& ty = state.get_lvalue_type(tmp, lval, lval.m_wrappers.size() - i);
        if( state.m_resolve.is_type_owned_box(ty) )
        {
            unsigned num_injected_fld_zeros = 0;

            // Handle Box by extracting it to its pointer.
            // - Locate (or remember) which field in Box is the pointer, and replace the inner by that field
            // > Dumb idea, assume it's always the first field. Keep accessing until located.

            const auto* typ = &ty;
            while( typ->data().is_Path() )
            {
                const auto& te = typ->data().as_Path();
                MIR_ASSERT(state, te.binding.is_Struct(), "Box contained a non-struct");
                const auto& str = *te.binding.as_Struct();
                const ::HIR::TypeRef* ty_tpl = nullptr;
                TU_MATCH_HDRA( (str.m_data), {)
                TU_ARMA(Unit, se) {
                    MIR_BUG(state, "Box contained a unit-like struct");
                    }
                TU_ARMA(Tuple, se) {
                    MIR_ASSERT(state, se.size() > 0, "Box contained an empty tuple struct");
                    ty_tpl = &se[0].ent;
                    }
                TU_ARMA(Named, se) {
                    MIR_ASSERT(state, se.size() > 0, "Box contained an empty named struct");
                    ty_tpl = &se[0].second.ent;
                    }
                }
                tmp = MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr).monomorph_type(state.sp, *ty_tpl);
                typ = &tmp;

                num_injected_fld_zeros ++;
            }
            MIR_ASSERT(state, typ->data().is_Pointer(), "First non-path field in Box wasn't a pointer - " << *typ);
            // We have reached the pointer. Good.

            // Inject all of the field zero accesses (before the deref)
            while(num_injected_fld_zeros--)
            {
                lval.m_wrappers.insert( lval.m_wrappers.begin() + i, ::MIR::LValue::Wrapper::new_Field(0) );
            }
        }
        else
        {
            // What about other types?
        }
    }
}
void MIR_Cleanup_Constant(const ::MIR::TypeResolve& state, MirMutator& mutator, ::MIR::Constant& p)
{
    if( auto* e = p.opt_Uint() )
    {
        switch(e->t)
        {
        // HACK: Restrict Usize to 32-bits when needed
        case ::HIR::CoreType::Usize:
            if( Target_GetCurSpec().m_arch.m_pointer_bits == 32 )
                e->v &= 0xFFFFFFFF;
            break;
        default:
            break;
        }
    }
}
void MIR_Cleanup_Param(const ::MIR::TypeResolve& state, MirMutator& mutator, ::MIR::Param& p)
{
    TU_MATCH_HDRA( (p), { )
    TU_ARMA(LValue, e) {
        MIR_Cleanup_LValue(state, mutator, e);
        }
    TU_ARMA(Borrow, e) {
        MIR_Cleanup_LValue(state, mutator, e.val);
        }
    TU_ARMA(Constant, e) {
        MIR_Cleanup_Constant(state, mutator, e);
        }
    }

    // Effectively a copy of the code that handles RValue::Constant below
    if( p.is_Constant() && p.as_Constant().is_Const() )
    {
        const auto& ce = p.as_Constant().as_Const();
        ::HIR::TypeRef  c_ty;
        const auto* lit_ptr = MIR_Cleanup_GetConstant(state, *ce.p, c_ty);
        if( lit_ptr && !lit_ptr->is_Defer() )
        {
            DEBUG("Replace constant " << *ce.p << " with " << *lit_ptr);
            auto new_rval = MIR_Cleanup_LiteralToRValue(state, mutator, *lit_ptr, c_ty.clone(), mv$(*ce.p));
            if( auto* lv = new_rval.opt_Use() ) {
                p = ::MIR::Param::make_LValue( ::std::move(*lv) );
            }
            else if( auto* c = new_rval.opt_Constant() ) {
                MIR_Cleanup_Constant(state, mutator, *c);
                p = ::MIR::Param::make_Constant( ::std::move(*c) );
            }
            else {
                auto tmp_lv = mutator.in_temporary( mv$(c_ty), mv$(new_rval) );
                p = ::MIR::Param::make_LValue( ::std::move(tmp_lv) );
            }
        }
        else
        {
            DEBUG("No replacement for constant " << *ce.p);
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
            mutator.update_state(state);
            auto& stmt = *it;

            // >> Detect use of `!` as a value
            ::HIR::TypeRef  tmp;
            if( TU_TEST1(stmt, Assign, .src.is_Borrow()) && state.get_lvalue_type(tmp, stmt.as_Assign().src.as_Borrow().val).data().is_Diverge() )
            {
                DEBUG(state << "Not killing block due to use of `!`, it's being borrowed");
            }
            else
            {
                if( ::MIR::visit::visit_mir_lvalues(stmt, [&](const auto& lv, auto /*vu*/){ return state.get_lvalue_type(tmp, lv).data().is_Diverge();}) )
                {
                    DEBUG(state << "Truncate entire block due to use of `!` as a value - " << stmt);
                    block.statements.erase(it, block.statements.end());
                    block.terminator = ::MIR::Terminator::make_Diverge({});
                    break ;
                }
            }
            // >> Visit all LValues for box deref hackery
            TU_MATCH_HDRA( (stmt), { )
            TU_ARMA(Drop, se) {
                MIR_Cleanup_LValue(state, mutator,  se.slot);
                }
            TU_ARMA(SetDropFlag, se) {
                }
            TU_ARMA(ScopeEnd, se) {
                }
            TU_ARMA(Asm, se) {
                for(auto& v : se.inputs)
                    MIR_Cleanup_LValue(state, mutator,  v.second);
                for(auto& v : se.outputs)
                    MIR_Cleanup_LValue(state, mutator,  v.second);
                }
            TU_ARMA(Assign, se) {
                MIR_Cleanup_LValue(state, mutator,  se.dst);
                TU_MATCH_HDRA( (se.src), {)
                TU_ARMA(Use, re) {
                    MIR_Cleanup_LValue(state, mutator,  re);
                    }
                TU_ARMA(Constant, re) {
                    MIR_Cleanup_Constant(state, mutator, re);
                    }
                TU_ARMA(SizedArray, re) {
                    MIR_Cleanup_Param(state, mutator,  re.val);
                    }
                TU_ARMA(Borrow, re) {
                    MIR_Cleanup_LValue(state, mutator,  re.val);
                    }
                TU_ARMA(Cast, re) {
                    MIR_Cleanup_LValue(state, mutator,  re.val);
                    }
                TU_ARMA(BinOp, re) {
                    MIR_Cleanup_Param(state, mutator,  re.val_l);
                    MIR_Cleanup_Param(state, mutator,  re.val_r);
                    }
                TU_ARMA(UniOp, re) {
                    MIR_Cleanup_LValue(state, mutator,  re.val);
                    }
                TU_ARMA(DstMeta, re) {
                    // HACK: Ensure that the box Deref conversion fires here.
                    re.val.m_wrappers.push_back( ::MIR::LValue::Wrapper::new_Deref() );
                    MIR_Cleanup_LValue(state, mutator,  re.val);
                    re.val.m_wrappers.pop_back();

                    // If the type is an array (due to a monomorpised generic?) then replace.
                    ::HIR::TypeRef  tmp;
                    const auto& ty = state.get_lvalue_type(tmp, re.val);
                    const ::HIR::TypeRef* ity_p;
                    if( const auto* te = ty.data().opt_Borrow() ) {
                        ity_p = &te->inner;
                    }
                    else if( const auto* te = ty.data().opt_Pointer() ) {
                        ity_p = &te->inner;
                    }
                    else {
                        BUG(Span(), "Unexpected input type for DstMeta - " << ty);
                    }
                    if( const auto* te = ity_p->data().opt_Array() ) {
                        se.src = ::MIR::Constant::make_Uint({ te->size.as_Known(), ::HIR::CoreType::Usize });
                    }
                    }
                TU_ARMA(DstPtr, re) {
                    // HACK: Ensure that the box Deref conversion fires here.
                    re.val.m_wrappers.push_back( ::MIR::LValue::Wrapper::new_Deref() );
                    MIR_Cleanup_LValue(state, mutator,  re.val);
                    re.val.m_wrappers.pop_back();
                    }
                TU_ARMA(MakeDst, re) {
                    MIR_Cleanup_Param(state, mutator,  re.ptr_val);
                    MIR_Cleanup_Param(state, mutator,  re.meta_val);
                    }
                TU_ARMA(Tuple, re) {
                    for(auto& lv : re.vals)
                        MIR_Cleanup_Param(state, mutator,  lv);
                    }
                TU_ARMA(Array, re) {
                    for(auto& lv : re.vals)
                        MIR_Cleanup_Param(state, mutator,  lv);
                    }
                TU_ARMA(UnionVariant, re) {
                    MIR_Cleanup_Param(state, mutator,  re.val);
                    }
                TU_ARMA(EnumVariant, re) {
                    for(auto& lv : re.vals)
                        MIR_Cleanup_Param(state, mutator,  lv);
                    }
                TU_ARMA(Struct, re) {
                    for(auto& lv : re.vals)
                        MIR_Cleanup_Param(state, mutator,  lv);
                    }
                }
                }
            }

            // 2. RValue conversions
            if( stmt.is_Assign() )
            {
                auto& se = stmt.as_Assign();

                if(auto* e = se.src.opt_Constant())
                {
                    // Replace `Const` with actual values
                    if(auto* ce = e->opt_Const())
                    {
                        // 1. Find the constant
                        ::HIR::TypeRef  ty;
                        const auto* lit_ptr = MIR_Cleanup_GetConstant(state, *ce->p, ty);
                        if( lit_ptr && !lit_ptr->is_Defer() )
                        {
                            DEBUG("Replace constant " << *ce->p << " with " << *lit_ptr);
                            se.src = MIR_Cleanup_LiteralToRValue(state, mutator, *lit_ptr, mv$(ty), mv$(*ce->p));
                            if( auto* p = se.src.opt_Constant() ) {
                                MIR_Cleanup_Constant(state, mutator, *p);
                            }
                        }
                        else
                        {
                            DEBUG("No replacement for constant " << *ce->p);
                        }
                    }
                }

                // Fix up RValue::Cast into coercions
                if( auto* e = se.src.opt_Cast() )
                {
                    ::HIR::TypeRef  tmp;
                    const auto& src_ty = state.get_lvalue_type(tmp, e->val);
                    // TODO: Unsize and CoerceUnsized operations
                    // - Unsize should create a fat pointer if the pointer class is known (vtable or len)
                    if( auto* te = e->type.data().opt_Borrow() )
                    {
                        //  > & -> & = Unsize, create DST based on the pointer class of the destination.
                        // (&-ptr being destination is otherwise invalid)
                        // TODO Share with the CoerceUnsized handling?
                        se.src = MIR_Cleanup_CoerceUnsized(state, mutator, e->type, src_ty, mv$(e->val));
                    }
                    // - CoerceUnsized should re-create the inner type if known.
                    else if(const auto* dte = e->type.data().opt_Path())
                    {
                        if(const auto* ste = src_ty.data().opt_Path())
                        {
                            ASSERT_BUG( sp, !dte->binding.is_Unbound(), "" );
                            ASSERT_BUG( sp, !ste->binding.is_Unbound(), "" );
                            if( dte->binding.is_Opaque() || ste->binding.is_Opaque() ) {
                                // Either side is opaque, leave for now
                            }
                            else {
                                se.src = MIR_Cleanup_CoerceUnsized(state, mutator, e->type, src_ty, mv$(e->val));
                            }
                        }
                        else {
                            ASSERT_BUG( sp, src_ty.data().is_Generic(), "Cast to Path from " << src_ty );
                        }
                    }
                    else {
                    }
                }
            }

            //DEBUG(it - block.statements.begin());
            it = mutator.flush_stmt();
            //DEBUG(it - block.statements.begin());
        }

        mutator.update_state(state);
        //state.set_cur_stmt_term( mutator.cur_block );

        TU_MATCH_HDRA( (block.terminator), {)
        TU_ARMA(Incomplete, e) {
            }
        TU_ARMA(Return, e) {
            }
        TU_ARMA(Diverge, e) {
            }
        TU_ARMA(Goto, e) {
            }
        TU_ARMA(Panic, e) {
            }
        TU_ARMA(If, e) {
            MIR_Cleanup_LValue(state, mutator, e.cond);
            }
        TU_ARMA(Switch, e) {
            MIR_Cleanup_LValue(state, mutator, e.val);
            }
        TU_ARMA(SwitchValue, e) {
            MIR_Cleanup_LValue(state, mutator, e.val);
            }
        TU_ARMA(Call, e) {
            MIR_Cleanup_LValue(state, mutator, e.ret_val);
            if( e.fcn.is_Value() ) {
                MIR_Cleanup_LValue(state, mutator, e.fcn.as_Value());
            }
            for(auto& lv : e.args)
                MIR_Cleanup_Param(state, mutator, lv);
            }
        }

        // VTable calls
        if(auto* ep = block.terminator.opt_Call())
        {
            auto& e = *ep;
            if(auto* path_p = e.fcn.opt_Path())
            {
                auto& path = *path_p;
                // Detect calling `<Trait as Trait>::method()` and replace with vtable call
                if( path.m_data.is_UfcsKnown() && path.m_data.as_UfcsKnown().type.data().is_TraitObject() )
                {
                    const auto& pe = path.m_data.as_UfcsKnown();
                    const auto& te = pe.type.data().as_TraitObject();
                    // TODO: What if the method is from a supertrait?

                    if( te.m_trait.m_path == pe.trait || resolve.find_named_trait_in_trait(
                            sp, pe.trait.m_path, pe.trait.m_params,
                            *te.m_trait.m_trait_ptr, te.m_trait.m_path.m_path, te.m_trait.m_path.m_params,
                            pe.type,
                            [](const auto&, auto){}
                            )
                        )
                    {
                        auto tgt_lvalue = MIR_Cleanup_Virtualize(sp, state, mutator, e.args.front().as_LValue(), te, pe);
                        e.fcn = mv$(tgt_lvalue);
                    }
                }

                if( path.m_data.is_UfcsKnown() && path.m_data.as_UfcsKnown().type.data().is_Function() )
                {
                    const auto& pe = path.m_data.as_UfcsKnown();
                    const auto& fcn_ty = pe.type.data().as_Function();
                    if( pe.trait.m_path == resolve.m_lang_Fn || pe.trait.m_path == resolve.m_lang_FnMut || pe.trait.m_path == resolve.m_lang_FnOnce )
                    {
                        MIR_ASSERT(state, e.args.size() == 2, "Fn* call requires two arguments");
                        auto fcn_lvalue = mv$(e.args[0].as_LValue());
                        auto args_lvalue = mv$(e.args[1].as_LValue());

                        DEBUG("Convert function pointer call");

                        e.args.clear();
                        e.args.reserve( fcn_ty.m_arg_types.size() );
                        for(unsigned int i = 0; i < fcn_ty.m_arg_types.size(); i ++)
                        {
                            e.args.push_back( ::MIR::LValue::new_Field(args_lvalue.clone(), i) );
                        }
                        // If the trait is Fn/FnMut, dereference the input value.
                        if( pe.trait.m_path == resolve.m_lang_FnOnce )
                            e.fcn = mv$(fcn_lvalue);
                        else
                            e.fcn = ::MIR::LValue::new_Deref( mv$(fcn_lvalue) );
                    }
                }
            }
        }

        mutator.flush_block();
    }


    // De-duplicate types
    {
        struct Visitor: MIR::visit::VisitorMut
        {
            std::set<HIR::TypeRef>  types;

            void dedup_type(HIR::TypeRef& ty)
            {
                auto it = types.find(ty);
                if( it != types.end() ) {
                    ty = HIR::TypeRef(*it);
                }
                else {
                    types.insert(HIR::TypeRef(ty));
                }
            }

            void visit_type(HIR::TypeRef& ty) override
            {
                dedup_type(ty);
            }
        } v;
        v.visit_function(state, fcn);
    }

}

void MIR_CleanupCrate(::HIR::Crate& crate)
{
    ::MIR::OuterVisitor    ov { crate, [&](const auto& res, const auto& p, ::HIR::ExprPtr& expr_ptr, const auto& args, const auto& ty){
            MIR_Cleanup(res, p, expr_ptr.get_mir_or_error_mut(Span()), args, ty);
        } };
    ov.visit_crate(crate);
}

