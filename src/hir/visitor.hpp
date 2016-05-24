/*
 */
#pragma once

#include <hir/hir.hpp>

namespace HIR {

class Visitor
{
public:
    virtual ~Visitor();
    
    virtual void visit_crate(::HIR::Crate& crate);
    
    virtual void visit_module(::HIR::Module& mod);
    
    virtual void visit_type_impl(::HIR::TypeImpl& impl);
    virtual void visit_trait_impl(::HIR::TraitImpl& impl);
    virtual void visit_marker_impl(::HIR::MarkerImpl& impl);
    
    // - Type Items
    virtual void visit_type_alias(::HIR::TypeAlias& item);
    virtual void visit_trait(::HIR::Trait& item);
    virtual void visit_struct(::HIR::Struct& item);
    virtual void visit_enum(::HIR::Enum& item);
    // - Value Items
    virtual void visit_function(::HIR::Function& item);
    virtual void visit_static(::HIR::Static& item);
    virtual void visit_constant(::HIR::Constant& item);
    
    // - Misc
    virtual void visit_params(::HIR::GenericParams& params);
    virtual void visit_pattern(::HIR::Pattern& pat);
    virtual void visit_pattern_val(::HIR::Pattern::Value& val);
    virtual void visit_type(::HIR::TypeRef& tr);
    
    virtual void visit_path(::HIR::Path& p);
    virtual void visit_path_params(::HIR::PathParams& p);
    virtual void visit_generic_path(::HIR::GenericPath& p);

    virtual void visit_expr(::HIR::ExprPtr& exp);
};

}   // namespace HIR

