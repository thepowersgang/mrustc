/*
 */
#pragma once

#include <hir/hir.hpp>

namespace HIR {

class PathChain
{
    const PathChain*    prev;
    const ::std::string&    name;
public:
    PathChain(const PathChain& prev, const ::std::string& name):
        prev(&prev),
        name(name)
    {}
    PathChain(const ::std::string& name):
        prev(nullptr),
        name(name)
    {}
    
    PathChain operator+(const ::std::string& name) const {
        return PathChain(*this, name);
    }
    
    ::HIR::SimplePath to_path() const {
        if( prev ) {
            return prev->to_path() + name;
        }
        else {
            if( name != "" ) {
                return ::HIR::SimplePath("", {name});
            }
            else {
                return ::HIR::SimplePath("", {});
            }
        }
    }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const PathChain& x) {
        if( x.prev )
            os << *x.prev << "::";
        os << x.name;
        return os;
    }
};

class Visitor
{
public:
    virtual ~Visitor();
    
    virtual void visit_crate(::HIR::Crate& crate);
    
    virtual void visit_module(PathChain p, ::HIR::Module& mod);
    
    virtual void visit_type_impl(::HIR::TypeImpl& impl);
    virtual void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl);
    virtual void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl);
    
    // - Type Items
    virtual void visit_type_alias(PathChain p, ::HIR::TypeAlias& item);
    virtual void visit_trait(PathChain p, ::HIR::Trait& item);
    virtual void visit_struct(PathChain p, ::HIR::Struct& item);
    virtual void visit_enum(PathChain p, ::HIR::Enum& item);
    // - Value Items
    virtual void visit_function(PathChain p, ::HIR::Function& item);
    virtual void visit_static(PathChain p, ::HIR::Static& item);
    virtual void visit_constant(PathChain p, ::HIR::Constant& item);
    
    // - Misc
    virtual void visit_params(::HIR::GenericParams& params);
    virtual void visit_pattern(::HIR::Pattern& pat);
    virtual void visit_pattern_val(::HIR::Pattern::Value& val);
    virtual void visit_type(::HIR::TypeRef& tr);
    
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

