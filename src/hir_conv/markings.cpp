/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/markings.cpp
 * - Fills the TraitMarkings structure on types as well as other metadata
 */
#include "main_bindings.hpp"
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <algorithm>    // std::find_if

#include <hir_typeck/static.hpp>

namespace {

class Visitor:
    public ::HIR::Visitor
{
    const ::HIR::Crate& m_crate;
    const ::HIR::SimplePath&    m_lang_Unsize;
    const ::HIR::SimplePath&    m_lang_CoerceUnsized;
    const ::HIR::SimplePath&    m_lang_Copy;
    const ::HIR::SimplePath&    m_lang_Deref;
    const ::HIR::SimplePath&    m_lang_Drop;
    const ::HIR::SimplePath&    m_lang_PhantomData;
public:
    Visitor(const ::HIR::Crate& crate):
        m_crate(crate),
        m_lang_Unsize( crate.get_lang_item_path_opt("unsize") ),
        m_lang_CoerceUnsized( crate.get_lang_item_path_opt("coerce_unsized") ),
        m_lang_Copy( crate.get_lang_item_path_opt("copy") ),
        m_lang_Deref( crate.get_lang_item_path_opt("deref") ),
        m_lang_Drop( crate.get_lang_item_path_opt("drop") ),
        m_lang_PhantomData( crate.get_lang_item_path_opt("phantom_data") )
    {
    }

    void visit_struct(::HIR::ItemPath ip, ::HIR::Struct& str) override
    {
        ::HIR::Visitor::visit_struct(ip, str);

        str.m_struct_markings.dst_type = get_struct_dst_type(str, str.m_params, {});
        if( str.m_struct_markings.dst_type != ::HIR::StructMarkings::DstType::None )
        {
            str.m_struct_markings.unsized_field = (str.m_data.is_Tuple() ? str.m_data.as_Tuple().size()-1 : str.m_data.as_Named().size()-1);
        }

        // Rules:
        // - A type parameter must be ?Sized
        // - That type parameter must only be used as part of the last field, and only once
        // - If the final field isn't the parameter, it must also impl Unsize

        // HACK: Just determine what ?Sized parameter is controlling the sized-ness
        if( str.m_struct_markings.dst_type == ::HIR::StructMarkings::DstType::Possible )
        {
            auto& last_field_ty = (str.m_data.is_Tuple() ? str.m_data.as_Tuple().back().ent : str.m_data.as_Named().back().second.ent);
            for(size_t i = 0; i < str.m_params.m_types.size(); i++)
            {
                const auto& param = str.m_params.m_types[i];
                auto ty = ::HIR::TypeRef(param.m_name, i);
                if( !param.m_is_sized )
                {
                    if( visit_ty_with(last_field_ty, [&](const auto& t){ return t == ty; }) )
                    {
                        ASSERT_BUG(Span(), str.m_struct_markings.unsized_param == ~0u, "Multiple unsized params to " << ip);
                        str.m_struct_markings.unsized_param = i;
                    }
                }
            }
            ASSERT_BUG(Span(), str.m_struct_markings.unsized_param != ~0u, "No unsized param for type " << ip);
            str.m_struct_markings.can_unsize = true;
        }
    }

    ::HIR::StructMarkings::DstType get_field_dst_type(const ::HIR::TypeRef& ty, const ::HIR::GenericParams& inner_def, const ::HIR::GenericParams& params_def, const ::HIR::PathParams* params)
    {
        TRACE_FUNCTION_F("ty=" << ty);
        // If the type is generic, and the pointed-to parameters is ?Sized, record as needing unsize
        if( const auto* te = ty.data().opt_Generic() )
        {
            if( inner_def.m_types.at(te->binding).m_is_sized == true )
            {
                return ::HIR::StructMarkings::DstType::None;
            }
            else if( params )
            {
                // Look at the param. Check for generic (use params_def), slice/traitobject, or path (no mono)
                return get_field_dst_type(params->m_types.at(te->binding), params_def, params_def, nullptr);
            }
            else
            {
                return ::HIR::StructMarkings::DstType::Possible;
            }
        }
        else if( ty.data().is_Slice() || TU_TEST1(ty.data(), Primitive, == HIR::CoreType::Str) )
        {
            return ::HIR::StructMarkings::DstType::Slice;
        }
        else if( ty.data().is_TraitObject() )
        {
            return ::HIR::StructMarkings::DstType::TraitObject;
        }
        else if( const auto* te = ty.data().opt_Path() )
        {
            // If the type is a struct, check it (recursively)
            if( ! te->path.m_data.is_Generic() ) {
                // Associated type, TODO: Check this better.
                return ::HIR::StructMarkings::DstType::None;
            }
            else if( te->binding.is_Struct() ) {
                const auto& params_tpl = te->path.m_data.as_Generic().m_params;
                if( params && monomorphise_pathparams_needed(params_tpl) ) {
                    static Span sp;
                    auto monomorph_cb = MonomorphStatePtr(nullptr, params, nullptr);
                    auto params_mono = monomorph_cb.monomorph_path_params(sp, params_tpl, false);
                    return get_struct_dst_type(*te->binding.as_Struct(), params_def, &params_mono);
                }
                else {
                    return get_struct_dst_type(*te->binding.as_Struct(), inner_def, &params_tpl);
                }
            }
            else {
                return ::HIR::StructMarkings::DstType::None;
            }
        }
        else
        {
            return ::HIR::StructMarkings::DstType::None;
        }
    }
    ::HIR::StructMarkings::DstType get_struct_dst_type(const ::HIR::Struct& str, const ::HIR::GenericParams& def, const ::HIR::PathParams* params)
    {
        TU_MATCH_HDRA( (str.m_data), {)
        TU_ARMA(Unit, se) {
            }
        TU_ARMA(Tuple, se) {
            // TODO: Ensure that only the last field is ?Sized
            if( se.size() > 0 )
            {
                return get_field_dst_type(se.back().ent, str.m_params, def, params);
            }
            }
        TU_ARMA(Named, se) {
            // Check the last field in the struct.
            // - If it is Sized, leave as-is (struct is marked as Sized)
            // - If it is known unsized, record the type
            // - If it is a ?Sized parameter, mark as possible and record index for MIR

            // TODO: Ensure that only the last field is ?Sized
            if( se.size() > 0 )
            {
                return get_field_dst_type(se.back().second.ent, str.m_params, def, params);
            }
            }
        }
        return ::HIR::StructMarkings::DstType::None;
    }

    void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
    {
        static Span sp;

        ::HIR::Visitor::visit_trait_impl(trait_path, impl);

        if( impl.m_type.data().is_Path() )
        {
            const auto& te = impl.m_type.data().as_Path();
            const ::HIR::TraitMarkings* markings_ptr = te.binding.get_trait_markings();
            if( markings_ptr )
            {
                ::HIR::TraitMarkings& markings = *const_cast<::HIR::TraitMarkings*>(markings_ptr);
                if( trait_path == m_lang_Unsize ) {
                    DEBUG("Type " << impl.m_type << " can Unsize");
                    ERROR(sp, E0000, "Unsize shouldn't be manually implemented");
                }
                else if( trait_path == m_lang_Drop )
                {
                    // TODO: Check that there's only one impl, and that it covers the same set as the type.
                    markings.has_drop_impl = true;
                }
                else if( trait_path == m_lang_CoerceUnsized ) {
                    auto& struct_markings = const_cast<::HIR::Struct*>(te.binding.as_Struct())->m_struct_markings;
                    if( struct_markings.coerce_unsized_index != ~0u )
                        ERROR(sp, E0000, "CoerceUnsized can only be implemented once per struct");

                    DEBUG("Type " << impl.m_type << " can Coerce");
                    if( impl.m_trait_args.m_types.size() != 1 )
                        ERROR(sp, E0000, "Unexpected number of arguments for CoerceUnsized");
                    const auto& dst_ty = impl.m_trait_args.m_types[0];
                    // Determine which field is the one that does the coerce
                    if( !te.binding.is_Struct() )
                        ERROR(sp, E0000, "Cannot implement CoerceUnsized on non-structs");
                    if( !dst_ty.data().is_Path() )
                        ERROR(sp, E0000, "Cannot implement CoerceUnsized from non-structs");
                    const auto& dst_te = dst_ty.data().as_Path();
                    if( !dst_te.binding.is_Struct() )
                        ERROR(sp, E0000, "Cannot implement CoerceUnsized from non-structs");
                    if( dst_te.binding.as_Struct() != te.binding.as_Struct() )
                        ERROR(sp, E0000, "CoerceUnsized can only be implemented between variants of the same struct");

                    // NOTES: (from IRC: eddyb)
                    // < eddyb> they're required that T and U are the same struct definition (with different type parameters) and exactly one field differs in type between T and U (ignoring PhantomData)
                    // < eddyb> Mutabah: I forgot to mention that the field that differs in type must also impl CoerceUnsized

                    // Determine the difference in monomorphised variants.
                    unsigned int field = ~0u;
                    const auto& str = te.binding.as_Struct();

                    auto monomorph_cb_l = MonomorphStatePtr(nullptr, &dst_te.path.m_data.as_Generic().m_params, nullptr);
                    auto monomorph_cb_r = MonomorphStatePtr(nullptr, &te.path.m_data.as_Generic().m_params, nullptr);

                    TU_MATCH_HDRA( (str->m_data), {)
                    TU_ARMA(Unit, se) {
                        }
                    TU_ARMA(Tuple, se) {
                        for(unsigned int i = 0; i < se.size(); i ++)
                        {
                            // If the data is PhantomData, ignore it.
                            if( TU_TEST2(se[i].ent.data(), Path, .path.m_data, Generic, .m_path == m_lang_PhantomData) )
                            {
                                continue ;
                            }
                            if( monomorphise_type_needed(se[i].ent) ) {
                                auto ty_l = monomorph_cb_l.monomorph_type(sp, se[i].ent, false);
                                auto ty_r = monomorph_cb_r.monomorph_type(sp, se[i].ent, false);
                                if( ty_l != ty_r ) {
                                    if( field != ~0u )
                                        ERROR(sp, E0000, "CoerceUnsized impls can only differ by one field");
                                    field = i;
                                }
                            }
                        }
                        }
                    TU_ARMA(Named, se) {
                        for(unsigned int i = 0; i < se.size(); i ++)
                        {
                            // If the data is PhantomData, ignore it.
                            if( TU_TEST2(se[i].second.ent.data(), Path, .path.m_data, Generic, .m_path == m_lang_PhantomData) )
                            {
                                continue ;
                            }
                            if( monomorphise_type_needed(se[i].second.ent) ) {
                                auto ty_l = monomorph_cb_l.monomorph_type(sp, se[i].second.ent, false);
                                auto ty_r = monomorph_cb_r.monomorph_type(sp, se[i].second.ent, false);
                                if( ty_l != ty_r ) {
                                    if( field != ~0u )
                                        ERROR(sp, E0000, "CoerceUnsized impls can only differ by one field");
                                    field = i;
                                }
                            }
                        }
                        }
                    }
                    if( field == ~0u )
                        ERROR(sp, E0000, "CoerceUnsized requires a field to differ between source and destination");
                    struct_markings.coerce_unsized_index = field;
                }
                else if( trait_path == m_lang_Deref ) {
                    DEBUG("Type " << impl.m_type << " can Deref");
                    markings.has_a_deref = true;
                }
                else if( trait_path == m_lang_Copy ) {
                    DEBUG("Type " << impl.m_type << " has a Copy impl");
                    markings.is_copy = true;
                }
                // TODO: Marker traits (with conditions)
                else {
                }
            }
        }
    }
};

class Visitor2:
    public ::HIR::Visitor
{
public:
    Visitor2()
    {
    }

    size_t get_unsize_param_idx(const Span& sp, const ::HIR::TypeRef& pointee) const
    {
        if( const auto* te = pointee.data().opt_Generic() )
        {
            return te->binding;
        }
        else if( const auto* te = pointee.data().opt_Path() )
        {
            ASSERT_BUG(sp, te->binding.is_Struct(), "Pointer to non-Unsize type - " << pointee);
            const auto& ism = te->binding.as_Struct()->m_struct_markings;
            ASSERT_BUG(sp, ism.unsized_param != ~0u, "Pointer to non-Unsize type - " << pointee);
            const auto& gp = te->path.m_data.as_Generic();
            return get_unsize_param_idx(sp, gp.m_params.m_types.at(ism.unsized_param));
        }
        else
        {
            BUG(sp, "Pointer to non-Unsize type? - " << pointee);
        }
    }
    ::HIR::StructMarkings::Coerce get_coerce_type(const Span& sp, ::HIR::ItemPath ip, const ::HIR::Struct& str, size_t& out_param_idx) const
    {
        if( str.m_struct_markings.coerce_unsized_index == ~0u )
            return ::HIR::StructMarkings::Coerce::None;
        if( str.m_struct_markings.coerce_unsized != ::HIR::StructMarkings::Coerce::None )
        {
            out_param_idx = str.m_struct_markings.coerce_param;
            return str.m_struct_markings.coerce_unsized;
        }

        const ::HIR::TypeRef*   field_ty = nullptr;
        TU_MATCHA( (str.m_data), (se),
        (Unit,
            ),
        (Tuple,
            field_ty = &se.at(str.m_struct_markings.coerce_unsized_index).ent;
            ),
        (Named,
            field_ty = &se.at(str.m_struct_markings.coerce_unsized_index).second.ent;
            )
        )
        assert(field_ty);
    try_again:
        DEBUG("field_ty = " << *field_ty);

        if( const auto* te = field_ty->data().opt_Path() )
        {
            ASSERT_BUG(sp, te->binding.is_Struct(), "CoerceUnsized impl differs on Path that isn't a struct - " << ip << " fld=" << *field_ty);
            const auto* istr = te->binding.as_Struct();
            const auto& gp = te->path.m_data.as_Generic();

            size_t inner_idx = 0;
            auto inner_type = get_coerce_type(sp, {*field_ty}, *istr, inner_idx);
            ASSERT_BUG(sp, inner_type != ::HIR::StructMarkings::Coerce::None, "CoerceUnsized impl differs on a non-CoerceUnsized type - " << ip << " fld=" << *field_ty);

            const auto& param_ty = gp.m_params.m_types.at( inner_idx );
            switch(inner_type)
            {
            case ::HIR::StructMarkings::Coerce::None:
                throw "";
            case ::HIR::StructMarkings::Coerce::Passthrough:
                // Recurse on the generic type.
                field_ty = &param_ty;
                goto try_again;
            case ::HIR::StructMarkings::Coerce::Pointer:
                out_param_idx = get_unsize_param_idx(sp, param_ty);
                return ::HIR::StructMarkings::Coerce::Pointer;
            }
        }
        else if( const auto* te = field_ty->data().opt_Generic() )
        {
            out_param_idx = te->binding;
            return ::HIR::StructMarkings::Coerce::Passthrough;
        }
        else if( const auto* te = field_ty->data().opt_Pointer() )
        {
            out_param_idx = get_unsize_param_idx(sp, te->inner);
            return ::HIR::StructMarkings::Coerce::Pointer;
        }
        else if( const auto* te = field_ty->data().opt_Borrow() )
        {
            out_param_idx = get_unsize_param_idx(sp, te->inner);
            return ::HIR::StructMarkings::Coerce::Pointer;
        }
        else
        {
            TODO(sp, "Handle CoerceUnsized type " << *field_ty);
        }
        BUG(sp, "Reached end of get_coerce_type - " << *field_ty);
    }

    void visit_struct(::HIR::ItemPath ip, ::HIR::Struct& str) override
    {
        static Span sp;

        auto& struct_markings = str.m_struct_markings;
        if( struct_markings.coerce_unsized_index == ~0u ) {
            return ;
        }

        size_t  idx = 0;
        auto cut = get_coerce_type(sp, ip, str, idx);
        struct_markings.coerce_param = idx;
        struct_markings.coerce_unsized = cut;
    }
};

}   // namespace

void ConvertHIR_Markings(::HIR::Crate& crate)
{
    Visitor exp { crate };
    exp.visit_crate( crate );

    // Visit again, visiting all structs and filling the coerce_unsized data
    Visitor2 exp2 { /*crate*/ };
    exp2.visit_crate( crate );
}

