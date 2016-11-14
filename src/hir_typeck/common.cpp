/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/common.cpp
 * - Typecheck common code
 */
#include "common.hpp"
#include <hir/path.hpp>

bool monomorphise_type_needed(const ::HIR::TypeRef& tpl);
::HIR::TypeRef monomorphise_type_with_inner(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_generic callback, bool allow_infer);

bool monomorphise_pathparams_needed(const ::HIR::PathParams& tpl)
{
    for(const auto& ty : tpl.m_types)
        if( monomorphise_type_needed(ty) )
            return true;
    return false;
}
bool monomorphise_path_needed(const ::HIR::Path& tpl)
{
    TU_MATCH(::HIR::Path::Data, (tpl.m_data), (e),
    (Generic,
        return monomorphise_pathparams_needed(e.m_params);
        ),
    (UfcsInherent,
        return monomorphise_type_needed(*e.type) || monomorphise_pathparams_needed(e.params);
        ),
    (UfcsKnown,
        return monomorphise_type_needed(*e.type) || monomorphise_pathparams_needed(e.trait.m_params) || monomorphise_pathparams_needed(e.params);
        ),
    (UfcsUnknown,
        return monomorphise_type_needed(*e.type) || monomorphise_pathparams_needed(e.params);
        )
    )
    throw "";
}
bool monomorphise_traitpath_needed(const ::HIR::TraitPath& tpl)
{
    if( monomorphise_pathparams_needed(tpl.m_path.m_params) )    return true;
    for(const auto& assoc : tpl.m_type_bounds)
        if( monomorphise_type_needed(assoc.second) )
            return true;
    return false;
}
bool monomorphise_type_needed(const ::HIR::TypeRef& tpl)
{
    TU_MATCH(::HIR::TypeRef::Data, (tpl.m_data), (e),
    (Infer,
        BUG(Span(), "_ type found in monomorphisation target - " << tpl);
        ),
    (Diverge,
        return false;
        ),
    (Primitive,
        return false;
        ),
    (Path,
        return monomorphise_path_needed(e.path);
        ),
    (Generic,
        return true;
        ),
    (TraitObject,
        if( monomorphise_traitpath_needed(e.m_trait) )
            return true;
        for(const auto& trait : e.m_markers)
            if( monomorphise_pathparams_needed(trait.m_params) )
                return true;
        return false;
        ),
    (ErasedType,
        if( monomorphise_path_needed(e.m_origin) )
            return true;
        for(const auto& trait : e.m_traits)
            if( monomorphise_traitpath_needed(trait) )
                return true;
        return false;
        ),
    (Array,
        return monomorphise_type_needed(*e.inner);
        ),
    (Slice,
        return monomorphise_type_needed(*e.inner);
        ),
    (Tuple,
        for(const auto& ty : e) {
            if( monomorphise_type_needed(ty) )
                return true;
        }
        return false;
        ),
    (Borrow,
        return monomorphise_type_needed(*e.inner);
        ),
    (Pointer,
        return monomorphise_type_needed(*e.inner);
        ),
    (Function,
        for(const auto& ty : e.m_arg_types) {
            if( monomorphise_type_needed(ty) )
                return true;
        }
        return monomorphise_type_needed(*e.m_rettype);
        ),
    (Closure,
        for(const auto& ty : e.m_arg_types) {
            if( monomorphise_type_needed(ty) )
                return true;
        }
        return monomorphise_type_needed(*e.m_rettype);
        )
    )
    throw "";
}

::HIR::PathParams clone_ty_with__path_params(const Span& sp, const ::HIR::PathParams& tpl, t_cb_clone_ty callback) {
    ::HIR::PathParams   rv;
    rv.m_types.reserve( tpl.m_types.size() );
    for( const auto& ty : tpl.m_types) 
        rv.m_types.push_back( clone_ty_with(sp, ty, callback) );
    return rv;
}
::HIR::GenericPath clone_ty_with__generic_path(const Span& sp, const ::HIR::GenericPath& tpl, t_cb_clone_ty callback) {
    return ::HIR::GenericPath( tpl.m_path, clone_ty_with__path_params(sp, tpl.m_params, callback) );
}
::HIR::TraitPath clone_ty_with__trait_path(const Span& sp, const ::HIR::TraitPath& tpl, t_cb_clone_ty callback) {
    ::HIR::TraitPath    rv {
        clone_ty_with__generic_path(sp, tpl.m_path, callback),
        tpl.m_hrls,
        {},
        tpl.m_trait_ptr
        };
    
    for(const auto& assoc : tpl.m_type_bounds) {
        rv.m_type_bounds.insert(::std::make_pair(
            assoc.first,
            clone_ty_with(sp, assoc.second, callback)
            ));
    }
    
    return rv;
}
::HIR::Path clone_ty_with__path(const Span& sp, const ::HIR::Path& tpl, t_cb_clone_ty callback) {
    TU_MATCH(::HIR::Path::Data, (tpl.m_data), (e2),
    (Generic,
        return ::HIR::Path( clone_ty_with__generic_path(sp, e2, callback) );
        ),
    (UfcsKnown,
        return ::HIR::Path::Data::make_UfcsKnown({
            box$( clone_ty_with(sp, *e2.type, callback) ),
            clone_ty_with__generic_path(sp, e2.trait, callback),
            e2.item,
            clone_ty_with__path_params(sp, e2.params, callback)
            });
        ),
    (UfcsUnknown,
        return ::HIR::Path::Data::make_UfcsUnknown({
            box$( clone_ty_with(sp, *e2.type, callback) ),
            e2.item,
            clone_ty_with__path_params(sp, e2.params, callback)
            });
        ),
    (UfcsInherent,
        TODO(sp, "UfcsInherent - " << tpl);
        )
    )
    throw "";
}
::HIR::TypeRef clone_ty_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_clone_ty callback)
{
    ::HIR::TypeRef  rv;
    
    if( callback(tpl, rv) ) {
        DEBUG(tpl << " => " << rv);
        return rv;
    }
    
    TU_MATCH(::HIR::TypeRef::Data, (tpl.m_data), (e),
    (Infer,
        rv = ::HIR::TypeRef(e);
        ),
    (Diverge,
        rv = ::HIR::TypeRef(e);
        ),
    (Primitive,
        rv = ::HIR::TypeRef(e);
        ),
    (Path,
        rv = ::HIR::TypeRef( ::HIR::TypeRef::Data::Data_Path {
            clone_ty_with__path(sp, e.path, callback),
            e.binding.clone()
            } );
        // If the input binding was Opaque, clear it back to Unbound
        if( e.binding.is_Opaque() ) {
            rv.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding();
        }
        ),
    (Generic,
        rv = ::HIR::TypeRef(e);
        ),
    (TraitObject,
        ::HIR::TypeRef::Data::Data_TraitObject  to;
        to.m_trait = clone_ty_with__trait_path(sp, e.m_trait, callback);
        for(const auto& trait : e.m_markers)
        {
            to.m_markers.push_back( clone_ty_with__generic_path(sp, trait, callback) ); 
        }
        to.m_lifetime = e.m_lifetime;
        rv = ::HIR::TypeRef( mv$(to) );
        ),
    (ErasedType,
        auto origin = clone_ty_with__path(sp, e.m_origin, callback);
        
        ::std::vector< ::HIR::TraitPath>    traits;
        traits.reserve( e.m_traits.size() );
        for(const auto& trait : e.m_traits)
            traits.push_back( clone_ty_with__trait_path(sp, trait, callback) );
        
        rv = ::HIR::TypeRef( ::HIR::TypeRef::Data::Data_ErasedType {
            mv$(origin), e.m_index,
            mv$(traits),
            e.m_lifetime
            } );
        ),
    (Array,
        if( e.size_val == ~0u ) {
            BUG(sp, "Attempting to clone array with unknown size - " << tpl);
        }
        rv = ::HIR::TypeRef::new_array( clone_ty_with(sp, *e.inner, callback), e.size_val );
        ),
    (Slice,
        rv = ::HIR::TypeRef::new_slice( clone_ty_with(sp, *e.inner, callback) );
        ),
    (Tuple,
        ::std::vector< ::HIR::TypeRef>  types;
        for(const auto& ty : e) {
            types.push_back( clone_ty_with(sp, ty, callback) );
        }
        rv = ::HIR::TypeRef( mv$(types) );
        ),
    (Borrow,
        rv = ::HIR::TypeRef::new_borrow (e.type, clone_ty_with(sp, *e.inner, callback));
        ),
    (Pointer,
        rv = ::HIR::TypeRef::new_pointer(e.type, clone_ty_with(sp, *e.inner, callback));
        ),
    (Function,
        ::HIR::FunctionType ft;
        ft.is_unsafe = e.is_unsafe;
        ft.m_abi = e.m_abi;
        ft.m_rettype = box$( clone_ty_with(sp, *e.m_rettype, callback) );
        for( const auto& arg : e.m_arg_types )
            ft.m_arg_types.push_back( clone_ty_with(sp, arg, callback) );
        rv = ::HIR::TypeRef( mv$(ft) );
        ),
    (Closure,
        ::HIR::TypeRef::Data::Data_Closure  oe;
        oe.node = e.node;
        oe.m_rettype = box$( clone_ty_with(sp, *e.m_rettype, callback) );
        for(const auto& a : e.m_arg_types)
            oe.m_arg_types.push_back( clone_ty_with(sp, a, callback) );
        rv = ::HIR::TypeRef( mv$(oe) );
        )
    )
    return rv;
}

::HIR::PathParams monomorphise_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_generic callback, bool allow_infer)
{
    ::HIR::PathParams   rv;
    for( const auto& ty : tpl.m_types) 
        rv.m_types.push_back( monomorphise_type_with_inner(sp, ty, callback, allow_infer) );
    return rv;
}
::HIR::GenericPath monomorphise_genericpath_with(const Span& sp, const ::HIR::GenericPath& tpl, t_cb_generic callback, bool allow_infer)
{
    return ::HIR::GenericPath( tpl.m_path, monomorphise_path_params_with(sp, tpl.m_params, callback, allow_infer) );
}
::HIR::TraitPath monomorphise_traitpath_with(const Span& sp, const ::HIR::TraitPath& tpl, t_cb_generic callback, bool allow_infer)
{
    ::HIR::TraitPath    rv {
        monomorphise_genericpath_with(sp, tpl.m_path, callback, allow_infer),
        tpl.m_hrls,
        {},
        tpl.m_trait_ptr
        };
    
    for(const auto& assoc : tpl.m_type_bounds)
        rv.m_type_bounds.insert(::std::make_pair( assoc.first, monomorphise_type_with_inner(sp, assoc.second, callback, allow_infer) ));
    
    return rv;
}
::HIR::Path monomorphise_path_with(const Span& sp, const ::HIR::Path& tpl, t_cb_generic callback, bool allow_infer)
{
    TU_MATCH(::HIR::Path::Data, (tpl.m_data), (e2),
    (Generic,
        return ::HIR::Path( monomorphise_genericpath_with(sp, e2, callback, allow_infer) );
        ),
    (UfcsKnown,
        return ::HIR::Path::Data::make_UfcsKnown({
            box$( monomorphise_type_with_inner(sp, *e2.type, callback, allow_infer) ),
            monomorphise_genericpath_with(sp, e2.trait, callback, allow_infer),
            e2.item,
            monomorphise_path_params_with(sp, e2.params, callback, allow_infer)
            });
        ),
    (UfcsUnknown,
        return ::HIR::Path::Data::make_UfcsUnknown({
            box$( monomorphise_type_with_inner(sp, *e2.type, callback, allow_infer) ),
            e2.item,
            monomorphise_path_params_with(sp, e2.params, callback, allow_infer)
            });
        ),
    (UfcsInherent,
        TODO(sp, "UfcsInherent - " << tpl);
        )
    )
    throw "";
}
::HIR::TypeRef monomorphise_type_with_inner(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_generic callback, bool allow_infer)
{
    return clone_ty_with(sp, tpl, [&](const auto& tpl, auto& rv) {
        if( tpl.m_data.is_Infer() && !allow_infer )
           BUG(sp, "_ type found in monomorphisation target");
        
        if( tpl.m_data.is_Generic() ) {
            rv = callback(tpl).clone();
            return true;
        }
        
        return false;
        });
}
::HIR::TypeRef monomorphise_type_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_generic callback, bool allow_infer)
{
    ::HIR::TypeRef  rv;
    TRACE_FUNCTION_FR("tpl = " << tpl, rv);
    rv = monomorphise_type_with_inner(sp, tpl, callback, allow_infer);
    return rv;
}

// TODO: Rework this function to support all three classes not just impl-level
// NOTE: This is _just_ for impl-level parameters
::HIR::TypeRef monomorphise_type(const Span& sp, const ::HIR::GenericParams& params_def, const ::HIR::PathParams& params,  const ::HIR::TypeRef& tpl)
{
    DEBUG("tpl = " << tpl);
    ASSERT_BUG(sp, params.m_types.size() == params_def.m_types.size(), "Parameter count mismatch - exp " << params_def.m_types.size() << ", got " << params.m_types.size());
    return monomorphise_type_with(sp, tpl, [&](const auto& gt)->const auto& {
        const auto& e = gt.m_data.as_Generic();
        if( e.binding == 0xFFFF ) {
            TODO(sp, "Handle 'Self' in `monomorphise_type`");
        }
        else if( (e.binding >> 8) == 0 ) {
            auto idx = e.binding & 0xFF;
            if( idx >= params.m_types.size() ) {
                BUG(sp, "Generic param out of input range - " << gt << " >= " << params.m_types.size());
            }
            return params.m_types[idx];
        }
        else if( (e.binding >> 8) == 1 ) {
            TODO(sp, "Handle fn-level params in `monomorphise_type` - " << gt);
        }
        else {
            BUG(sp, "Unknown param in `monomorphise_type` - " << gt);
        }
        }, false);
}

void check_type_class_primitive(const Span& sp, const ::HIR::TypeRef& type, ::HIR::InferClass ic, ::HIR::CoreType ct)
{
    switch(ic)
    {
    case ::HIR::InferClass::None:
    case ::HIR::InferClass::Diverge:
        break;
    case ::HIR::InferClass::Float:
        switch(ct)
        {
        case ::HIR::CoreType::F32:
        case ::HIR::CoreType::F64:
            break;
        default:
            ERROR(sp, E0000, "Type unificiation of integer literal with non-integer - " << type);
        }
        break;
    case ::HIR::InferClass::Integer:
        switch(ct)
        {
        case ::HIR::CoreType::I8:    case ::HIR::CoreType::U8:
        case ::HIR::CoreType::I16:   case ::HIR::CoreType::U16:
        case ::HIR::CoreType::I32:   case ::HIR::CoreType::U32:
        case ::HIR::CoreType::I64:   case ::HIR::CoreType::U64:
        case ::HIR::CoreType::Isize: case ::HIR::CoreType::Usize:
            break;
        default:
            ERROR(sp, E0000, "Type unificiation of integer literal with non-integer - " << type);
        }
        break;
    }
}
