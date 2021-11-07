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

namespace typeck {
    void ModuleState::prepare_from_path(const ::HIR::ItemPath& ip)
    {
        static Span sp;
        ASSERT_BUG(sp, ip.parent, "prepare_from_path with too-short path - " << ip);
        struct H {
            static const ::HIR::Module& get_mod_for_ip(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip)
            {
                if( ip.parent )
                {
                    const auto& mod = H::get_mod_for_ip(crate, *ip.parent);
                    return mod.m_mod_items.at(ip.name)->ent.as_Module();
                }
                else
                {
                    assert(ip.crate_name);
                    return (ip.crate_name[0] ? crate.m_ext_crates.at(ip.crate_name).m_data->m_root_module : crate.m_root_module);
                }
            }
            static void add_traits_from_mod(ModuleState& ms, const ::HIR::Module& mod)
            {
                // In-scope traits.
                ms.m_traits.clear();
                for(const auto& tp : mod.m_traits)
                {
                    const auto& trait = ms.m_crate.get_trait_by_path(sp, tp);
                    ms.m_traits.push_back(::std::make_pair( &tp, &trait ));
                }
            }
        };
        if( ip.parent->trait && ip.parent->ty )
        {
            // Trait impl
            TODO(sp, "prepare_from_path - Trait impl " << ip);
        }
        else if( ip.parent->trait )
        {
            // Trait definition
            //const auto& trait_mod = H::get_mod_for_ip(m_crate, *ip.parent->trait->parent);
            //const auto& trait = trait_mod.m_mod_items.at(ip.parent->trait->name).ent.as_Trait();
            const auto& trait = m_crate.get_trait_by_path(sp, *ip.parent->trait);
            const auto& item = trait.m_values.at(ip.name);
            TU_MATCH_HDRA( (item), { )
            TU_ARMA(Function, e) {
                m_item_generics = &e.m_params;
                }
            TU_ARMA(Constant, e) {
                m_item_generics = &e.m_params;
                }
            TU_ARMA(Static, e) {
                m_item_generics = nullptr;
                }
            }
        }
        else if( ip.parent->ty )
        {
            // Inherent impl
            TODO(sp, "prepare_from_path - Type impl " << ip);
        }
        else
        {
            // Namespace path
            const auto& mod = H::get_mod_for_ip(m_crate, *ip.parent);
            H::add_traits_from_mod(*this, mod);
            const auto& item = mod.m_value_items.at(ip.name)->ent;
            m_impl_generics = nullptr;
            TU_MATCH_HDRA( (item), { )
            TU_ARMA(Constant, e) {
                m_item_generics = &e.m_params;
                }
            TU_ARMA(Static, e) {
                //m_item_generics = &e.m_params;
                }
            TU_ARMA(Function, e) {
                m_item_generics = &e.m_params;
                }
            TU_ARMA(StructConstant, _e) BUG(sp, ip << " is StructConstant");
            TU_ARMA(StructConstructor, _e) BUG(sp, ip << " is StructConstructor");
            TU_ARMA(Import, _e) BUG(sp, ip << " is Import");
            }
        }
    }
} // namespace typeck

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
            m_ms.push_traits(p, mod);
            ::HIR::Visitor::visit_module(p, mod);
            m_ms.pop_traits(mod);
        }

        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(exp->m_span, "Reached expression");
        }

        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override
        {
            HIR::GenericPath    trait_gpath;
            trait_gpath.m_path = p.get_simple_path();
            for(size_t i = 0; i < item.m_params.m_types.size(); i ++) {
                trait_gpath.m_params.m_types.push_back(HIR::TypeRef(item.m_params.m_types[i].m_name, i));
            }
            for(size_t i = 0; i < item.m_params.m_values.size(); i ++) {
                trait_gpath.m_params.m_values.push_back(HIR::GenericRef(item.m_params.m_values[i].m_name, i));
            }
            auto _1 = this->m_ms.set_current_trait(trait_gpath);
            auto _ = this->m_ms.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }

        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = this->m_ms.set_impl_generics(impl.m_params);

            const auto& mod = this->m_ms.m_crate.get_mod_by_path(Span(), impl.m_src_module);
            m_ms.push_traits(impl.m_src_module, mod);
            ::HIR::Visitor::visit_type_impl(impl);
            m_ms.pop_traits(mod);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << impl.m_trait_args << " for " << impl.m_type);
            auto trait_gpath = ::HIR::GenericPath(trait_path, impl.m_trait_args.clone());
            auto _1 = this->m_ms.set_current_trait(trait_gpath);
            auto _ = this->m_ms.set_impl_generics(impl.m_params);

            const auto& mod = this->m_ms.m_crate.get_mod_by_path(Span(), impl.m_src_module);
            m_ms.push_traits(impl.m_src_module, mod);
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
            m_ms.push_traits(impl.m_src_module, mod);
            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
            m_ms.pop_traits(mod);
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            if(auto* e = ty.data_mut().opt_Array())
            {
                this->visit_type( e->inner );
                DEBUG("Array size " << ty);
                t_args  tmp;
                if( auto* se = e->size.opt_Unevaluated() ) {
                    if( se->is_Unevaluated() ) {
                        Typecheck_Code( m_ms, tmp, ::HIR::TypeRef(::HIR::CoreType::Usize), *se->as_Unevaluated() );
                    }
                }
            }
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
                auto enum_type = ::HIR::Enum::get_repr_type(item.m_tag_repr);

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
