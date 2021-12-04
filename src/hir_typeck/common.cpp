/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/common.cpp
 * - Typecheck common code
 */
#include "common.hpp"
#include <hir/path.hpp>
#include "trans/target.hpp"


template<typename I>
struct WConst {
    typedef const I T;
};
template<typename I>
struct WMut {
    typedef I T;
};
template<template<typename> class W>
struct TyVisitor
{
    const LList<const HIR::TypeRef*>*  m_cur_recurse_stack = nullptr;

    virtual typename W<HIR::TypeData>::T& get_ty_data(typename W<HIR::TypeRef>::T& ty) const = 0;

    virtual bool visit_path_params(typename W<::HIR::PathParams>::T& tpl)
    {
        for(auto& ty : tpl.m_types)
            if( visit_type(ty) )
                return true;
        return false;
    }

    virtual bool visit_trait_path(typename W<::HIR::TraitPath>::T& tpl)
    {
        if( visit_path_params(tpl.m_path.m_params) )
            return true;
        for(auto& assoc : tpl.m_type_bounds)
        {
            visit_path_params(assoc.second.source_trait.m_params);
            if( visit_type(assoc.second.type) )
                return true;
        }
        for(auto& assoc : tpl.m_trait_bounds)
        {
            visit_path_params(assoc.second.source_trait.m_params);
            for(auto& t : assoc.second.traits)
                visit_trait_path(t);
        }
        return false;
    }
    virtual bool visit_path(typename W<HIR::Path>::T& path)
    {
        TU_MATCH_HDRA((path.m_data), {)
        TU_ARMA(Generic, e) {
            return visit_path_params(e.m_params);
            }
        TU_ARMA(UfcsInherent, e) {
            return visit_type(e.type) || visit_path_params(e.params);
            }
        TU_ARMA(UfcsKnown, e) {
            return visit_type(e.type) || visit_path_params(e.trait.m_params) || visit_path_params(e.params);
            }
        TU_ARMA(UfcsUnknown, e) {
            return visit_type(e.type) || visit_path_params(e.params);
            }
        }
        throw "";
    }
    virtual bool visit_type(typename W<HIR::TypeRef>::T& ty)
    {
        if(m_cur_recurse_stack) {
            for(const auto* p : *m_cur_recurse_stack) {
                if( p == &ty ) {
                    return false;
                }
            }
        }
        struct _ {
            typedef LList<const HIR::TypeRef*> stack_t;
            const stack_t*&  dst;
            stack_t stack;
            _(const stack_t*& dst, const HIR::TypeRef& ty): dst(dst), stack(dst, &ty) {
                dst = &stack;
            }
            ~_() {
                dst = stack.m_prev;
            }
        } h(m_cur_recurse_stack, ty);
        TU_MATCH_HDRA( (this->get_ty_data(ty)), {)
        TU_ARMA(Infer, e) {
            }
        TU_ARMA(Diverge, e) {
            }
        TU_ARMA(Primitive, e) {
            }
        TU_ARMA(Generic, e) {
            }
        TU_ARMA(Path, e) {
            return visit_path(e.path);
            }
        TU_ARMA(TraitObject, e) {
            if( visit_trait_path(e.m_trait) )
                return true;
            for(auto& trait : e.m_markers)
                if( visit_path_params(trait.m_params) )
                    return true;
            return false;
            }
        TU_ARMA(ErasedType, e) {
            if( visit_path(e.m_origin) )
                return true;
            for(auto& trait : e.m_traits)
                if( visit_trait_path(trait) )
                    return true;
            return false;
            }
        TU_ARMA(Array, e) {
            return visit_type(e.inner);
            }
        TU_ARMA(Slice, e) {
            return visit_type(e.inner);
            }
        TU_ARMA(Tuple, e) {
            for(auto& ty : e) {
                if( visit_type(ty) )
                    return true;
            }
            return false;
            }
        TU_ARMA(Borrow, e) {
            return visit_type(e.inner);
            }
        TU_ARMA(Pointer, e) {
            return visit_type(e.inner);
            }
        TU_ARMA(Function, e) {
            for(auto& ty : e.m_arg_types) {
                if( visit_type(ty) )
                    return true;
            }
            return visit_type(e.m_rettype);
            }
        TU_ARMA(Closure, e) {
            for(auto& ty : e.m_arg_types) {
                if( visit_type(ty) )
                    return true;
            }
            return visit_type(e.m_rettype);
            }
        TU_ARMA(Generator, e) {
            // Visits?
            }
        }
        return false;
    }
};



struct TyVisitorCbConst: TyVisitor<WConst>
{
    t_cb_visit_ty   callback;
    const HIR::TypeData& get_ty_data(const HIR::TypeRef& ty) const override {
        return ty.data();
    }
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

// /*
struct TyVisitorCbMut: TyVisitor<WMut>
{
    t_cb_visit_ty_mut   callback;
    HIR::TypeData& get_ty_data(HIR::TypeRef& ty) const override {
        return ty.data_mut();
    }
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
// */


struct TyVisitorMonomorphNeeded: TyVisitor<WConst>
{
    const HIR::TypeData& get_ty_data(const HIR::TypeRef& ty) const override {
        return ty.data();
    }
    bool visit_path_params(const ::HIR::PathParams& pp) override
    {
        for(const auto& v : pp.m_values) {
            if(v.is_Generic())
                return true;
        }
        return TyVisitor::visit_path_params(pp);
    }
    bool visit_type(const ::HIR::TypeRef& ty) override
    {
        if( ty.data().is_Generic() )
            return true;
        if( ty.data().is_Array() && ty.data().as_Array().size.is_Unevaluated() /*&& ty.data().as_Array().size.as_Unevaluated().*/ )
            return true;
        return TyVisitor::visit_type(ty);
    }
};
bool monomorphise_pathparams_needed(const ::HIR::PathParams& tpl)
{
    TyVisitorMonomorphNeeded v;
    return v.visit_path_params(tpl);
}
bool monomorphise_traitpath_needed(const ::HIR::TraitPath& tpl)
{
    TyVisitorMonomorphNeeded v;
    return v.visit_trait_path(tpl);
}
bool monomorphise_path_needed(const ::HIR::Path& tpl)
{
    TyVisitorMonomorphNeeded v;
    return v.visit_path(tpl);
}
bool monomorphise_type_needed(const ::HIR::TypeRef& tpl)
{
    TyVisitorMonomorphNeeded v;
    return v.visit_type(tpl);
}


::HIR::TypeRef Monomorphiser::monomorph_type(const Span& sp, const ::HIR::TypeRef& tpl, bool allow_infer/*=true*/) const
{
    TU_MATCH_HDRA( (tpl.data()), {)
    TU_ARMA(Infer, e) {
        ASSERT_BUG(sp, allow_infer, "Unexpected ivar seen - " << tpl);
        return ::HIR::TypeRef(e);
        }
    TU_ARMA(Diverge, e) {
        return ::HIR::TypeRef(e);
        }
    TU_ARMA(Primitive, e) {
        return ::HIR::TypeRef(e);
        }
    TU_ARMA(Path, e) {
        auto rv = ::HIR::TypeRef( ::HIR::TypeData::Data_Path {
            this->monomorph_path(sp, e.path, allow_infer),
            e.binding.clone()
            } );
        // If the input binding was Opaque
        if( e.binding.is_Opaque() ) {
            // NOTE: The replacement can be Self=Self, which should trigger a binding clear.
            rv.get_unique().as_Path().binding = ::HIR::TypePathBinding();
        }
        return rv;
        }
    TU_ARMA(Generic, e) {
        return this->get_type(sp, e);
        }
    TU_ARMA(TraitObject, e) {
        ::HIR::TypeData::Data_TraitObject  to;
        to.m_trait = this->monomorph_traitpath(sp, e.m_trait, allow_infer);
        for(const auto& trait : e.m_markers)
        {
            to.m_markers.push_back( this->monomorph_genericpath(sp, trait, allow_infer) );
        }
        to.m_lifetime = e.m_lifetime;
        return ::HIR::TypeRef( mv$(to) );
        }
    TU_ARMA(ErasedType, e) {
        auto origin = this->monomorph_path(sp, e.m_origin, allow_infer);

        ::std::vector< ::HIR::TraitPath>    traits;
        traits.reserve( e.m_traits.size() );
        for(const auto& trait : e.m_traits)
            traits.push_back( this->monomorph_traitpath(sp, trait, allow_infer) );

        return ::HIR::TypeRef( ::HIR::TypeData::Data_ErasedType {
            mv$(origin), e.m_index,
            e.m_is_sized,
            mv$(traits),
            e.m_lifetime
            } );
        }
    TU_ARMA(Array, e) {
        return ::HIR::TypeRef( ::HIR::TypeData::make_Array({
            this->monomorph_type(sp, e.inner, allow_infer),
            this->monomorph_arraysize(sp, e.size)
            }) );
        }
    TU_ARMA(Slice, e) {
        return ::HIR::TypeRef::new_slice( this->monomorph_type(sp, e.inner, allow_infer) );
        }
    TU_ARMA(Tuple, e) {
        ::std::vector< ::HIR::TypeRef>  types;
        for(const auto& ty : e) {
            types.push_back( this->monomorph_type(sp, ty, allow_infer) );
        }
        return ::HIR::TypeRef( mv$(types) );
        }
    TU_ARMA(Borrow, e) {
        return ::HIR::TypeRef::new_borrow (e.type, this->monomorph_type(sp, e.inner, allow_infer));
        }
    TU_ARMA(Pointer, e) {
        return ::HIR::TypeRef::new_pointer(e.type, this->monomorph_type(sp, e.inner, allow_infer));
        }
    TU_ARMA(Function, e) {
        ::HIR::FunctionType ft;
        ft.is_unsafe = e.is_unsafe;
        ft.m_abi = e.m_abi;
        ft.m_rettype = this->monomorph_type(sp, e.m_rettype, allow_infer);
        for( const auto& arg : e.m_arg_types )
            ft.m_arg_types.push_back( this->monomorph_type(sp, arg, allow_infer) );
        return ::HIR::TypeRef( mv$(ft) );
        }
    TU_ARMA(Closure, e) {
        ::HIR::TypeData::Data_Closure  oe;
        oe.node = e.node;
        oe.m_rettype = this->monomorph_type(sp, e.m_rettype, allow_infer);
        for(const auto& arg : e.m_arg_types)
            oe.m_arg_types.push_back( this->monomorph_type(sp, arg, allow_infer) );
        return ::HIR::TypeRef( mv$(oe) );
        }
    TU_ARMA(Generator, e) {
        TODO(sp, "Monomorphising a generator type?");
        }
    }
    throw "";
}
::HIR::Path Monomorphiser::monomorph_path(const Span& sp, const ::HIR::Path& tpl, bool allow_infer/*=true*/) const
{
    TU_MATCH_HDRA( (tpl.m_data), {)
    TU_ARMA(Generic, e2) {
        return ::HIR::Path( this->monomorph_genericpath(sp, e2, allow_infer) );
        }
    TU_ARMA(UfcsKnown, e2) {
        return ::HIR::Path::Data::make_UfcsKnown({
            this->monomorph_type(sp, e2.type, allow_infer),
            this->monomorph_genericpath(sp, e2.trait, allow_infer),
            e2.item,
            this->monomorph_path_params(sp, e2.params, allow_infer)
            });
        }
    TU_ARMA(UfcsUnknown, e2) {
        return ::HIR::Path::Data::make_UfcsUnknown({
            this->monomorph_type(sp, e2.type, allow_infer),
            e2.item,
            this->monomorph_path_params(sp, e2.params, allow_infer)
            });
        }
    TU_ARMA(UfcsInherent, e2) {
        return ::HIR::Path::Data::make_UfcsInherent({
            this->monomorph_type(sp, e2.type, allow_infer),
            e2.item,
            this->monomorph_path_params(sp, e2.params, allow_infer),
            this->monomorph_path_params(sp, e2.impl_params, allow_infer)
            });
        }
    }
    throw "";
}
::HIR::TraitPath Monomorphiser::monomorph_traitpath(const Span& sp, const ::HIR::TraitPath& tpl, bool allow_infer) const
{
    ::HIR::TraitPath    rv {
        this->monomorph_genericpath(sp, tpl.m_path, allow_infer),
        tpl.m_hrls,
        {},
        {},
        tpl.m_trait_ptr
        };

    for(const auto& assoc : tpl.m_type_bounds) {
        rv.m_type_bounds.insert(::std::make_pair(
            assoc.first,
            HIR::TraitPath::AtyEqual {
                this->monomorph_genericpath(sp, assoc.second.source_trait, allow_infer),
                this->monomorph_type(sp, assoc.second.type, allow_infer)
                }
            ));
    }
    for(const auto& assoc : tpl.m_trait_bounds) {
        auto v = HIR::TraitPath::AtyBound { this->monomorph_genericpath(sp, assoc.second.source_trait, allow_infer), {} };
        for(const auto& trait : assoc.second.traits)
            v.traits.push_back( monomorph_traitpath(sp, trait, allow_infer) );
        rv.m_trait_bounds.insert(::std::make_pair( assoc.first, std::move(v) ));
    }

    return rv;
}
::HIR::PathParams Monomorphiser::monomorph_path_params(const Span& sp, const ::HIR::PathParams& tpl, bool allow_infer) const
{
    ::HIR::PathParams   rv;
    rv.m_types.reserve( tpl.m_types.size() );
    for( const auto& ty : tpl.m_types)
        rv.m_types.push_back( this->monomorph_type(sp, ty, allow_infer) );

    rv.m_values.reserve( tpl.m_values.size() );
    for( const auto& val : tpl.m_values)
    {
        if(const auto* ge = val.opt_Generic())
        {
            rv.m_values.push_back( this->get_value(sp, *ge) );
        }
        else
        {
            rv.m_values.push_back( val.clone() );
        }
    }

    return rv;
}
::HIR::GenericPath Monomorphiser::monomorph_genericpath(const Span& sp, const ::HIR::GenericPath& tpl, bool allow_infer) const
{
    return ::HIR::GenericPath( tpl.m_path, this->monomorph_path_params(sp, tpl.m_params, allow_infer) );
}

::HIR::ArraySize Monomorphiser::monomorph_arraysize(const Span& sp, const ::HIR::ArraySize& tpl) const
{
    if( auto* se = tpl.opt_Unevaluated() ) {
        HIR::ArraySize  sz;
        if( se->is_Generic() ) {
            sz = this->get_value(sp, se->as_Generic());
            DEBUG(tpl << " -> " << sz);
            se = sz.opt_Unevaluated();
            assert(se);
        }

        if(se->is_Unevaluated()) {
            // TODO: Evaluate
            DEBUG("Evaluate unevaluated generic for array size - " << *se);
            return se->clone();
        }
        else if( se->is_Evaluated() ) {
            return se->as_Evaluated()->read_usize(0);
        }
        else {
            return se->clone();
        }
    }
    else {
        return tpl.clone();
    }
}


::HIR::PathParams clone_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_clone_ty callback) {
    ::HIR::PathParams   rv;
    rv.m_types.reserve( tpl.m_types.size() );
    for( const auto& ty : tpl.m_types)
        rv.m_types.push_back( clone_ty_with(sp, ty, callback) );
    for( const auto& v : tpl.m_values)
        rv.m_values.push_back( v.clone() );
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
        {},
        tpl.m_trait_ptr
        };

    for(const auto& assoc : tpl.m_type_bounds) {
        rv.m_type_bounds.insert(::std::make_pair(
            assoc.first,
            HIR::TraitPath::AtyEqual {
                clone_ty_with__generic_path(sp, assoc.second.source_trait, callback),
                clone_ty_with(sp, assoc.second.type, callback)
            }
            ));
    }
    for(const auto& assoc : tpl.m_trait_bounds) {
        auto& traits = rv.m_trait_bounds.insert(::std::make_pair( assoc.first,
                HIR::TraitPath::AtyBound { clone_ty_with__generic_path(sp, assoc.second.source_trait, callback), {} }
            ) ).first->second.traits;

        for(const auto& t : assoc.second.traits)
            traits.push_back(clone_ty_with__trait_path(sp, t, callback));
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
            e.m_is_sized,
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
    TU_ARMA(Generator, e) {
        TODO(sp, "Cloning a generator type?");
        }
    }
    return rv;
}


::HIR::TypeRef MonomorphiserPP::get_type(const Span& sp, const ::HIR::GenericRef& ty) const /*override*/
{
    if(ty.is_self())
    {
        if( const auto* s = this->get_self_type() )
        {
            return s->clone();
        }
        else
        {
            BUG(sp, "Unexpected Self");
        }
    }
    else
    {
        switch(ty.group())
        {
        case 0:
            if( const auto* p = this->get_impl_params() )
            {
                ASSERT_BUG(sp, ty.idx() < p->m_types.size(), "Type param " << ty << " out of range for (max " << p->m_types.size() << ")");
                return p->m_types[ty.idx()].clone();
            }
            else
            {
                BUG(sp, "Impl parameters were not expected (got " << ty << ")");
            }
            break;
        case 1:
            if( const auto* p = this->get_method_params() )
            {
                ASSERT_BUG(sp, ty.idx() < p->m_types.size(), "Type param " << ty << " out of range for (max " << p->m_types.size() << ")");
                return p->m_types[ty.idx()].clone();
            }
            else
            {
                BUG(sp, "Method parameters were not expected (got " << ty << ")");
            }
            break;
        default:
            BUG(sp, "Unexpected type param " << ty);
        }
    }
}
::HIR::ConstGeneric MonomorphiserPP::get_value(const Span& sp, const ::HIR::GenericRef& val) const /*override*/
{
    switch(val.group())
    {
    case 0:
        if( const auto* p = this->get_impl_params() )
        {
            ASSERT_BUG(sp, val.idx() < p->m_values.size(), "Value param " << val << " out of range for (max " << p->m_values.size() << ")");
            return p->m_values[val.idx()].clone();
        }
        else
        {
            BUG(sp, "Impl parameters were not expected (got " << val << ")");
        }
        break;
    case 1:
        if( const auto* p = this->get_method_params() )
        {
            ASSERT_BUG(sp, val.idx() < p->m_values.size(), "Value param " << val << " out of range for (max " << p->m_values.size() << ")");
            return p->m_values[val.idx()].clone();
        }
        else
        {
            BUG(sp, "Method parameters were not expected (got " << val << ")");
        }
        break;
    default:
        BUG(sp, "Unexpected value param " << val);
    }
}

//t_cb_generic MonomorphState::get_cb(const Span& sp) const
//{
//    return monomorphise_type_get_cb(sp, this->self_ty, this->pp_impl, this->pp_method);
//}
::std::ostream& operator<<(::std::ostream& os, const MonomorphState& ms)
{
    os << "MonomorphState {";
    if(ms.self_ty != HIR::TypeRef())
        os << " self=" << ms.self_ty;
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
