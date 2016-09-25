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
        assert(!"ERROR: _ type found in monomorphisation target");
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

::HIR::PathParams monomorphise_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_generic callback, bool allow_infer)
{
    ::HIR::PathParams   rv;
    for( const auto& ty : tpl.m_types) 
        rv.m_types.push_back( monomorphise_type_with(sp, ty, callback, allow_infer) );
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
        rv.m_type_bounds.insert(::std::make_pair( assoc.first, monomorphise_type_with(sp, assoc.second, callback, allow_infer) ));
    
    return rv;
}
::HIR::TypeRef monomorphise_type_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_generic callback, bool allow_infer)
{
    ::HIR::TypeRef  rv;
    TRACE_FUNCTION_FR("tpl = " << tpl, rv);
    TU_MATCH(::HIR::TypeRef::Data, (tpl.m_data), (e),
    (Infer,
        if( allow_infer ) {
            rv = ::HIR::TypeRef(e);
        }
        else {
           BUG(sp, "_ type found in monomorphisation target");
        }
        ),
    (Diverge,
        rv = ::HIR::TypeRef(e);
        ),
    (Primitive,
        rv = ::HIR::TypeRef(e);
        ),
    (Path,
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
        (Generic,
            rv = ::HIR::TypeRef( ::HIR::TypeRef::Data::Data_Path {
                    monomorphise_genericpath_with(sp, e2, callback, allow_infer),
                    e.binding.clone()
                    } );
            ),
        (UfcsKnown,
            rv = ::HIR::TypeRef( ::HIR::Path::Data::make_UfcsKnown({
                box$( monomorphise_type_with(sp, *e2.type, callback, allow_infer) ),
                monomorphise_genericpath_with(sp, e2.trait, callback, allow_infer),
                e2.item,
                monomorphise_path_params_with(sp, e2.params, callback, allow_infer)
                }) );
            ),
        (UfcsUnknown,
            rv = ::HIR::TypeRef( ::HIR::Path::Data::make_UfcsUnknown({
                box$( monomorphise_type_with(sp, *e2.type, callback, allow_infer) ),
                e2.item,
                monomorphise_path_params_with(sp, e2.params, callback, allow_infer)
                }) );
            ),
        (UfcsInherent,
            TODO(sp, "UfcsInherent - " << tpl);
            )
        )
        ),
    (Generic,
        rv = callback(tpl).clone();
        ),
    (TraitObject,
        ::HIR::TypeRef::Data::Data_TraitObject  to;
        to.m_trait = monomorphise_traitpath_with(sp, e.m_trait, callback, allow_infer);
        for(const auto& trait : e.m_markers)
        {
            to.m_markers.push_back( monomorphise_genericpath_with(sp, trait, callback, allow_infer) ); 
        }
        to.m_lifetime = e.m_lifetime;
        rv = ::HIR::TypeRef( mv$(to) );
        ),
    (Array,
        if( e.size_val == ~0u ) {
            BUG(sp, "Attempting to clone array with unknown size - " << tpl);
        }
        rv = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Array({
            box$( monomorphise_type_with(sp, *e.inner, callback) ),
            ::HIR::ExprPtr(),
            e.size_val
            }) );
        ),
    (Slice,
        rv = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Slice({ box$(monomorphise_type_with(sp, *e.inner, callback)) }) );
        ),
    (Tuple,
        ::std::vector< ::HIR::TypeRef>  types;
        for(const auto& ty : e) {
            types.push_back( monomorphise_type_with(sp, ty, callback) );
        }
        rv = ::HIR::TypeRef( mv$(types) );
        ),
    (Borrow,
        rv = ::HIR::TypeRef::new_borrow(e.type, monomorphise_type_with(sp, *e.inner, callback));
        ),
    (Pointer,
        rv = ::HIR::TypeRef::new_pointer(e.type, monomorphise_type_with(sp, *e.inner, callback));
        ),
    (Function,
        ::HIR::FunctionType ft;
        ft.is_unsafe = e.is_unsafe;
        ft.m_abi = e.m_abi;
        ft.m_rettype = box$( monomorphise_type_with(sp, *e.m_rettype, callback) );
        for( const auto& arg : e.m_arg_types )
            ft.m_arg_types.push_back( monomorphise_type_with(sp, arg, callback) );
        rv = ::HIR::TypeRef( mv$(ft) );
        ),
    (Closure,
        ::HIR::TypeRef::Data::Data_Closure  oe;
        oe.node = e.node;
        oe.m_rettype = box$( monomorphise_type_with(sp, *e.m_rettype, callback) );
        for(const auto& a : e.m_arg_types)
            oe.m_arg_types.push_back( monomorphise_type_with(sp, a, callback) );
        rv = ::HIR::TypeRef(::HIR::TypeRef::Data::make_Closure( mv$(oe) ));
        )
    )
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
