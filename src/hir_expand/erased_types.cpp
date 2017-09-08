/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/erased_types.cpp
 * - HIR Expansion - Replace `impl Trait` with the real type
 */
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/static.hpp>
#include <algorithm>
#include "main_bindings.hpp"

const ::HIR::Function& HIR_Expand_ErasedType_GetFunction(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::Path& origin_path, t_cb_generic& monomorph_cb, ::HIR::PathParams& impl_params)
{
    const ::HIR::Function*  fcn_ptr = nullptr;
    switch(origin_path.m_data.tag())
    {
    case ::HIR::Path::Data::TAGDEAD:
        BUG(Span(), "DEAD in ErasedType - " << origin_path);
    case ::HIR::Path::Data::TAG_UfcsUnknown:
        BUG(Span(), "UfcsUnknown in ErasedType - " << origin_path);
    case ::HIR::Path::Data::TAG_Generic: {
        const auto& pe = origin_path.m_data.as_Generic();
        monomorph_cb = monomorphise_type_get_cb(sp, nullptr, nullptr, &pe.m_params);
        fcn_ptr = &resolve.m_crate.get_function_by_path(sp, pe.m_path);
        } break;
    case ::HIR::Path::Data::TAG_UfcsKnown:
        // NOTE: This isn't possible yet (will it be? or will it expand to an associated type?)
        TODO(sp, "Replace ErasedType - " << origin_path << " with source (UfcsKnown)");
        break;
    case ::HIR::Path::Data::TAG_UfcsInherent: {
        const auto& pe = origin_path.m_data.as_UfcsInherent();
        // 1. Find correct impl block for the path
        const ::HIR::TypeImpl* impl_ptr = nullptr;
        resolve.m_crate.find_type_impls(*pe.type, [&](const auto& ty)->const auto& { return ty; },
            [&](const auto& impl) {
                DEBUG("- impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                auto it = impl.m_methods.find(pe.item);
                if( it == impl.m_methods.end() )
                    return false;
                fcn_ptr = &it->second.data;
                impl_ptr = &impl;
                return true;
            });
        ASSERT_BUG(sp, fcn_ptr, "Failed to locate function " << origin_path);
        assert(impl_ptr);

        // 2. Obtain monomorph_cb (including impl params)
        impl_params.m_types.resize(impl_ptr->m_params.m_types.size());
        impl_ptr->m_type .match_test_generics(sp, *pe.type, [](const auto& x)->const auto&{return x;}, [&](auto idx, const auto& /*name*/, const auto& ty) {
            assert( idx < impl_params.m_types.size() );
            impl_params.m_types[idx] = ty.clone();
            return ::HIR::Compare::Equal;
            });
        for(const auto& t : impl_params.m_types)
        {
            if( t == ::HIR::TypeRef() )
            {
                TODO(sp, "Handle ErasedType where an impl parameter comes from a bound - " << origin_path);
            }
        }

        monomorph_cb = monomorphise_type_get_cb(sp, &*pe.type, &impl_params, &pe.params);
        } break;
    }
    assert(fcn_ptr);
    return *fcn_ptr;
}

namespace {


    class ExprVisitor_Extract:
        public ::HIR::ExprVisitorDef
    {
        const StaticTraitResolve& m_resolve;

    public:
        ExprVisitor_Extract(const StaticTraitResolve& resolve):
            m_resolve(resolve)
        {
        }

        void visit_root(::HIR::ExprPtr& root)
        {
            root->visit(*this);
            visit_type(root->m_res_type);
            for(auto& ty : root.m_bindings)
                visit_type(ty);
            for(auto& ty : root.m_erased_types)
                visit_type(ty);
        }

        void visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) override {
            assert(node_ptr);
            node_ptr->visit(*this);
            visit_type(node_ptr->m_res_type);
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            static Span sp;

            if( ty.m_data.is_ErasedType() )
            {
                TRACE_FUNCTION_FR(ty, ty);

                const auto& e = ty.m_data.as_ErasedType();

                ::HIR::PathParams   impl_params;    // cache.
                t_cb_generic    monomorph_cb;
                const auto& fcn = HIR_Expand_ErasedType_GetFunction(sp, m_resolve, e.m_origin, monomorph_cb, impl_params);
                const auto& erased_types = fcn.m_code.m_erased_types;

                ASSERT_BUG(sp, e.m_index < erased_types.size(), "Erased type index out of range for " << e.m_origin << " - " << e.m_index << " >= " << erased_types.size());
                const auto& tpl = erased_types[e.m_index];

                auto new_ty = monomorphise_type_with(sp, tpl, monomorph_cb);
                DEBUG("> " << ty << " => " << new_ty);
                ty = mv$(new_ty);
                // Recurse (TODO: Cleanly prevent infinite recursion - TRACE_FUNCTION does crude prevention)
                visit_type(ty);
            }
            else
            {
                ::HIR::ExprVisitorDef::visit_type(ty);
            }
        }
    };

    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
        const ::HIR::ItemPath* m_fcn_path = nullptr;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_resolve(crate)
        {}

        void visit_expr(::HIR::ExprPtr& exp) override
        {
            if( exp )
            {
                ExprVisitor_Extract    ev(m_resolve);
                ev.visit_root( exp );
            }
        }

        void visit_function(::HIR::ItemPath p, ::HIR::Function& fcn) override
        {
            m_fcn_path = &p;
            ::HIR::Visitor::visit_function(p, fcn);
            m_fcn_path = nullptr;
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            static const Span   sp;
            if( ty.m_data.is_ErasedType() )
            {
                //ASSERT_BUG(sp, m_fcn_path, "Erased type outside of a function - " << ty);

                const auto& e = ty.m_data.as_ErasedType();

                TU_MATCHA( (e.m_origin.m_data), (pe),
                (Generic,
                    if( m_fcn_path && *m_fcn_path == pe.m_path ) {
                        ::HIR::Visitor::visit_type(ty);
                        return ;
                    }
                    ),
                (UfcsUnknown,
                    BUG(sp, "UfcsUnknown unexpected");
                    ),
                (UfcsKnown,
                    BUG(sp, "UfcsKnown not supported");
                    ),
                (UfcsInherent,
                    if( m_fcn_path && m_fcn_path->parent && m_fcn_path->parent->ty && !m_fcn_path->parent->trait && *m_fcn_path->parent->ty == *pe.type && m_fcn_path->name == pe.item ) {
                        ::HIR::Visitor::visit_type(ty);
                        return ;
                    }
                    )
                )

                TRACE_FUNCTION_FR(ty, ty);

                ::HIR::PathParams   impl_params;
                t_cb_generic    monomorph_cb;
                const auto& fcn = HIR_Expand_ErasedType_GetFunction(sp, m_resolve, e.m_origin, monomorph_cb, impl_params);
                const auto& erased_types = fcn.m_code.m_erased_types;

                ASSERT_BUG(sp, e.m_index < erased_types.size(), "Erased type index out of range for " << e.m_origin << " - " << e.m_index << " >= " << erased_types.size());
                const auto& tpl = erased_types[e.m_index];

                auto new_ty = monomorphise_type_with(sp, tpl, monomorph_cb);
                DEBUG("> " << ty << " => " << new_ty);
                ty = mv$(new_ty);
                // Recurse (TODO: Cleanly prevent infinite recursion - TRACE_FUNCTION does crude prevention)
                visit_type(ty);
            }
            else
            {
                ::HIR::Visitor::visit_type(ty);
            }
        }
    };
}

void HIR_Expand_ErasedType(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

