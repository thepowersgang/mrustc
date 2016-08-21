/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/check.cpp
 * - MIR Correctness validation
 */
#include "main_bindings.hpp"
#include "mir.hpp"
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>

void MIR_Validate(const StaticTraitResolve& resolve, const ::MIR::Function& fcn)
{
    
}

namespace {
    // TODO: Create visitor that handles setting up a StaticTraitResolve?
    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_resolve(crate)
        {}
        
        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                DEBUG("Array size " << ty);
                if( e.size ) {
                    MIR_Validate(m_resolve, *e.size.m_mir);
                }
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }

        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            if( item.m_code ) {
                DEBUG("Function code " << p);
                MIR_Validate(m_resolve, *item.m_code.m_mir);
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            if( item.m_value ) {
                DEBUG("`static` value " << p);
                MIR_Validate(m_resolve, *item.m_value.m_mir);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value ) {
                DEBUG("`const` value " << p);
                MIR_Validate(m_resolve, *item.m_value.m_mir);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    MIR_Validate(m_resolve, *e.m_mir);
                )
            }
        }
        
        // Boilerplate
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override {
            auto _ = this->m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override {
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override {
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
    };
}

// --------------------------------------------------------------------

void MIR_CheckCrate(/*const*/ ::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}
