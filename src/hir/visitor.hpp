/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/visitor.hpp
 * - HIR Outer Visitor
 *
 * Calls methods on each item type in the HIR (and on paths/types/patterns)
 * Does NOT visit expression nodes
 */
#pragma once

#include <hir/hir.hpp>
#include <hir/item_path.hpp>

namespace HIR {

// TODO: Split into Visitor and ItemVisitor
class Visitor
{
public:
    virtual ~Visitor();

    virtual void visit_crate(::HIR::Crate& crate);

    virtual void visit_module(ItemPath p, ::HIR::Module& mod);

    virtual void visit_type_impl(::HIR::TypeImpl& impl);
    virtual void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl);
    virtual void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl);

    // - Type Items
    virtual void visit_type_alias(ItemPath p, ::HIR::TypeAlias& item);
    virtual void visit_trait_alias(::HIR::ItemPath p, ::HIR::TraitAlias& item);
    virtual void visit_trait(ItemPath p, ::HIR::Trait& item);
    virtual void visit_struct(ItemPath p, ::HIR::Struct& item);
    virtual void visit_enum(ItemPath p, ::HIR::Enum& item);
    virtual void visit_union(ItemPath p, ::HIR::Union& item);
    virtual void visit_associatedtype(ItemPath p, ::HIR::AssociatedType& item);
    // - Value Items
    virtual void visit_function(ItemPath p, ::HIR::Function& item);
    virtual void visit_static(ItemPath p, ::HIR::Static& item);
    virtual void visit_constant(ItemPath p, ::HIR::Constant& item);

    // - Misc
    virtual void visit_params(::HIR::GenericParams& params);
    virtual void visit_generic_bound(::HIR::GenericBound& bound);
    virtual void visit_pattern(::HIR::Pattern& pat);
    virtual void visit_pattern_val(::HIR::Pattern::Value& val);

    virtual void visit_type(::HIR::TypeRef& tr);
    virtual void visit_constgeneric(::HIR::ConstGeneric& c);

    enum class PathContext {
        TYPE,
        TRAIT,

        VALUE,
    };
    virtual void visit_trait_path(::HIR::TraitPath& p);
    virtual void visit_path(::HIR::Path& p, PathContext );
    virtual void visit_path_params(::HIR::PathParams& p);
    virtual void visit_generic_path(::HIR::GenericPath& p, PathContext );

    virtual void visit_expr(::HIR::ExprPtr& exp);
};

}   // namespace HIR

