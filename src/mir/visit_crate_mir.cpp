/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/visit_crate_mir.cpp
 * - Visitor to visit all MIR blobs in a crate
 */
#include "visit_crate_mir.hpp"
#include <hir/expr.hpp>

// NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
void MIR::OuterVisitor::visit_expr(::HIR::ExprPtr& exp)
{
    BUG(Span(), "visit_expr hit in OuterVisitor");
}

void MIR::OuterVisitor::visit_type(::HIR::TypeRef& ty)
{
    if(auto* e = ty.data_mut().opt_Array())
    {
        this->visit_type( e->inner );
        DEBUG("Array size " << ty);
        if( auto* se1 = e->size.opt_Unevaluated() ) {
            if( auto* se = se1->opt_Unevaluated() ) {
                m_cb(m_resolve, ::HIR::ItemPath(""), **se, {}, ::HIR::TypeRef(::HIR::CoreType::Usize));
            }
        }
    }
    else {
        ::HIR::Visitor::visit_type(ty);
    }
}

void MIR::OuterVisitor::visit_function(::HIR::ItemPath p, ::HIR::Function& item)
{
    auto _ = this->m_resolve.set_item_generics(item.m_params);
    if( item.m_code )
    {
        DEBUG("Function code " << p);
        m_cb(m_resolve, p, item.m_code, item.m_args, item.m_return);
    }
}
void MIR::OuterVisitor::visit_static(::HIR::ItemPath p, ::HIR::Static& item)
{
    if( item.m_value ) {
        DEBUG("`static` value " << p);
        m_cb(m_resolve, p, item.m_value, {}, item.m_type);
    }
}
void MIR::OuterVisitor::visit_constant(::HIR::ItemPath p, ::HIR::Constant& item)
{
    if( item.m_value ) {
        DEBUG("`const` value " << p);
        m_cb(m_resolve, p, item.m_value, {}, item.m_type);
    }
}
void MIR::OuterVisitor::visit_enum(::HIR::ItemPath p, ::HIR::Enum& item)
{
    auto _ = this->m_resolve.set_item_generics(item.m_params);

    if( auto* e = item.m_data.opt_Value() )
    {
        auto enum_type = ::HIR::Enum::get_repr_type(item.m_tag_repr);

        for(auto& var : e->variants)
        {
            if( var.expr ) {
                m_cb(m_resolve, p + var.name, var.expr, {}, enum_type);
            }
        }
    }
}

void MIR::OuterVisitor::visit_trait(::HIR::ItemPath p, ::HIR::Trait& item)
{
    auto _ = this->m_resolve.set_impl_generics(item.m_params);
    ::HIR::Visitor::visit_trait(p, item);
}
void MIR::OuterVisitor::visit_type_impl(::HIR::TypeImpl& impl)
{
    auto _ = this->m_resolve.set_impl_generics(impl.m_params);
    ::HIR::Visitor::visit_type_impl(impl);
}
void MIR::OuterVisitor::visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl)
{
    auto _ = this->m_resolve.set_impl_generics(impl.m_params);
    ::HIR::Visitor::visit_trait_impl(trait_path, impl);
}
