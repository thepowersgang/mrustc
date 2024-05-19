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

namespace {

    void expand_erased_type(const Span& sp, const StaticTraitResolve& m_resolve, HIR::TypeRef& ty)
    {
        const auto& e = ty.data().as_ErasedType();

        HIR::TypeRef    new_ty;
        TU_MATCH_HDRA( (e.m_inner), { )
        TU_ARMA(Fcn, ee) {
            MonomorphState    monomorph_cb;
            auto val = m_resolve.get_value(sp, ee.m_origin, monomorph_cb);
            const auto& fcn = *val.as_Function();
            const auto& erased_types = fcn.m_code.m_erased_types;

            ASSERT_BUG(sp, ee.m_index < erased_types.size(), "Erased type index out of range for " << ee.m_origin << " - " << ee.m_index << " >= " << erased_types.size());
            const auto& tpl = erased_types[ee.m_index];

            new_ty = monomorph_cb.monomorph_type(sp, tpl);
            m_resolve.expand_associated_types(sp, new_ty);
            }
        TU_ARMA(Alias, ee) {
            if( ee.inner->type == HIR::TypeRef() ) {
                ERROR(Span(), E0000, "Erased type alias " << ee.inner->type << " never set?");
            }
            new_ty = MonomorphStatePtr(nullptr, &ee.params, nullptr).monomorph_type(sp, ee.inner->type);
            m_resolve.expand_associated_types(sp, new_ty);
            }
        TU_ARMA(Known, ee) {
            new_ty = ee.clone();
            }
        }
        DEBUG("> " << ty << " => " << new_ty);
        struct M: public MonomorphiserNop {
            HIR::LifetimeRef monomorph_lifetime(const Span& sp, const HIR::LifetimeRef& lft) const override {
                ASSERT_BUG(sp, lft.binding < ::HIR::LifetimeRef::MAX_LOCAL, "Found local/ivar lifetime - " << lft);
                return lft;
            }
        };
        M().monomorph_type(sp, new_ty);
        ty = mv$(new_ty);
    }

    void visit_type(const Span& sp, const StaticTraitResolve& resolve, ::HIR::TypeRef& ty) {
        TRACE_FUNCTION_FR(ty, ty);
        class V:
            public ::HIR::Visitor
        {
            const Span& sp;
            const StaticTraitResolve&  m_resolve;
            bool clear_opaque;
        public:
            V(const Span& sp, const StaticTraitResolve& resolve)
                : sp(sp)
                , m_resolve(resolve)
                , clear_opaque(false)
            {}

            void visit_type(::HIR::TypeRef& ty) override
            {
                static const Span   sp;
                auto saved_clear_opaque = this->clear_opaque;
                this->clear_opaque = false;
                if( ty.data().is_ErasedType() )
                {
                    TRACE_FUNCTION_FR(ty, ty);

                    expand_erased_type(sp, m_resolve, ty);

                    // Recurse (TODO: Cleanly prevent infinite recursion - TRACE_FUNCTION does crude prevention)
                    this->visit_type(ty);
                    this->clear_opaque = true;
                }
                else
                {
                    ::HIR::Visitor::visit_type(ty);
                    // If there was an erased type anywhere within this type, then clear an Opaque binding so EAT runs again
                    if( auto* p = ty.data_mut().opt_Path() ) {
                        // NOTE: This is both an optimisation, and avoids issues (if all types are cleared, the alias list in
                        // `StaticTraitResolve` ends up with un-expanded ATYs which leads to expansion not happening when it shoud.
                        if( this->clear_opaque && p->binding.is_Opaque() ) {
                            p->binding = HIR::TypePathBinding::make_Unbound({});
                        }
                    }
                }
                this->clear_opaque |= saved_clear_opaque;
            }
        } v(sp, resolve);
        v.visit_type(ty);
        resolve.expand_associated_types(sp, ty);
    }

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
            ::visit_type(sp, m_resolve, ty);
        }
    };

    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
    public:
        OuterVisitor(const ::HIR::Crate& crate)
            : ::HIR::Visitor(&m_resolve)
            , m_resolve(crate)
        {}

        void visit_expr(::HIR::ExprPtr& exp) override
        {
            if( exp )
            {
                ExprVisitor_Extract    ev(m_resolve);
                ev.visit_root( exp );
            }
        }
    };
    class OuterVisitor_Fixup:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
    public:
        OuterVisitor_Fixup(const ::HIR::Crate& crate)
            : ::HIR::Visitor(&m_resolve)
            , m_resolve(crate)
        {}

        void visit_type(::HIR::TypeRef& ty) override
        {
            static Span sp;
            ::visit_type(sp, m_resolve, ty);
        }
    };
}

void HIR_Expand_ErasedType(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );

    OuterVisitor_Fixup  ov_fix(crate);
    ov_fix.visit_crate(crate);
}

