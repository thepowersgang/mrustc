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

::HIR::TypeRef ConvertHIR_ExpandAliases_GetExpansion_GP(const Span& sp, const ::HIR::Crate& crate, const ::HIR::GenericPath& path, bool is_expr)
{
    ::HIR::TypeRef  empty_type;

    const auto& ti = crate.get_typeitem_by_path(sp, path.m_path);
    TU_MATCH_DEF( ::HIR::TypeItem, (ti), (e2),
    (
        // Anything else - leave it be
        ),
    (TypeAlias,
        if( !is_expr && path.m_params.m_types.size() != e2.m_params.m_types.size() ) {
            ERROR(sp, E0000, "Mismatched parameter count in " << path << ", expected " << e2.m_params.m_types.size() << " got " << path.m_params.m_types.size());
        }
        if( e2.m_params.m_types.size() > 0 ) {
            // TODO: Better `monomorphise_type`
            return monomorphise_type_with(sp, e2.m_type, [&](const auto& gt)->const ::HIR::TypeRef& {
                const auto& ge = gt.m_data.as_Generic();
                if( ge.binding == GENERIC_Self ) {
                    BUG(sp, "Self encountered in expansion for " << path << " - " << e2.m_type);
                }
                else if( (ge.binding >> 8) == 0 ) {
                    auto idx = ge.binding & 0xFF;
                    if( idx < path.m_params.m_types.size() )
                        return path.m_params.m_types[idx];
                    else if( is_expr )
                        return empty_type;
                    else
                        BUG(sp, "Referenced parameter missing from input");
                }
                else {
                    BUG(sp, "Bad index " << ge.binding << " encountered in expansion for " << path << " - " << e2.m_type);
                }
                });
        }
        else {
            return e2.m_type.clone();
        }
        )
    )
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

class Expander:
    public ::HIR::Visitor
{
    const ::HIR::Crate& m_crate;
    const ::HIR::TypeRef*   m_impl_type = nullptr;
    bool m_in_expr = false;

public:
    Expander(const ::HIR::Crate& crate):
        m_crate(crate)
    {}

    void visit_type(::HIR::TypeRef& ty) override
    {
        ::HIR::Visitor::visit_type(ty);

        if(const auto* te = ty.m_data.opt_Generic() )
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

        TU_IFLET(::HIR::TypeRef::Data, (ty.m_data), Path, (e),
            ::HIR::TypeRef  new_type = ConvertHIR_ExpandAliases_GetExpansion(m_crate, e.path, m_in_expr);
            // Keep trying to expand down the chain
            unsigned int num_exp = 1;
            const unsigned int MAX_RECURSIVE_TYPE_EXPANSIONS = 100;
            while(num_exp < MAX_RECURSIVE_TYPE_EXPANSIONS)
            {
                ::HIR::Visitor::visit_type(new_type);
                TU_IFLET(::HIR::TypeRef::Data, (new_type.m_data), Path, (e),
                    auto nt = ConvertHIR_ExpandAliases_GetExpansion(m_crate, e.path, m_in_expr);
                    if( nt == ::HIR::TypeRef() )
                        break;
                    num_exp ++;
                    new_type = mv$(nt);
                )
                else {
                    break;
                }
            }
            ASSERT_BUG(Span(), num_exp < MAX_RECURSIVE_TYPE_EXPANSIONS, "Recursion limit hit expanding " << ty << " (currently on " << new_type << ")");
            if( ! new_type.m_data.is_Infer() ) {
                DEBUG("Replacing " << ty << " with " << new_type << " (" << num_exp << " expansions)");
                ty = mv$(new_type);
            }
        )
    }


    ::HIR::GenericPath expand_alias_gp(const Span& sp, const ::HIR::GenericPath& path)
    {
        const unsigned int MAX_RECURSIVE_TYPE_EXPANSIONS = 100;

        ::HIR::GenericPath  rv;
        const auto* cur = &path;

        unsigned int num_exp = 0;
        do {
            auto ty = ConvertHIR_ExpandAliases_GetExpansion_GP(sp, m_crate, *cur, m_in_expr);
            if( ty == ::HIR::TypeRef() )
                break ;
            if( !ty.m_data.is_Path() )
                ERROR(sp, E0000, "Type alias referenced in generic path doesn't point to a path");
            auto& ty_p = ty.m_data.as_Path().path;
            if( !ty_p.m_data.is_Generic() )
                ERROR(sp, E0000, "Type alias referenced in generic path doesn't point to a generic path");
            rv = mv$( ty_p.m_data.as_Generic() );

            this->visit_generic_path(rv, ::HIR::Visitor::PathContext::TYPE);

            cur = &rv;
        } while( ++num_exp < MAX_RECURSIVE_TYPE_EXPANSIONS );
        ASSERT_BUG(sp, num_exp < MAX_RECURSIVE_TYPE_EXPANSIONS, "Recursion limit expanding " << path << " (currently on " << *cur << ")");
        return rv;
    }

    void visit_pattern(::HIR::Pattern& pat) override
    {
        static Span sp;
        ::HIR::Visitor::visit_pattern(pat);

        TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (e),
        (
            ),
        (StructValue,
            auto new_path = expand_alias_gp(sp, e.path);
            if( new_path.m_path.m_components.size() != 0 )
            {
                DEBUG("Replacing " << e.path << " with " << new_path);
                e.path = mv$(new_path);
            }
            ),
        (StructTuple,
            auto new_path = expand_alias_gp(sp, e.path);
            if( new_path.m_path.m_components.size() != 0 )
            {
                DEBUG("Replacing " << e.path << " with " << new_path);
                e.path = mv$(new_path);
            }
            ),
        (Struct,
            auto new_path = expand_alias_gp(sp, e.path);
            if( new_path.m_path.m_components.size() != 0 )
            {
                DEBUG("Replacing " << e.path << " with " << new_path);
                e.path = mv$(new_path);
            }
            )
        )
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

            // TODO: Use the other visitors.
            void visit_path(::HIR::Visitor::PathContext pc, ::HIR::Path& p) override
            {
                upper_visitor.visit_path(p, pc);
            }
            void visit_generic_path(::HIR::Visitor::PathContext pc, ::HIR::GenericPath& p) override
            {
                upper_visitor.visit_generic_path(p, pc);
            }

            void visit(::HIR::ExprNode_Let& node) override
            {
                upper_visitor.visit_type(node.m_type);
                upper_visitor.visit_pattern(node.m_pattern);
                ::HIR::ExprVisitorDef::visit(node);
            }
            void visit(::HIR::ExprNode_Cast& node) override
            {
                upper_visitor.visit_type(node.m_res_type);
                ::HIR::ExprVisitorDef::visit(node);
            }

            void visit(::HIR::ExprNode_CallPath& node) override
            {
                //TRACE_FUNCTION_F(node.m_path);
                upper_visitor.visit_path(node.m_path, ::HIR::Visitor::PathContext::VALUE);
                ::HIR::ExprVisitorDef::visit(node);
            }
            void visit(::HIR::ExprNode_CallMethod& node) override
            {
                upper_visitor.visit_path_params(node.m_params);
                ::HIR::ExprVisitorDef::visit(node);
            }

            void visit(::HIR::ExprNode_Closure& node) override
            {
                upper_visitor.visit_type(node.m_return);
                for(auto& arg : node.m_args) {
                    upper_visitor.visit_pattern(arg.first);
                    upper_visitor.visit_type(arg.second);
                }
                ::HIR::ExprVisitorDef::visit(node);
            }

            void visit(::HIR::ExprNode_StructLiteral& node) override
            {
                if( node.m_is_struct )
                {
                    auto new_path = upper_visitor.expand_alias_gp(node.span(), node.m_path);
                    if( new_path.m_path.m_components.size() != 0 )
                    {
                        DEBUG("Replacing " << node.m_path << " with " << new_path);
                        node.m_path = mv$(new_path);
                    }
                }

                ::HIR::ExprVisitorDef::visit(node);
            }

            void visit(::HIR::ExprNode_Match& node) override
            {
                for(auto& arm : node.m_arms) {
                    for(auto& pat : arm.m_patterns) {
                        upper_visitor.visit_pattern(pat);
                    }
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

        // HACK: Expand defaults for parameters in trait names here.
        {
            const auto& trait = m_crate.get_trait_by_path(sp, trait_path);
            auto monomorph_cb = monomorphise_type_get_cb(sp, &impl.m_type, &impl.m_trait_args, nullptr);

            while( impl.m_trait_args.m_types.size() < trait.m_params.m_types.size() )
            {
                const auto& def = trait.m_params.m_types[ impl.m_trait_args.m_types.size() ];
                auto ty = monomorphise_type_with(sp, def.m_default, monomorph_cb);
                DEBUG("Add default trait arg " << ty << " from " << def.m_default);
                impl.m_trait_args.m_types.push_back( mv$(ty) );
            }
        }

        ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        m_impl_type = nullptr;
    }
};

void ConvertHIR_ExpandAliases(::HIR::Crate& crate)
{
    Expander    exp { crate };
    exp.visit_crate( crate );
}
