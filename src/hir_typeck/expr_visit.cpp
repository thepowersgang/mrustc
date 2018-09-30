/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/expr_visit.cpp
 * - Wrapper around HIR typecheck that visits all expressions
 */
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>
#include "expr_visit.hpp"
#include <hir/expr_state.hpp>

void Typecheck_Code(const typeck::ModuleState& ms, t_args& args, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr) {
    if( expr.m_state->stage < ::HIR::ExprState::Stage::Typecheck )
    {
        //Typecheck_Code_Simple(ms, args, result_type, expr);
        Typecheck_Code_CS(ms, args, result_type, expr);
    }
}

namespace {


    class OuterVisitor:
        public ::HIR::Visitor
    {
        ::typeck::ModuleState m_ms;
    public:
        OuterVisitor(::HIR::Crate& crate):
            m_ms(crate)
        {
        }


    public:
        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            m_ms.push_traits(mod);
            ::HIR::Visitor::visit_module(p, mod);
            m_ms.pop_traits(mod);
        }

        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(exp->m_span, "Reached expression");
        }

        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override
        {
            auto _ = this->m_ms.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }

        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = this->m_ms.set_impl_generics(impl.m_params);

            const auto& mod = this->m_ms.m_crate.get_mod_by_path(Span(), impl.m_src_module);
            m_ms.push_traits(mod);
            ::HIR::Visitor::visit_type_impl(impl);
            m_ms.pop_traits(mod);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            auto _ = this->m_ms.set_impl_generics(impl.m_params);

            const auto& mod = this->m_ms.m_crate.get_mod_by_path(Span(), impl.m_src_module);
            m_ms.push_traits(mod);
            m_ms.m_traits.push_back( ::std::make_pair( &trait_path, &this->m_ms.m_crate.get_trait_by_path(Span(), trait_path) ) );
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            m_ms.m_traits.pop_back( );
            m_ms.pop_traits(mod);
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type << " { }");
            auto _ = this->m_ms.set_impl_generics(impl.m_params);

            const auto& mod = this->m_ms.m_crate.get_mod_by_path(Span(), impl.m_src_module);
            m_ms.push_traits(mod);
            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
            m_ms.pop_traits(mod);
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                DEBUG("Array size " << ty);
                t_args  tmp;
                if( e.size ) {
                    Typecheck_Code( m_ms, tmp, ::HIR::TypeRef(::HIR::CoreType::Usize), *e.size );
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
            auto _ = this->m_ms.set_item_generics(item.m_params);
            if( item.m_code )
            {
                DEBUG("Function code " << p);
                Typecheck_Code( m_ms, item.m_args, item.m_return, item.m_code );
            }
            else
            {
                DEBUG("Function code " << p << " (none)");
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            //auto _ = this->m_ms.set_item_generics(item.m_params);
            if( item.m_value )
            {
                DEBUG("Static value " << p);
                t_args  tmp;
                Typecheck_Code(m_ms, tmp, item.m_type, item.m_value);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            auto _ = this->m_ms.set_item_generics(item.m_params);
            if( item.m_value )
            {
                DEBUG("Const value " << p);
                t_args  tmp;
                Typecheck_Code(m_ms, tmp, item.m_type, item.m_value);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            auto _ = this->m_ms.set_item_generics(item.m_params);

            if( auto* e = item.m_data.opt_Value() )
            {
                // TODO: Use a different type depding on repr()
                auto enum_type = ::HIR::TypeRef(::HIR::CoreType::Isize);

                for(auto& var : e->variants)
                {
                    DEBUG("Enum value " << p << " - " << var.name);
                    if( var.expr )
                    {
                        t_args  tmp;
                        Typecheck_Code(m_ms, tmp, enum_type, var.expr);
                    }
                }
            }
        }
    };
}

void Typecheck_Expressions(::HIR::Crate& crate)
{
    OuterVisitor    visitor { crate };
    visitor.visit_crate( crate );
}
