/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/common.cpp
 * - Typecheck common code
 */
#include "common.hpp"
#include <hir/path.hpp>

struct TyVisitorCbConst: TyVisitor<WConst>
{
    t_cb_visit_ty   callback;
    bool visit_type(const ::HIR::TypeRef& ty) override
    {
        if(callback(ty)) {
            return true;
        }
        return TyVisitor::visit_type(ty);
    }
};
bool visit_ty_with(const ::HIR::TypeRef& ty, t_cb_visit_ty callback)
{
    TyVisitorCbConst v;
    v.callback = callback;
    return v.visit_type(ty);
}
bool visit_path_tys_with(const ::HIR::Path& path, t_cb_visit_ty callback)
{
    TyVisitorCbConst v;
    v.callback = callback;
    return v.visit_path(path);
}

/*
struct TyVisitorCbMut: TyVisitor<WMut>
{
    t_cb_visit_ty_mut   callback;
    bool visit_type(::HIR::TypeRef& ty) override
    {
        if(callback(ty)) {
            return true;
        }
        return TyVisitor::visit_type(ty);
    }
};
bool visit_ty_with_mut(::HIR::TypeRef& ty, t_cb_visit_ty_mut callback)
{
    TyVisitorCbMut v;
    v.callback = callback;
    return v.visit_type(ty);
}
bool visit_path_tys_with_mut(::HIR::Path& path, t_cb_visit_ty_mut callback)
{
    TyVisitorCbMut v;
    v.callback = callback;
    return v.visit_path(path);
}
*/

bool monomorphise_pathparams_needed(const ::HIR::PathParams& tpl)
{
    TyVisitorCbConst v;
    v.callback = [&](const auto& ty) { return (ty.data().is_Generic() ? true : false); };
    return v.visit_path_params(tpl);
}
bool monomorphise_traitpath_needed(const ::HIR::TraitPath& tpl)
{
    TyVisitorCbConst v;
    v.callback = [&](const auto& ty) { return (ty.data().is_Generic() ? true : false); };
    return v.visit_trait_path(tpl);
}
bool monomorphise_path_needed(const ::HIR::Path& tpl)
{
    return visit_path_tys_with(tpl, [&](const auto& ty) {
        return (ty.data().is_Generic() ? true : false);
        });
}
bool monomorphise_type_needed(const ::HIR::TypeRef& tpl)
{
    return visit_ty_with(tpl, [&](const auto& ty) {
        return (ty.data().is_Generic() ? true : false);
        });
}


::HIR::PathParams clone_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_clone_ty callback) {
    ::HIR::PathParams   rv;
    rv.m_types.reserve( tpl.m_types.size() );
    for( const auto& ty : tpl.m_types)
        rv.m_types.push_back( clone_ty_with(sp, ty, callback) );
    return rv;
}
::HIR::GenericPath clone_ty_with__generic_path(const Span& sp, const ::HIR::GenericPath& tpl, t_cb_clone_ty callback) {
    return ::HIR::GenericPath( tpl.m_path, clone_path_params_with(sp, tpl.m_params, callback) );
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
    TU_MATCH_HDRA( (tpl.m_data), {)
    TU_ARMA(Generic, e2) {
        return ::HIR::Path( clone_ty_with__generic_path(sp, e2, callback) );
        }
    TU_ARMA(UfcsKnown, e2) {
        return ::HIR::Path::Data::make_UfcsKnown({
            clone_ty_with(sp, e2.type, callback),
            clone_ty_with__generic_path(sp, e2.trait, callback),
            e2.item,
            clone_path_params_with(sp, e2.params, callback)
            });
        }
    TU_ARMA(UfcsUnknown, e2) {
        return ::HIR::Path::Data::make_UfcsUnknown({
            clone_ty_with(sp, e2.type, callback),
            e2.item,
            clone_path_params_with(sp, e2.params, callback)
            });
        }
    TU_ARMA(UfcsInherent, e2) {
        return ::HIR::Path::Data::make_UfcsInherent({
            clone_ty_with(sp, e2.type, callback),
            e2.item,
            clone_path_params_with(sp, e2.params, callback),
            clone_path_params_with(sp, e2.impl_params, callback)
            });
        }
    }
    throw "";
}
::HIR::TypeRef clone_ty_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_clone_ty callback)
{
    ::HIR::TypeRef  rv;

    if( callback(tpl, rv) ) {
        //DEBUG(tpl << " => " << rv);
        return rv;
    }

    TU_MATCH_HDRA( (tpl.data()), {)
    TU_ARMA(Infer, e) {
        rv = ::HIR::TypeRef(e);
        }
    TU_ARMA(Diverge, e) {
        rv = ::HIR::TypeRef(e);
        }
    TU_ARMA(Primitive, e) {
        rv = ::HIR::TypeRef(e);
        }
    TU_ARMA(Path, e) {
        rv = ::HIR::TypeRef( ::HIR::TypeData::Data_Path {
            clone_ty_with__path(sp, e.path, callback),
            e.binding.clone()
            } );
        // If the input binding was Opaque, AND the type changed, clear it back to Unbound
        if( e.binding.is_Opaque() /*&& rv != tpl*/ ) {
            // NOTE: The replacement can be Self=Self, which should trigger a binding clear.
            rv.get_unique().as_Path().binding = ::HIR::TypePathBinding();
        }
        }
    TU_ARMA(Generic, e) {
        rv = ::HIR::TypeRef(e);
        }
    TU_ARMA(TraitObject, e) {
        ::HIR::TypeData::Data_TraitObject  to;
        to.m_trait = clone_ty_with__trait_path(sp, e.m_trait, callback);
        for(const auto& trait : e.m_markers)
        {
            to.m_markers.push_back( clone_ty_with__generic_path(sp, trait, callback) );
        }
        to.m_lifetime = e.m_lifetime;
        rv = ::HIR::TypeRef( mv$(to) );
        }
    TU_ARMA(ErasedType, e) {
        auto origin = clone_ty_with__path(sp, e.m_origin, callback);

        ::std::vector< ::HIR::TraitPath>    traits;
        traits.reserve( e.m_traits.size() );
        for(const auto& trait : e.m_traits)
            traits.push_back( clone_ty_with__trait_path(sp, trait, callback) );

        rv = ::HIR::TypeRef( ::HIR::TypeData::Data_ErasedType {
            mv$(origin), e.m_index,
            mv$(traits),
            e.m_lifetime
            } );
        }
    TU_ARMA(Array, e) {
        rv = ::HIR::TypeRef( ::HIR::TypeData::make_Array({ clone_ty_with(sp, e.inner, callback), e.size.clone() }) );
        }
    TU_ARMA(Slice, e) {
        rv = ::HIR::TypeRef::new_slice( clone_ty_with(sp, e.inner, callback) );
        }
    TU_ARMA(Tuple, e) {
        ::std::vector< ::HIR::TypeRef>  types;
        for(const auto& ty : e) {
            types.push_back( clone_ty_with(sp, ty, callback) );
        }
        rv = ::HIR::TypeRef( mv$(types) );
        }
    TU_ARMA(Borrow, e) {
        rv = ::HIR::TypeRef::new_borrow (e.type, clone_ty_with(sp, e.inner, callback));
        }
    TU_ARMA(Pointer, e) {
        rv = ::HIR::TypeRef::new_pointer(e.type, clone_ty_with(sp, e.inner, callback));
        }
    TU_ARMA(Function, e) {
        ::HIR::FunctionType ft;
        ft.is_unsafe = e.is_unsafe;
        ft.m_abi = e.m_abi;
        ft.m_rettype = clone_ty_with(sp, e.m_rettype, callback);
        for( const auto& arg : e.m_arg_types )
            ft.m_arg_types.push_back( clone_ty_with(sp, arg, callback) );
        rv = ::HIR::TypeRef( mv$(ft) );
        }
    TU_ARMA(Closure, e) {
        ::HIR::TypeData::Data_Closure  oe;
        oe.node = e.node;
        oe.m_rettype = clone_ty_with(sp, e.m_rettype, callback);
        for(const auto& a : e.m_arg_types)
            oe.m_arg_types.push_back( clone_ty_with(sp, a, callback) );
        rv = ::HIR::TypeRef( mv$(oe) );
        }
    }
    return rv;
}

namespace {
    template<typename T>
    t_cb_clone_ty monomorphise_type_with__closure(const Span& sp, const T& outer_tpl, t_cb_generic& callback, bool allow_infer)
    {
        return [&sp,&outer_tpl,callback,allow_infer](const auto& tpl, auto& rv) {
            if( tpl.data().is_Infer() && !allow_infer )
               BUG(sp, "_ type found in " << outer_tpl);

            if( tpl.data().is_Generic() ) {
                rv = callback(tpl).clone();
                return true;
            }

            return false;
            };
    }
}

::HIR::PathParams monomorphise_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_generic callback, bool allow_infer)
{
    return clone_path_params_with(sp, tpl, monomorphise_type_with__closure(sp, tpl, callback, allow_infer));
}
::HIR::GenericPath monomorphise_genericpath_with(const Span& sp, const ::HIR::GenericPath& tpl, t_cb_generic callback, bool allow_infer)
{
    return clone_ty_with__generic_path(sp, tpl, monomorphise_type_with__closure(sp, tpl, callback, allow_infer));
}
::HIR::TraitPath monomorphise_traitpath_with(const Span& sp, const ::HIR::TraitPath& tpl, t_cb_generic callback, bool allow_infer)
{
    return clone_ty_with__trait_path(sp, tpl, monomorphise_type_with__closure(sp, tpl, callback, allow_infer));
}
::HIR::Path monomorphise_path_with(const Span& sp, const ::HIR::Path& tpl, t_cb_generic callback, bool allow_infer)
{
    return clone_ty_with__path(sp, tpl, monomorphise_type_with__closure(sp, tpl, callback, allow_infer));
}
::HIR::TypeRef monomorphise_type_with_inner(const Span& sp, const ::HIR::TypeRef& outer_tpl, t_cb_generic callback, bool allow_infer)
{
    return clone_ty_with(sp, outer_tpl, monomorphise_type_with__closure(sp, outer_tpl, callback, allow_infer));
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
    ASSERT_BUG(sp, params.m_types.size() == params_def.m_types.size(),
        "Parameter count mismatch - exp " << params_def.m_types.size() << ", got " << params.m_types.size() << " for " << params << " and " << params_def.fmt_args());
    return monomorphise_type_with(sp, tpl, [&](const auto& gt)->const auto& {
        const auto& e = gt.data().as_Generic();
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

t_cb_generic MonomorphState::get_cb(const Span& sp) const
{
    return monomorphise_type_get_cb(sp, this->self_ty, this->pp_impl, this->pp_method);
}
::std::ostream& operator<<(::std::ostream& os, const MonomorphState& ms)
{
    os << "MonomorphState {";
    if(ms.self_ty)
        os << " self=" << *ms.self_ty;
    if(ms.pp_impl)
        os << " I=" << *ms.pp_impl;
    if(ms.pp_method)
        os << " M=" << *ms.pp_method;
    os << " }";
    return os;
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
        case ::HIR::CoreType::I128:  case ::HIR::CoreType::U128:
        case ::HIR::CoreType::Isize: case ::HIR::CoreType::Usize:
            break;
        default:
            ERROR(sp, E0000, "Type unificiation of integer literal with non-integer - " << type);
        }
        break;
    }
}
