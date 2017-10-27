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
            auto    ty = ::HIR::TypeRef("", 0);
            for(size_t i = 0; i < str.m_params.m_types.size(); i++)
            {
                const auto& param = str.m_params.m_types[i];
                ty = ::HIR::TypeRef(param.m_name, i);
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

    void visit_trait(::HIR::ItemPath ip, ::HIR::Trait& tr) override
    {
        static Span sp;
        TRACE_FUNCTION_F(ip);

        // Enumerate supertraits and save for later stages
        struct Enumerate
        {
            ::std::vector< ::HIR::TraitPath>    supertraits;

            void enum_supertraits_in(const ::HIR::Trait& tr, ::HIR::GenericPath path, ::std::function<::HIR::TypeRef(const char*)> get_aty)
            {
                TRACE_FUNCTION_F(path);

                // Fill defaulted parameters.
                // NOTE: Doesn't do much error checking.
                if( path.m_params.m_types.size() != tr.m_params.m_types.size() )
                {
                    ASSERT_BUG(sp, path.m_params.m_types.size() < tr.m_params.m_types.size(), "");
                    for(unsigned int i = path.m_params.m_types.size(); i < tr.m_params.m_types.size(); i ++)
                    {
                        const auto& def = tr.m_params.m_types[i];
                        path.m_params.m_types.push_back( def.m_default.clone() );
                    }
                }

                ::HIR::TypeRef  ty_self { "Self", 0xFFFF };
                auto monomorph_cb = monomorphise_type_get_cb(sp, &ty_self, &path.m_params, nullptr);
                if( tr.m_all_parent_traits.size() > 0 )
                {
                    for(const auto& pt : tr.m_all_parent_traits)
                    {
                        supertraits.push_back( monomorphise_traitpath_with(sp, pt, monomorph_cb, false) );
                    }
                }
                else
                {
                    // Recurse into parent traits
                    for(const auto& pt : tr.m_parent_traits)
                    {
                        auto get_aty_this = [&](const char* name) {
                            auto it = pt.m_type_bounds.find(name);
                            if( it != pt.m_type_bounds.end() )
                                return monomorphise_type_with(sp, it->second, monomorph_cb);
                            return get_aty(name);
                            };
                        enum_supertraits_in(*pt.m_trait_ptr, monomorphise_genericpath_with(sp, pt.m_path, monomorph_cb, false), get_aty_this);
                    }
                    // - Bound parent traits
                    for(const auto& b : tr.m_params.m_bounds)
                    {
                        if( !b.is_TraitBound() )
                            continue;
                        const auto& be = b.as_TraitBound();
                        if( be.type != ::HIR::TypeRef("Self", 0xFFFF) )
                            continue;
                        const auto& pt = be.trait;
                        if( pt.m_path.m_path == path.m_path )
                            continue ;

                        auto get_aty_this = [&](const char* name) {
                            auto it = pt.m_type_bounds.find(name);
                            if( it != pt.m_type_bounds.end() )
                                return monomorphise_type_with(sp, it->second, monomorph_cb);
                            return get_aty(name);
                            };

                        enum_supertraits_in(*pt.m_trait_ptr, monomorphise_genericpath_with(sp, pt.m_path, monomorph_cb, false), get_aty_this);
                    }
                }


                // Build output path.
                ::HIR::TraitPath    out_path;
                out_path.m_path = mv$(path);
                out_path.m_trait_ptr = &tr;
                // - Locate associated types for this trait
                for(const auto& ty : tr.m_types)
                {
                    auto v = get_aty(ty.first.c_str());
                    if( v != ::HIR::TypeRef() )
                    {
                        out_path.m_type_bounds.insert( ::std::make_pair(ty.first, mv$(v)) );
                    }
                }
                // TODO: HRLs?
                supertraits.push_back( mv$(out_path) );
            }
        };

        auto this_path = ip.get_simple_path();
        this_path.m_crate_name = m_crate.m_crate_name;

        Enumerate   e;
        for(const auto& pt : tr.m_parent_traits)
        {
            auto get_aty = [&](const char* name) {
                auto it = pt.m_type_bounds.find(name);
                if( it != pt.m_type_bounds.end() )
                    return it->second.clone();
                return ::HIR::TypeRef();
                };
            e.enum_supertraits_in(*pt.m_trait_ptr, pt.m_path.clone(), get_aty);
        }
        for(const auto& b : tr.m_params.m_bounds)
        {
            if( !b.is_TraitBound() )
                continue;
            const auto& be = b.as_TraitBound();
            if( be.type != ::HIR::TypeRef("Self", 0xFFFF) )
                continue;
            const auto& pt = be.trait;

            // TODO: Remove this along with the from_ast.cpp hack
            if( pt.m_path.m_path == this_path )
            {
                // TODO: Should this restrict based on the parameters
                continue ;
            }

            auto get_aty = [&](const char* name) {
                auto it = be.trait.m_type_bounds.find(name);
                if( it != be.trait.m_type_bounds.end() )
                    return it->second.clone();
                return ::HIR::TypeRef();
                };
            e.enum_supertraits_in(*be.trait.m_trait_ptr, be.trait.m_path.clone(), get_aty);
        }

        ::std::sort(e.supertraits.begin(), e.supertraits.end());
        DEBUG("supertraits = " << e.supertraits);
        if( e.supertraits.size() > 0 )
        {
            bool dedeup_done = false;
            auto prev = e.supertraits.begin();
            for(auto it = e.supertraits.begin()+1; it != e.supertraits.end(); )
            {
                if( prev->m_path == it->m_path )
                {
                    if( *prev == *it ) {
                    }
                    else if( prev->m_type_bounds.size() == 0 ) {
                        ::std::swap(*prev, *it);
                    }
                    else if( it->m_type_bounds.size() == 0 ) {
                    }
                    else {
                        TODO(sp, "Merge associated types from " << *prev << " and " << *it);
                    }
                    it = e.supertraits.erase(it);
                    dedeup_done = true;
                }
                else
                {
                    ++ it;
                    ++ prev;
                }
            }
            if( dedeup_done ) {
                DEBUG("supertraits dd = " << e.supertraits);
            }
        }
        tr.m_all_parent_traits = mv$(e.supertraits);
    }

    ::HIR::StructMarkings::DstType get_field_dst_type(const ::HIR::TypeRef& ty, const ::HIR::GenericParams& inner_def, const ::HIR::GenericParams& params_def, const ::HIR::PathParams* params)
    {
        TRACE_FUNCTION_F("ty=" << ty);
        // If the type is generic, and the pointed-to parameters is ?Sized, record as needing unsize
        if( const auto* te = ty.m_data.opt_Generic() )
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
        else if( ty.m_data.is_Slice() )
        {
            return ::HIR::StructMarkings::DstType::Slice;
        }
        else if( ty.m_data.is_TraitObject() )
        {
            return ::HIR::StructMarkings::DstType::TraitObject;
        }
        else if( const auto* te = ty.m_data.opt_Path() )
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
                    auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr, params, nullptr);
                    auto params_mono = monomorphise_path_params_with(sp, params_tpl, monomorph_cb, false);
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
        return ::HIR::StructMarkings::DstType::None;
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
                    struct_markings.coerce_unsized_index = field;
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

class Visitor2:
    public ::HIR::Visitor
{
public:
    Visitor2()
    {
    }

    size_t get_unsize_param_idx(const Span& sp, const ::HIR::TypeRef& pointee) const
    {
        if( const auto* te = pointee.m_data.opt_Generic() )
        {
            return te->binding;
        }
        else if( const auto* te = pointee.m_data.opt_Path() )
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

        if( const auto* te = field_ty->m_data.opt_Path() )
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
        else if( const auto* te = field_ty->m_data.opt_Generic() )
        {
            out_param_idx = te->binding;
            return ::HIR::StructMarkings::Coerce::Passthrough;
        }
        else if( const auto* te = field_ty->m_data.opt_Pointer() )
        {
            out_param_idx = get_unsize_param_idx(sp, *te->inner);
            return ::HIR::StructMarkings::Coerce::Pointer;
        }
        else if( const auto* te = field_ty->m_data.opt_Borrow() )
        {
            out_param_idx = get_unsize_param_idx(sp, *te->inner);
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

