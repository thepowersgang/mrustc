
#include <hir/hir.hpp>

class StaticTraitResolve
{
    const ::HIR::Crate&   m_crate;
public:
    StaticTraitResolve(const ::HIR::Crate& crate):
        m_crate(crate)
    {}
    
    typedef ::std::function<bool(const ::std::vector<const ::HIR::TypeRef*>& , const ::HIR::TraitImpl&)> t_cb_find_impl;
    
    bool find_impl(
        const Span& sp,
        const ::HIR::Crate& crate, const ::HIR::SimplePath& trait_path, const ::HIR::PathParams& trait_params,
        const ::HIR::TypeRef& type,
        t_cb_find_impl found_cb
        ) const;

    void expand_associated_types(const Span& sp, ::HIR::TypeRef& input) const;
};

