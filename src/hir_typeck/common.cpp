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
#include <hir_conv/main_bindings.hpp>

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
            for(auto& trait : e.m_traits)
                if( visit_trait_path(trait) )
                    return true;
            TU_MATCH_HDRA( (e.m_inner), {)
            TU_ARMA(Fcn, ee) {
                if( visit_path(ee.m_origin) ) {
                    return true;
                }
                }
            TU_ARMA(Known, ee) {
                if( visit_type(ee) ) {
                    return true;
                }
                }
            TU_ARMA(Alias, ee) {
                }
            }
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
        TU_ARMA(NamedFunction, e) {
            return visit_path(e.path);
            }
        TU_ARMA(Function, e) {
            for(auto& ty : e.m_arg_types) {
                if( visit_type(ty) )
                    return true;
            }
            return visit_type(e.m_rettype);
            }
        TU_ARMA(Closure, e) {
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
bool visit_trait_path_tys_with(const ::HIR::TraitPath& path, t_cb_visit_ty callback)
{
    TyVisitorCbConst v;
    v.callback = callback;
    return v.visit_trait_path(path);
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
    bool ignore_lifetimes;
    TyVisitorMonomorphNeeded(bool ignore_lifetimes): ignore_lifetimes(ignore_lifetimes) {}
    const HIR::TypeData& get_ty_data(const HIR::TypeRef& ty) const override {
        return ty.data();
    }
    bool is_generic_lft(const ::HIR::LifetimeRef& lft) const {
        return lft.is_param() && (lft.binding >> 8) != 3;
    }
    bool visit_path_params(const ::HIR::PathParams& pp) override
    {
        if( !this->ignore_lifetimes )
        {
            for(const auto& lft : pp.m_lifetimes) {
                if( is_generic_lft(lft) )
                    return true;
            }
        }
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
        if( !this->ignore_lifetimes )
        {
            if( ty.data().is_Borrow() && is_generic_lft(ty.data().as_Borrow().lifetime) )
                return true;
            if( ty.data().is_TraitObject() && is_generic_lft(ty.data().as_TraitObject().m_lifetime) )
                return true;
            if( ty.data().is_ErasedType() ) {
                for( const auto& l : ty.data().as_ErasedType().m_lifetimes ) {
                    if( is_generic_lft(l) )
                        return true;
                }
            }
        }
        return TyVisitor::visit_type(ty);
    }
};
bool monomorphise_pathparams_needed(const ::HIR::PathParams& tpl, bool ignore_lifetimes/*=false*/)
{
    TyVisitorMonomorphNeeded v { ignore_lifetimes };
    return v.visit_path_params(tpl);
}
bool monomorphise_traitpath_needed(const ::HIR::TraitPath& tpl, bool ignore_lifetimes/*=false*/)
{
    TyVisitorMonomorphNeeded v { ignore_lifetimes };
    return v.visit_trait_path(tpl);
}
bool monomorphise_path_needed(const ::HIR::Path& tpl, bool ignore_lifetimes/*=false*/)
{
    TyVisitorMonomorphNeeded v { ignore_lifetimes };
    return v.visit_path(tpl);
}
bool monomorphise_type_needed(const ::HIR::TypeRef& tpl, bool ignore_lifetimes/*=false*/)
{
    TyVisitorMonomorphNeeded v { ignore_lifetimes };
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
        if( e.m_trait.m_hrtbs ) {
            to.m_trait.m_hrtbs = box$(e.m_trait.m_hrtbs->clone());
            m_hrb_stack.push_back(e.m_trait.m_hrtbs.get());
        }
        to.m_trait = this->monomorph_traitpath(sp, e.m_trait, allow_infer, false);
        for(const auto& trait : e.m_markers)
        {
            to.m_markers.push_back( this->monomorph_genericpath(sp, trait, allow_infer, false) );
        }
        if( e.m_trait.m_hrtbs ) {
            m_hrb_stack.pop_back();
        }
        to.m_lifetime = monomorph_lifetime(sp, e.m_lifetime);
        return ::HIR::TypeRef( mv$(to) );
        }
    TU_ARMA(ErasedType, e) {
        ::std::vector< ::HIR::TraitPath>    traits;
        traits.reserve( e.m_traits.size() );
        for(const auto& trait : e.m_traits)
            traits.push_back( this->monomorph_traitpath(sp, trait, allow_infer, false) );
        ::std::vector< ::HIR::LifetimeRef>  lfts;
        for(const auto& lft : e.m_lifetimes)
            lfts.push_back(monomorph_lifetime(sp, lft));

        HIR::TypeData_ErasedType_Inner  inner;
        TU_MATCH_HDRA( (e.m_inner), {)
        TU_ARMA(Fcn, ee) {
            inner = ::HIR::TypeData_ErasedType_Inner::Data_Fcn {
                this->monomorph_path(sp, ee.m_origin, allow_infer),
                ee.m_index
                };
            }
        TU_ARMA(Alias, ee) {
            inner = ::HIR::TypeData_ErasedType_Inner::Data_Alias {
                this->monomorph_path_params(sp, ee.params, allow_infer),
                ee.inner
                };
            }
        TU_ARMA(Known, ee) {
            inner = this->monomorph_type(sp, ee, allow_infer);
            }
        }

        return ::HIR::TypeRef( ::HIR::TypeData::Data_ErasedType {
            e.m_is_sized,
            mv$(traits),
            mv$(lfts),
            mv$(inner)
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
        return ::HIR::TypeRef::new_borrow (e.type, this->monomorph_type(sp, e.inner, allow_infer), monomorph_lifetime(sp, e.lifetime));
        }
    TU_ARMA(Pointer, e) {
        return ::HIR::TypeRef::new_pointer(e.type, this->monomorph_type(sp, e.inner, allow_infer));
        }
    TU_ARMA(NamedFunction, e) {
        return ::HIR::TypeRef( ::HIR::TypeData::Data_NamedFunction {
            this->monomorph_path(sp, e.path, allow_infer),
            e.def.clone()   // Should this become `nullptr`? Or should the definition be fixed
            } );
        }
    TU_ARMA(Function, e) {
        m_hrb_stack.push_back(&e.hrls);
        ::HIR::TypeData_FunctionPointer ft;
        ft.hrls = e.hrls.clone();
        ft.is_unsafe = e.is_unsafe;
        ft.is_variadic = e.is_variadic;
        ft.m_abi = e.m_abi;
        ft.m_rettype = this->monomorph_type(sp, e.m_rettype, allow_infer);
        for( const auto& arg : e.m_arg_types )
            ft.m_arg_types.push_back( this->monomorph_type(sp, arg, allow_infer) );
        m_hrb_stack.pop_back();
        return ::HIR::TypeRef( mv$(ft) );
        }
    // Closures and generators are just passed through, needed for hackery in type checking (erasing HRLs)
    TU_ARMA(Closure, e) {
        ::HIR::TypeData::Data_Closure  oe;
        oe.node = e.node;
        return ::HIR::TypeRef( mv$(oe) );
        }
    TU_ARMA(Generator, e) {
        ::HIR::TypeData::Data_Generator oe;
        oe.node = e.node;
        return ::HIR::TypeRef( mv$(oe) );
        }
    }
    throw "";
}
::HIR::LifetimeRef Monomorphiser::monomorph_lifetime(const Span& sp, const ::HIR::LifetimeRef& lft) const
{
    if( lft.is_param() ) {
        HIR::GenericRef g {"", lft.binding};

        // TODO: Have a flag/stack here for current defined HRL batches (trait paths and function pointers), if in one then do the hack
        // - Otherwise, pass to `get_lifetime`
        if( !m_hrb_stack.empty() && g.group() == 3 )  {
            // TODO: Ensure that the param is in range.
            return lft;
        }

        return get_lifetime(sp, g);
    }
    else {
        return lft;
    }
}
::HIR::Path Monomorphiser::monomorph_path(const Span& sp, const ::HIR::Path& tpl, bool allow_infer/*=true*/) const
{
    TU_MATCH_HDRA( (tpl.m_data), {)
    TU_ARMA(Generic, e2) {
        return ::HIR::Path( this->monomorph_genericpath(sp, e2, allow_infer, false) );
        }
    TU_ARMA(UfcsKnown, e2) {
        if( e2.hrtbs /*&& !ignore_hrls*/ ) {
            m_hrb_stack.push_back(e2.hrtbs.get());
        }
        auto rv = ::HIR::Path(::HIR::Path::Data::make_UfcsKnown({
            this->monomorph_type(sp, e2.type, allow_infer),
            this->monomorph_genericpath(sp, e2.trait, allow_infer, false),
            e2.item,
            this->monomorph_path_params(sp, e2.params, allow_infer),
            e2.hrtbs ? box$(e2.hrtbs->clone()) : nullptr
            }));
        if( e2.hrtbs /*&& !ignore_hrls*/ ) {
            m_hrb_stack.pop_back();
        }
        return rv;
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
::HIR::TraitPath Monomorphiser::monomorph_traitpath(const Span& sp, const ::HIR::TraitPath& tpl, bool allow_infer, bool ignore_hrls) const
{
    if( tpl.m_hrtbs && !ignore_hrls ) {
        m_hrb_stack.push_back(tpl.m_hrtbs.get());
    }

    ::HIR::TraitPath    rv {
        tpl.m_hrtbs ? box$(tpl.m_hrtbs->clone()) : nullptr,
        this->monomorph_genericpath(sp, tpl.m_path, allow_infer, true),
        {},
        {},
        tpl.m_trait_ptr
        };

    for(const auto& assoc : tpl.m_type_bounds) {
        rv.m_type_bounds.insert(::std::make_pair(
            assoc.first,
            HIR::TraitPath::AtyEqual {
                this->monomorph_genericpath(sp, assoc.second.source_trait, allow_infer, false),
                this->monomorph_type(sp, assoc.second.type, allow_infer)
                }
            ));
    }
    for(const auto& assoc : tpl.m_trait_bounds) {
        auto v = HIR::TraitPath::AtyBound { this->monomorph_genericpath(sp, assoc.second.source_trait, allow_infer, false), {} };
        for(const auto& trait : assoc.second.traits)
            v.traits.push_back( monomorph_traitpath(sp, trait, allow_infer, false) );
        rv.m_trait_bounds.insert(::std::make_pair( assoc.first, std::move(v) ));
    }

    if( tpl.m_hrtbs && !ignore_hrls ) {
        m_hrb_stack.pop_back();
    }

    return rv;
}
::HIR::ConstGeneric Monomorphiser::monomorph_constgeneric(const Span& sp, const ::HIR::ConstGeneric& val, bool allow_infer) const
{
    if(const auto* ge = val.opt_Generic())
    {
        return this->get_value(sp, *ge);
    }
    else if( const auto* ge = val.opt_Unevaluated() )
    {
        auto rv = HIR::ConstGeneric(std::make_unique<HIR::ConstGeneric_Unevaluated>( (*ge)->monomorph(sp, *this, true) ));
        // TODO: Evaluate this constant (if possible), but that requires knowing the target type :/
        return rv;
    }
    else
    {
        return val.clone();
    }
}
::HIR::PathParams Monomorphiser::monomorph_path_params(const Span& sp, const ::HIR::PathParams& tpl, bool allow_infer) const
{
    ::HIR::PathParams   rv;

    rv.m_lifetimes.reserve( tpl.m_lifetimes.size() );
    for( const auto& lft : tpl.m_lifetimes) {
        rv.m_lifetimes.push_back( this->monomorph_lifetime(sp, lft) );
    }

    rv.m_types.reserve( tpl.m_types.size() );
    for( const auto& ty : tpl.m_types) {
        rv.m_types.push_back( this->monomorph_type(sp, ty, allow_infer) );
    }

    rv.m_values.reserve( tpl.m_values.size() );
    for( const auto& val : tpl.m_values) {
        rv.m_values.push_back(monomorph_constgeneric(sp, val, allow_infer));
    }

    return rv;
}
::HIR::GenericPath Monomorphiser::monomorph_genericpath(const Span& sp, const ::HIR::GenericPath& tpl, bool allow_infer, bool ignore_hrls) const
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
        }
        else if( se->is_Unevaluated() ) {
            sz = HIR::ConstGeneric(std::make_unique<HIR::ConstGeneric_Unevaluated>( se->as_Unevaluated()->monomorph(sp, *this, true) ));
        }
        else {
            sz = se->clone();
        }
        se = sz.opt_Unevaluated();
        assert(se);

        // Evaluate, if possible
        if(se->is_Unevaluated()) {
            if( this->consteval_crate ) {
                ConvertHIR_ConstantEvaluate_ConstGeneric(sp, *this->consteval_crate, HIR::CoreType::Usize, sz.as_Unevaluated());
            }
            else {
                DEBUG("TODO: Evaluate unevaluated generic for array size - " << *se);
            }
        }

        if( const auto* e = se->opt_Evaluated() ) {
            return (*e)->read_usize(0);
        }
        return sz;
    }
    else {
        return tpl.clone();
    }
}


struct CloneTyWith_Monomorph: Monomorphiser {
    t_cb_clone_ty callback;

    ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& g) const override {
        return HIR::TypeRef(g);
    }
    ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& g) const override {
        return g;
    }
    ::HIR::LifetimeRef get_lifetime(const Span& sp, const ::HIR::GenericRef& g) const override {
        return HIR::LifetimeRef(g.binding);
    }

    ::HIR::TypeRef monomorph_type(const Span& sp, const ::HIR::TypeRef& ty, bool allow_infer=true) const {
        ::HIR::TypeRef  rv;

        if( callback(ty, rv) ) {
            //DEBUG(tpl << " => " << rv);
            return rv;
        }
        return Monomorphiser::monomorph_type(sp, ty, allow_infer);
    }
};
::HIR::PathParams clone_path_params_with(const Span& sp, const ::HIR::PathParams& tpl, t_cb_clone_ty callback)
{
    ::HIR::PathParams   rv;
    for( const auto& v : tpl.m_lifetimes )
        rv.m_lifetimes.push_back( v );
    rv.m_types.reserve( tpl.m_types.size() );
    for( const auto& ty : tpl.m_types)
        rv.m_types.push_back( clone_ty_with(sp, ty, callback) );
    for( const auto& v : tpl.m_values)
        rv.m_values.push_back( v.clone() );
    return rv;
}
::HIR::TypeRef clone_ty_with(const Span& sp, const ::HIR::TypeRef& tpl, t_cb_clone_ty callback)
{
    CloneTyWith_Monomorph   m;
    m.callback = std::move(callback);
    return m.monomorph_type(sp, tpl, true);
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
            if( const auto* p = this->get_impl_params() ) {
                ASSERT_BUG(sp, ty.idx() < p->m_types.size(), "Type param " << ty << " out of range for (max " << p->m_types.size() << ")");
                return p->m_types[ty.idx()].clone();
            }
            else {
                BUG(sp, "Impl parameters were not expected (got " << ty << ")");
            }
            break;
        case 1:
            if( const auto* p = this->get_method_params() ) {
                ASSERT_BUG(sp, ty.idx() < p->m_types.size(), "Type param " << ty << " out of range for (max " << p->m_types.size() << ")");
                return p->m_types[ty.idx()].clone();
            }
            else {
                BUG(sp, "Method parameters were not expected (got " << ty << ")");
            }
            break;
#if 0
        case 3:
            if( const auto* p = this->get_hrl_params() ) {
                ASSERT_BUG(sp, ty.idx() < p->m_types.size(), "Type param " << ty << " out of range for (max " << p->m_types.size() << ")");
                return p->m_types[ty.idx()].clone();
            }
            else {
                BUG(sp, "Higher-ranked parameters were not expected (got " << ty << ")");
            }
            break;
#endif
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
#if 0
    case 3:
        if( const auto* p = this->get_hrl_params() ) {
            ASSERT_BUG(sp, val.idx() < p->m_values.size(), "Value param " << val << " out of range for (max " << p->m_values.size() << ")");
            return p->m_values[val.idx()].clone();
        }
        else {
            BUG(sp, "Higher-ranked parameters were not expected (got " << val << ")");
        }
        break;
#endif
    default:
        BUG(sp, "Unexpected value param " << val);
    }
}
::HIR::LifetimeRef MonomorphiserPP::get_lifetime(const Span& sp, const ::HIR::GenericRef& lft_ref) const /*override*/
{
    // HACK: If no params are present at all, just return unchanged
    // - Note: Equality on PathParams ignores lifetimes, hence the second check
    if( (!this->get_impl_params  () || (*this->get_impl_params  () == HIR::PathParams() && this->get_impl_params  ()->m_lifetimes.empty()))
     && (!this->get_method_params() || (*this->get_method_params() == HIR::PathParams() && this->get_method_params()->m_lifetimes.empty()))
     && (!this->get_hrb_params   () || (*this->get_hrb_params   () == HIR::PathParams() && this->get_hrb_params   ()->m_lifetimes.empty()))
      )
    {
        DEBUG("Passthrough " << lft_ref);
        return HIR::LifetimeRef(lft_ref.binding);
    }

    switch(lft_ref.group())
    {
    case 0:
        if( const auto* p = this->get_impl_params() ) {
            ASSERT_BUG(sp, lft_ref.idx() < p->m_lifetimes.size(), "Lifetime param " << lft_ref << " out of range for (max " << p->m_lifetimes.size() << ")");
            return p->m_lifetimes[lft_ref.idx()];
        }
        else {
            BUG(sp, "Impl lifetime parameters were not expected (got " << lft_ref << ")");
        }
        break;
    case 1:
        if( const auto* p = this->get_method_params() ) {
            ASSERT_BUG(sp, lft_ref.idx() < p->m_lifetimes.size(), "Lifetime param " << lft_ref << " out of range for (max " << p->m_lifetimes.size() << ")");
            return p->m_lifetimes[lft_ref.idx()];
        }
        else {
            BUG(sp, "Method lifetime parameters were not expected (got " << lft_ref << ")");
        }
        break;
    case 2: // Placeholders, just pass through
        DEBUG("Placeholder " << lft_ref);
        return HIR::LifetimeRef(lft_ref.binding);
    case 3: // HRLs
        if( const auto* p = this->get_hrb_params() ) {
            ASSERT_BUG(sp, lft_ref.idx() < p->m_lifetimes.size(), "Lifetime param " << lft_ref << " out of range for (max " << p->m_lifetimes.size() << ")");
            return p->m_lifetimes[lft_ref.idx()];
        }
        else {
            BUG(sp, "Higher-ranked lifetime parameters were not expected (got " << lft_ref << ")");
            //DEBUG("No HRBs " << lft_ref);
            //return HIR::LifetimeRef(lft_ref.binding);
        }
        break;
    default:
        BUG(sp, "Unexpected lifetime param " << lft_ref);
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
    //if(ms.pp_hrb)
    //    os << " H=" << *ms.pp_hrb;
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
