/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/expand_type.cpp
 * - Expand `type` aliases in HIR
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>
#include <hir_typeck/common.hpp>    // monomorphise_type_with

namespace {
    HIR::PathParams get_path_params(const Span& sp, const ::HIR::GenericParams& params_def, const ::HIR::GenericPath& path, bool is_expr)
    {
        auto pp = path.m_params.clone();

        if( is_expr && pp.m_types.empty() )
        {
            // Empty list, fill with ivars
            while( pp.m_types.size() < params_def.m_types.size() )
            {
                pp.m_types.push_back( ::HIR::TypeRef() );
            }
        }

        auto ms_o = MonomorphStatePtr(nullptr, &path.m_params, nullptr);
        while( pp.m_types.size() < params_def.m_types.size() && params_def.m_types[pp.m_types.size()].m_default != ::HIR::TypeRef() ) {
            pp.m_types.push_back( ms_o.monomorph_type(sp, params_def.m_types[pp.m_types.size()].m_default) );
        }
        if( pp.m_types.size() != params_def.m_types.size() ) {
            ERROR(sp, E0000, "Mismatched parameter count in " << path << ", expected " << params_def.m_types.size() << " got " << pp.m_types.size());
        }
        return pp;
    }
}
::HIR::TypeRef ConvertHIR_ExpandAliases_GetExpansion_GP(const Span& sp, const ::HIR::Crate& crate, const ::HIR::GenericPath& path, bool is_expr)
{
    const auto& ti = crate.get_typeitem_by_path(sp, path.m_path);
    if(const auto* ep = ti.opt_TypeAlias() )
    {
        const auto& ta = *ep;
        auto pp = get_path_params(sp, ta.m_params, path, is_expr);
        // Monomorphise the exapnded type using the created params
        auto ms = MonomorphStatePtr(nullptr, &pp, nullptr);
        HIR::TypeRef rv = ms.monomorph_type(sp, ta.m_type);
        DEBUG(path << " -> " << path.m_path << pp << " -> " << rv);
        return rv;
    }
    return ::HIR::TypeRef();
}

::HIR::TypeRef ConvertHIR_ExpandAliases_GetExpansion(const ::HIR::Crate& crate, const ::HIR::Path& path, bool is_expr)
{
    static Span sp;
    TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
    (Generic,
        return ConvertHIR_ExpandAliases_GetExpansion_GP(sp, crate, e, is_expr);
        ),
    (UfcsInherent,
        DEBUG("TODO: Locate impl blocks for types - path=" << path);
        ),
    (UfcsKnown,
        DEBUG("TODO: Locate impl blocks for traits on types - path=" << path);
        ),
    (UfcsUnknown,
        DEBUG("TODO: Locate impl blocks for traits on types - path=" << path);
        )
    )
    return ::HIR::TypeRef();
}

std::vector<HIR::TraitPath> ConvertHIR_ExpandAliases_GetTraitExpansion_GP(const Span& sp, const ::HIR::Crate& crate, const HIR::GenericPath& path, bool is_expr)
{
    const auto& ti = crate.get_typeitem_by_path(sp, path.m_path);
    if(const auto* ep = ti.opt_TraitAlias() )
    {
        const auto& ta = *ep;
        auto pp = get_path_params(sp, ta.m_params, path, is_expr);
        auto ms = MonomorphStatePtr(nullptr, &pp, nullptr);
        std::vector<HIR::TraitPath> rv;
        rv.reserve(ta.m_traits.size());
        for(const auto& exp : ta.m_traits)
        {
            rv.push_back(ms.monomorph_traitpath(sp, exp, false));
        }
        DEBUG(path << " -> " << path.m_path << pp << " -> {" << rv << "}");
        return rv;
    }
    else
    {
        return std::vector<HIR::TraitPath>();
    }
}
std::vector<HIR::TraitPath> ConvertHIR_ExpandAliases_GetTraitExpansion(const Span& sp, const ::HIR::Crate& crate, const HIR::TraitPath& path, bool is_expr)
{
    auto rv = ConvertHIR_ExpandAliases_GetTraitExpansion_GP(sp, crate, path.m_path, is_expr);
    if( !rv.empty() )
    {
        if( !path.m_trait_bounds.empty() || !path.m_type_bounds.empty() )
        {
            TODO(sp, "Re-assign ATYs - " << path);
        }
    }
    return rv;
}

class Expander:
    public ::HIR::Visitor
{
    const ::HIR::Crate& m_crate;
    bool m_in_expr = false;
    const ::HIR::TypeRef*   m_impl_type = nullptr;

public:
    Expander(const ::HIR::Crate& crate):
        m_crate(crate)
    {}

    void visit_type(::HIR::TypeRef& ty) override
    {
        static Span sp;

        ::HIR::Visitor::visit_type(ty);

        if( const auto* e = ty.data().opt_Path() ) 
        {
            ::HIR::TypeRef  new_type = ConvertHIR_ExpandAliases_GetExpansion(m_crate, e->path, m_in_expr);
            // Keep trying to expand down the chain
            unsigned int num_exp = 1;
            const unsigned int MAX_RECURSIVE_TYPE_EXPANSIONS = 100;
            while(num_exp < MAX_RECURSIVE_TYPE_EXPANSIONS)
            {
                // NOTE: inner recurses
                ::HIR::Visitor::visit_type(new_type);
                if( const auto* e = new_type.data().opt_Path() )
                {
                    auto nt = ConvertHIR_ExpandAliases_GetExpansion(m_crate, e->path, m_in_expr);
                    if( nt == ::HIR::TypeRef() )
                        break;
                    num_exp ++;
                    new_type = mv$(nt);
                }
                else {
                    break;
                }
            }
            ASSERT_BUG(sp, num_exp < MAX_RECURSIVE_TYPE_EXPANSIONS, "Recursion limit hit expanding " << ty << " (currently on " << new_type << ")");
            if( ! new_type.data().is_Infer() ) {
                DEBUG("Replacing " << ty << " with " << new_type << " (" << num_exp << " expansions)");
                ty = mv$(new_type);
            }
        }
    }

    void visit_trait_path(::HIR::TraitPath& tp) override
    {
        static Span sp;
        // 1. Make sure that the trait path isn't pointing at an alias (should have been handled by the caller, which can expand to multiple items)
        ASSERT_BUG(sp, m_crate.get_typeitem_by_path(sp, tp.m_path.m_path).is_Trait(), "Bad trait path - " << tp.m_path);
        // 2. Handle AtyBounds
        for(auto& tb : tp.m_trait_bounds)
        {
            for(auto it = tb.second.traits.begin(); it != tb.second.traits.end(); ++it)
            {
                auto n = ConvertHIR_ExpandAliases_GetTraitExpansion(sp, m_crate, *it, m_in_expr);
                if(!n.empty())
                {
                    it = tb.second.traits.erase(it);
                    it = tb.second.traits.insert(it, std::make_move_iterator(n.begin()), std::make_move_iterator(n.end()));
                    --it;
                }
            }
        }

        // Finally. Recurse
        ::HIR::Visitor::visit_trait_path(tp);
    }

    ::HIR::Path expand_alias_path(const Span& sp, const ::HIR::Path& path)
    {
        const unsigned int MAX_RECURSIVE_TYPE_EXPANSIONS = 100;

        // If the path is already generic and points at an enum variant, skip
        if( path.m_data.is_Generic() )
        {
            const auto& gp = path.m_data.as_Generic();
            if( gp.m_path.m_components.size() > 1 && m_crate.get_typeitem_by_path(sp, gp.m_path, /*igncrate*/false, /*ignlast*/true).is_Enum() )
            {
                return ::HIR::GenericPath();
            }
        }

        ::HIR::Path  rv = ::HIR::GenericPath();
        const auto* cur = &path;

        unsigned int num_exp = 0;
        do {
            auto ty = ConvertHIR_ExpandAliases_GetExpansion(m_crate, *cur, m_in_expr);
            if( ty == ::HIR::TypeRef() )
                break ;
            if( !ty.data().is_Path() )
                ERROR(sp, E0000, "Type alias referenced in generic path doesn't point to a path");
            rv = std::move(ty.get_unique().as_Path().path);

            this->visit_path(rv, ::HIR::Visitor::PathContext::TYPE);

            cur = &rv;
        } while( ++num_exp < MAX_RECURSIVE_TYPE_EXPANSIONS );
        ASSERT_BUG(sp, num_exp < MAX_RECURSIVE_TYPE_EXPANSIONS, "Recursion limit expanding " << path << " (currently on " << *cur << ")");
        return mv$( rv );
    }

    ::HIR::Pattern::PathBinding visit_pattern_PathBinding(const Span& sp, ::HIR::Path& path)
    {
        if( path.m_data.is_UfcsUnknown() ) {
            const auto& ty = path.m_data.as_UfcsUnknown().type;
            const auto& name = path.m_data.as_UfcsUnknown().item;

            if( !ty.data().is_Path() ) {
                ERROR(sp, E0000, "Expeted path in pattern binding, got " << ty);
            }
            if( !ty.data().as_Path().path.m_data.is_Generic() ) {
                ERROR(sp, E0000, "Expeted generic path in pattern binding, got " << ty);
            }
            const auto& gp = ty.data().as_Path().path.m_data.as_Generic();
            const auto& ti = m_crate.get_typeitem_by_path(sp, gp.m_path);
            if( !ti.is_Enum() ) {
                ERROR(sp, E0000, "Expeted enum path in pattern binding, got " << ti.tag_str());
            }
            const auto& enm = ti.as_Enum();

            auto gp2 = gp.clone();
            gp2.m_path.m_components.push_back(name);
            gp2.m_params.m_types.resize( enm.m_params.m_types.size() );

            auto idx = enm.find_variant(name);
            if(idx == ~0u) {
                TODO(sp, "Variant " << name << " not found in " << gp);
            }
            path = std::move(gp2);
            return ::HIR::Pattern::PathBinding::make_Enum({ &enm, static_cast<unsigned>(idx) });
        }
        // `Self { ... }` patterns - Encoded as `<Self>::`
        if( path.m_data.is_UfcsInherent() ) {
            const auto& ty = path.m_data.as_UfcsInherent().type;
            const auto& name = path.m_data.as_UfcsInherent().item;
            ASSERT_BUG(sp, ty.data().is_Generic() && ty.data().as_Generic().binding == GENERIC_Self, path);
            ASSERT_BUG(sp, name == "", path);
            if(!m_impl_type) {
                ERROR(sp, E0000, "Use of `Self` pattern outside of an impl block");
            }
            if(!TU_TEST1(m_impl_type->data(), Path, .path.m_data.is_Generic()) ) {
                ERROR(sp, E0000, "Use of `Self` pattern in non-struct impl block - " << *m_impl_type);
            }
            const auto& p = m_impl_type->data().as_Path().path.m_data.as_Generic();
            const auto& str = m_crate.get_struct_by_path(sp, p.m_path);

            path = p.clone();
            return ::HIR::Pattern::PathBinding::make_Struct(&str);
        }

        // TODO: `Self { ... }` encoded as `<Self>::`
        ASSERT_BUG(sp, path.m_data.is_Generic(), path);
        auto& gp = path.m_data.as_Generic();

        // TODO: Better error messages?
        if( gp.m_path.m_components.size() > 1 )
        {
            const auto& ti = m_crate.get_typeitem_by_path(sp, gp.m_path, false, /*ignore_last*/true);
            if( ti.is_Enum() ) {
                // Enum variant!
                const auto& enm = ti.as_Enum();

                gp.m_params.m_types.resize( enm.m_params.m_types.size() );

                auto idx = ti.as_Enum().find_variant(gp.m_path.m_components.back());
                return ::HIR::Pattern::PathBinding::make_Enum({ &enm, static_cast<unsigned>(idx) });
            }
        }

        // Has to be a struct
        const auto& str = m_crate.get_struct_by_path(sp, gp.m_path);

        gp.m_params.m_types.resize( str.m_params.m_types.size() );

        return ::HIR::Pattern::PathBinding::make_Struct(&str);
    }

    void visit_pattern(::HIR::Pattern& pat) override
    {
        static Span sp;

        ::HIR::Visitor::visit_pattern(pat);

        TU_MATCH_HDRA( (pat.m_data), {)
        default:
            break;
        TU_ARMA(PathValue, e) {
            auto new_path = expand_alias_path(sp, e.path);
            if( new_path != ::HIR::GenericPath() )
            {
                DEBUG("Replacing " << e.path << " with " << new_path);
                e.path = mv$(new_path);
            }
            e.binding = visit_pattern_PathBinding(sp, e.path);
            }
        TU_ARMA(PathTuple, e) {
            auto new_path = expand_alias_path(sp, e.path);
            if( new_path != ::HIR::GenericPath() )
            {
                DEBUG("Replacing " << e.path << " with " << new_path);
                e.path = mv$(new_path);
            }
            e.binding = visit_pattern_PathBinding(sp, e.path);
            }
        TU_ARMA(PathNamed, e) {
            auto new_path = expand_alias_path(sp, e.path);
            if( new_path != ::HIR::GenericPath() )
            {
                DEBUG("Replacing " << e.path << " with " << new_path);
                e.path = mv$(new_path);
            }
            e.binding = visit_pattern_PathBinding(sp, e.path);
            // TODO: If this is an empty/wildcard AND it's poiting at a value/tuple entry, change to PathValue/PathTuple
            }
        }
    }

    void visit_params(::HIR::GenericParams& params) override
    {
        for(auto it = params.m_bounds.begin(); it != params.m_bounds.end(); ++it)
        {
            static Span sp;
            if( it->is_TraitBound() )
            {
                auto n = ConvertHIR_ExpandAliases_GetTraitExpansion(sp, m_crate, it->as_TraitBound().trait, m_in_expr);
                if(!n.empty())
                {
                    auto type = std::move(it->as_TraitBound().type);
                    visit_type(type);

                    it = params.m_bounds.erase(it);
                    for(auto& t : n)
                    {
                        it = params.m_bounds.insert(it, HIR::GenericBound::make_TraitBound({ type.clone(), std::move(t) }));
                    }
                }
            }
        }
        ::HIR::Visitor::visit_params(params);
    }

    void visit_expr(::HIR::ExprPtr& expr) override
    {
        struct Visitor:
            public ::HIR::ExprVisitorDef
        {
            Expander& upper_visitor;

            Visitor(Expander& uv):
                upper_visitor(uv)
            {}

            void visit_type(::HIR::TypeRef& ty) override
            {
                upper_visitor.visit_type(ty);
            }
            void visit_pattern(const Span& sp, ::HIR::Pattern& pat) override
            {
                upper_visitor.visit_pattern(pat);
            }

            // Custom impl to visit the inner expression
            void visit(::HIR::ExprNode_ArraySized& node) override
            {
                auto& as = node.m_size;
                if( as.is_Unevaluated() && as.as_Unevaluated().is_Unevaluated() )
                {
                    upper_visitor.visit_expr(*as.as_Unevaluated().as_Unevaluated());
                }
                ::HIR::ExprVisitorDef::visit(node);
            }
        };

        if( expr.get() != nullptr )
        {
            auto old = m_in_expr;
            m_in_expr = true;

            Visitor v { *this };
            (*expr).visit(v);

            m_in_expr = old;
        }
    }


    void visit_type_impl(::HIR::TypeImpl& impl) override
    {
        m_impl_type = &impl.m_type;
        ::HIR::Visitor::visit_type_impl(impl);
        m_impl_type = nullptr;
    }
    void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
    {
        static Span sp;
        m_impl_type = &impl.m_type;
        ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        m_impl_type = nullptr;
    }
};


class Expander_Self:
    public ::HIR::Visitor
{
    const ::HIR::Crate& m_crate;
    const ::HIR::TypeRef*   m_impl_type = nullptr;
    bool m_in_expr = false;

public:
    Expander_Self(const ::HIR::Crate& crate):
        m_crate(crate)
    {}

    void visit_type(::HIR::TypeRef& ty) override
    {
        ::HIR::Visitor::visit_type(ty);

        if(const auto* te = ty.data().opt_Generic() )
        {
            if( te->binding == GENERIC_Self )
            {
                if( m_impl_type )
                {
                    DEBUG("Replace Self with " << *m_impl_type);
                    ty = m_impl_type->clone();
                }
                else
                {
                    // NOTE: Valid for `trait` definitions.
                    DEBUG("Self outside of an `impl` block");
                }
            }
        }
    }


    void visit_expr(::HIR::ExprPtr& expr) override
    {
        struct Visitor:
            public ::HIR::ExprVisitorDef
        {
            Expander_Self& upper_visitor;

            Visitor(Expander_Self& uv):
                upper_visitor(uv)
            {}

            void visit_type(::HIR::TypeRef& ty) override
            {
                upper_visitor.visit_type(ty);
            }
            void visit_pattern(const Span& sp, ::HIR::Pattern& pat) override
            {
                upper_visitor.visit_pattern(pat);
            }

            // Custom impl to visit the inner expression
            void visit(::HIR::ExprNode_ArraySized& node) override
            {
                auto& as = node.m_size;
                if( as.is_Unevaluated() && as.as_Unevaluated().is_Unevaluated() )
                {
                    upper_visitor.visit_expr(*as.as_Unevaluated().as_Unevaluated());
                }
                ::HIR::ExprVisitorDef::visit(node);
            }
        };

        if( expr.get() != nullptr )
        {
            auto old = m_in_expr;
            m_in_expr = true;

            Visitor v { *this };
            (*expr).visit(v);

            m_in_expr = old;
        }
    }

    void visit_type_impl(::HIR::TypeImpl& impl) override
    {
        m_impl_type = &impl.m_type;
        ::HIR::Visitor::visit_type_impl(impl);
        m_impl_type = nullptr;
    }
    void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
    {
        static Span sp;
        m_impl_type = &impl.m_type;
        ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        m_impl_type = nullptr;
    }
};

void ConvertHIR_ExpandAliases(::HIR::Crate& crate)
{
    Expander    exp { crate };
    exp.visit_crate( crate );
}

void ConvertHIR_ExpandAliases_Self(::HIR::Crate& crate)
{
    Expander_Self    exp { crate };
    exp.visit_crate( crate );
}

