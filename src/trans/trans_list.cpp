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
        rv.first->second.reset( new TransList_Function(rv.first->first) );
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
TransList_Const* TransList::add_const(::HIR::Path p)
{
    auto rv = m_constants.insert( ::std::make_pair(mv$(p), nullptr) );
    if( rv.second )
    {
        DEBUG("Const " << rv.first->first);
        assert( !rv.first->second );
        rv.first->second.reset( new TransList_Const {} );
        return &*rv.first->second;
    }
    else
    {
        return nullptr;
    }
}

::HIR::Path Trans_Params::monomorph(const ::StaticTraitResolve& resolve, const ::HIR::Path& p) const
{
    TRACE_FUNCTION_F(p);
    auto rv = this->monomorph_path(sp, p, false);

    TU_MATCH_HDRA( (rv.m_data), {)
    TU_ARMA(Generic, e2) {
        for(auto& arg : e2.m_params.m_types)
            resolve.expand_associated_types(sp, arg);
        }
    TU_ARMA(UfcsInherent, e2) {
        resolve.expand_associated_types(sp, e2.type);
        for(auto& arg : e2.params.m_types)
            resolve.expand_associated_types(sp, arg);
        // TODO: impl params too?
        for(auto& arg : e2.impl_params.m_types)
            resolve.expand_associated_types(sp, arg);
        }
    TU_ARMA(UfcsKnown, e2) {
        resolve.expand_associated_types(sp, e2.type);
        for(auto& arg : e2.trait.m_params.m_types)
            resolve.expand_associated_types(sp, arg);
        for(auto& arg : e2.params.m_types)
            resolve.expand_associated_types(sp, arg);
        }
    TU_ARMA(UfcsUnknown, e2) {
        BUG(sp, "Encountered UfcsUnknown");
        }
    }
    return rv;
}

::HIR::GenericPath Trans_Params::monomorph(const ::StaticTraitResolve& resolve, const ::HIR::GenericPath& p) const
{
    return ::HIR::GenericPath( p.m_path,  this->monomorph(resolve, p.m_params) );
}

::HIR::PathParams Trans_Params::monomorph(const ::StaticTraitResolve& resolve, const ::HIR::PathParams& p) const
{
    auto rv = this->monomorph_path_params(sp, p, false);
    for(auto& arg : rv.m_types)
        resolve.expand_associated_types(sp, arg);
    return rv;
}

::HIR::TypeRef Trans_Params::monomorph(const ::StaticTraitResolve& resolve, const ::HIR::TypeRef& ty) const
{
    return resolve.monomorph_expand(sp, ty, *this);
}
