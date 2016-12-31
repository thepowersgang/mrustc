/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/trans_list.cpp
 * - A list of items that require translation
 */
#include "trans_list.hpp"
#include <hir_typeck/static.hpp>    // StaticTraitResolve

TransList_Function* TransList::add_function(::HIR::Path p)
{
    auto rv = m_functions.insert( ::std::make_pair(mv$(p), nullptr) );
    if( rv.second )
    {
        DEBUG("Function " << rv.first->first);
        assert( !rv.first->second );
        rv.first->second.reset( new TransList_Function {} );
        return &*rv.first->second;
    }
    else
    {
        return nullptr;
    }
}
TransList_Static* TransList::add_static(::HIR::Path p)
{
    auto rv = m_statics.insert( ::std::make_pair(mv$(p), nullptr) );
    if( rv.second )
    {
        DEBUG("Static " << rv.first->first);
        assert( !rv.first->second );
        rv.first->second.reset( new TransList_Static {} );
        return &*rv.first->second;
    }
    else
    {
        return nullptr;
    }
}

t_cb_generic Trans_Params::get_cb() const
{
    return monomorphise_type_get_cb(sp, &self_type, &pp_impl, &pp_method);
}
::HIR::Path Trans_Params::monomorph(const ::StaticTraitResolve& resolve, const ::HIR::Path& p) const
{
    TRACE_FUNCTION_F(p);
    auto rv = monomorphise_path_with(sp, p, this->get_cb(), false);

    TU_MATCH(::HIR::Path::Data, (rv.m_data), (e2),
    (Generic,
        for(auto& arg : e2.m_params.m_types)
            resolve.expand_associated_types(sp, arg);
        ),
    (UfcsInherent,
        resolve.expand_associated_types(sp, *e2.type);
        for(auto& arg : e2.params.m_types)
            resolve.expand_associated_types(sp, arg);
        // TODO: impl params too?
        for(auto& arg : e2.impl_params.m_types)
            resolve.expand_associated_types(sp, arg);
        ),
    (UfcsKnown,
        resolve.expand_associated_types(sp, *e2.type);
        for(auto& arg : e2.trait.m_params.m_types)
            resolve.expand_associated_types(sp, arg);
        for(auto& arg : e2.params.m_types)
            resolve.expand_associated_types(sp, arg);
        ),
    (UfcsUnknown,
        BUG(sp, "Encountered UfcsUnknown");
        )
    )
    return rv;
}

::HIR::GenericPath Trans_Params::monomorph(const ::StaticTraitResolve& resolve, const ::HIR::GenericPath& p) const
{
    return ::HIR::GenericPath( p.m_path,  this->monomorph(resolve, p.m_params) );
}

::HIR::PathParams Trans_Params::monomorph(const ::StaticTraitResolve& resolve, const ::HIR::PathParams& p) const
{
    auto rv = monomorphise_path_params_with(sp, p, this->get_cb(), false);
    for(auto& arg : rv.m_types)
        resolve.expand_associated_types(sp, arg);
    return rv;
}

::HIR::TypeRef Trans_Params::monomorph(const ::StaticTraitResolve& resolve, const ::HIR::TypeRef& ty) const
{
    auto rv = monomorphise_type_with(sp, ty, this->get_cb(), false);
    resolve.expand_associated_types(sp, rv);
    return rv;
}
