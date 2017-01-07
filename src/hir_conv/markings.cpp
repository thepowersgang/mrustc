/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/markings.cpp
 * - Fills the TraitMarkings structure on types
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
    const ::HIR::SimplePath&    m_lang_Deref;
    const ::HIR::SimplePath&    m_lang_Drop;
    const ::HIR::SimplePath&    m_lang_PhantomData;
public:
    Visitor(const ::HIR::Crate& crate):
        m_crate(crate),
        m_lang_Unsize( crate.get_lang_item_path_opt("unsize") ),
        m_lang_CoerceUnsized( crate.get_lang_item_path_opt("coerce_unsized") ),
        m_lang_Deref( crate.get_lang_item_path_opt("deref") ),
        m_lang_Drop( crate.get_lang_item_path_opt("drop") ),
        m_lang_PhantomData( crate.get_lang_item_path_opt("phantom_data") )
    {
    }

    void visit_struct(::HIR::ItemPath ip, ::HIR::Struct& str) override
    {
        ::HIR::Visitor::visit_struct(ip, str);

        str.m_markings.dst_type = get_struct_dst_type(str, str.m_params, {});
        if( str.m_markings.dst_type != ::HIR::TraitMarkings::DstType::None )
        {
            str.m_markings.unsized_field = (str.m_data.is_Tuple() ? str.m_data.as_Tuple().size()-1 : str.m_data.as_Named().size()-1);
        }
    }

    ::HIR::TraitMarkings::DstType get_field_dst_type(const ::HIR::TypeRef& ty, const ::HIR::GenericParams& inner_def, const ::HIR::GenericParams& params_def, const ::HIR::PathParams* params)
    {
        // If the type is generic, and the pointed-to parameters is ?Sized, record as needing unsize
        if( const auto* te = ty.m_data.opt_Generic() )
        {
            if( inner_def.m_types.at(te->binding).m_is_sized == true )
            {
                return ::HIR::TraitMarkings::DstType::None;
            }
            else if( params )
            {
                // Look at the param. Check for generic (use params_def), slice/traitobject, or path (no mono)
                return get_field_dst_type(params->m_types.at(te->binding), params_def, params_def, nullptr);
            }
            else
            {
                return ::HIR::TraitMarkings::DstType::Possible;
            }
        }
        else if( ty.m_data.is_Slice() )
        {
            return ::HIR::TraitMarkings::DstType::Slice;
        }
        else if( ty.m_data.is_TraitObject() )
        {
            return ::HIR::TraitMarkings::DstType::TraitObject;
        }
        else if( const auto* te = ty.m_data.opt_Path() )
        {
            // If the type is a struct, check it (recursively)
            if( ! te->path.m_data.is_Generic() ) {
                // Associated type, TODO: Check this better.
                return ::HIR::TraitMarkings::DstType::Possible;
            }
            else if( te->binding.is_Struct() ) {
                const auto& params_tpl = te->path.m_data.as_Generic().m_params;
                if( params && monomorphise_pathparams_needed(params_tpl) ) {
                    static Span sp;
                    auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr, params, nullptr);
                    auto params_mono = monomorphise_path_params_with(sp, params_tpl, monomorph_cb, false);
                    return get_struct_dst_type(*te->binding.as_Struct(), params_def, &params_mono);
                }
                else {
                    return get_struct_dst_type(*te->binding.as_Struct(), inner_def, &params_tpl);
                }
            }
            else {
                return ::HIR::TraitMarkings::DstType::None;
            }
        }
        else
        {
            return ::HIR::TraitMarkings::DstType::None;
        }
    }
    ::HIR::TraitMarkings::DstType get_struct_dst_type(const ::HIR::Struct& str, const ::HIR::GenericParams& def, const ::HIR::PathParams* params)
    {
        TU_MATCHA( (str.m_data), (se),
        (Unit,
            ),
        (Tuple,
            // TODO: Ensure that only the last field is ?Sized
            if( se.size() > 0 )
            {
                return get_field_dst_type(se.back().ent, str.m_params, def, params);
            }
            ),
        (Named,
            // Check the last field in the struct.
            // - If it is Sized, leave as-is (struct is marked as Sized)
            // - If it is known unsized, record the type
            // - If it is a ?Sized parameter, mark as possible and record index for MIR

            // TODO: Ensure that only the last field is ?Sized
            if( se.size() > 0 )
            {
                return get_field_dst_type(se.back().second.ent, str.m_params, def, params);
            }
            )
        )
        return ::HIR::TraitMarkings::DstType::None;
    }

    void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
    {
        static Span sp;

        ::HIR::Visitor::visit_trait_impl(trait_path, impl);

        if( impl.m_type.m_data.is_Path() )
        {
            const auto& te = impl.m_type.m_data.as_Path();
            const ::HIR::TraitMarkings* markings_ptr = nullptr;
            TU_MATCHA( (te.binding), (tpb),
            (Unbound, ),
            (Opaque, ),
            (Struct, markings_ptr = &tpb->m_markings; ),
            (Union , markings_ptr = &tpb->m_markings; ),
            (Enum  , markings_ptr = &tpb->m_markings; )
            )
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
                    if( markings_ptr->coerce_unsized_index != ~0u )
                        ERROR(sp, E0000, "CoerceUnsized can only be implemented once per struct");

                    DEBUG("Type " << impl.m_type << " can Coerce");
                    if( impl.m_trait_args.m_types.size() != 1 )
                        ERROR(sp, E0000, "Unexpected number of arguments for CoerceUnsized");
                    const auto& dst_ty = impl.m_trait_args.m_types[0];
                    // Determine which field is the one that does the coerce
                    if( !te.binding.is_Struct() )
                        ERROR(sp, E0000, "Cannot implement CoerceUnsized on non-structs");
                    if( !dst_ty.m_data.is_Path() )
                        ERROR(sp, E0000, "Cannot implement CoerceUnsized from non-structs");
                    const auto& dst_te = dst_ty.m_data.as_Path();
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

                    auto monomorph_cb_l = monomorphise_type_get_cb(sp, nullptr, &dst_te.path.m_data.as_Generic().m_params, nullptr);
                    auto monomorph_cb_r = monomorphise_type_get_cb(sp, nullptr, &te.path.m_data.as_Generic().m_params, nullptr);

                    TU_MATCHA( (str->m_data), (se),
                    (Unit,
                        ),
                    (Tuple,
                        for(unsigned int i = 0; i < se.size(); i ++)
                        {
                            // If the data is PhantomData, ignore it.
                            TU_IFLET(::HIR::TypeRef::Data, se[i].ent.m_data, Path, ite,
                                TU_IFLET(::HIR::Path::Data, ite.path.m_data, Generic, pe,
                                    if( pe.m_path == m_lang_PhantomData )
                                        continue ;
                                )
                            )
                            if( monomorphise_type_needed(se[i].ent) ) {
                                auto ty_l = monomorphise_type_with(sp, se[i].ent, monomorph_cb_l, false);
                                auto ty_r = monomorphise_type_with(sp, se[i].ent, monomorph_cb_r, false);
                                if( ty_l != ty_r ) {
                                    if( field != ~0u )
                                        ERROR(sp, E0000, "CoerceUnsized impls can only differ by one field");
                                    field = i;
                                }
                            }
                        }
                        ),
                    (Named,
                        for(unsigned int i = 0; i < se.size(); i ++)
                        {
                            // If the data is PhantomData, ignore it.
                            TU_IFLET(::HIR::TypeRef::Data, se[i].second.ent.m_data, Path, ite,
                                TU_IFLET(::HIR::Path::Data, ite.path.m_data, Generic, pe,
                                    if( pe.m_path == m_lang_PhantomData )
                                        continue ;
                                )
                            )
                            if( monomorphise_type_needed(se[i].second.ent) ) {
                                auto ty_l = monomorphise_type_with(sp, se[i].second.ent, monomorph_cb_l, false);
                                auto ty_r = monomorphise_type_with(sp, se[i].second.ent, monomorph_cb_r, false);
                                if( ty_l != ty_r ) {
                                    if( field != ~0u )
                                        ERROR(sp, E0000, "CoerceUnsized impls can only differ by one field");
                                    field = i;
                                }
                            }
                        }
                        )
                    )
                    if( field == ~0u )
                        ERROR(sp, E0000, "CoerceUnsized requires a field to differ between source and destination");
                    markings.coerce_unsized_index = field;
                }
                else if( trait_path == m_lang_Deref ) {
                    DEBUG("Type " << impl.m_type << " can Deref");
                    markings.has_a_deref = true;
                }
                // TODO: Marker traits (with conditions)
                else {
                }
            }
        }
    }
};

}   // namespace

void ConvertHIR_Markings(::HIR::Crate& crate)
{
    Visitor exp { crate };
    exp.visit_crate( crate );
}

