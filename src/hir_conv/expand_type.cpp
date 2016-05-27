/*
 * Expand `type` aliases in HIR
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>

::HIR::TypeRef ConvertHIR_ExpandAliases_GetExpansion(const ::HIR::Crate& crate, const ::HIR::Path& path)
{
    TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
    (Generic,
        const ::HIR::Module* mod = &crate.m_root_module;
        assert( e.m_path.m_crate_name == "" && "TODO: Handle extern crates" );
        for( unsigned int i = 0; i < e.m_path.m_components.size() - 1; i ++ )
        {
            const auto& pc = e.m_path.m_components[i];
            auto it = mod->m_mod_items.find( pc );
            if( it == mod->m_mod_items.end() ) {
                BUG(Span(), "Couldn't find component " << i << " of " << e.m_path);
            }
            TU_MATCH_DEF( ::HIR::TypeItem, (it->second->ent), (e2),
            (
                BUG(Span(), "Node " << i << " of path " << e.m_path << " wasn't a module");
                ),
            (Module,
                mod = &e2;
                )
            )
        }
        auto it = mod->m_mod_items.find( e.m_path.m_components.back() );
        if( it == mod->m_mod_items.end() ) {
            BUG(Span(), "Could not find type name in " << e.m_path);
        }
        
        TU_MATCH_DEF( ::HIR::TypeItem, (it->second->ent), (e2),
        (
            ),
        (TypeAlias,
            if( e2.m_params.m_types.size() > 0 ) {
                TODO(Span(), "Replace type params in type alias");
            }
            return e2.m_type.clone();
            )
        )
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

public:
    Expander(const ::HIR::Crate& crate):
        m_crate(crate)
    {}
    
    void visit_type(::HIR::TypeRef& ty) override
    {
        ::HIR::Visitor::visit_type(ty);
        
        TU_IFLET(::HIR::TypeRef::Data, (ty.m_data), Path, (e),
            auto new_type = ConvertHIR_ExpandAliases_GetExpansion(m_crate, e);
            if( ! new_type.m_data.is_Infer() ) {
                DEBUG("Replacing " << ty << " with " << new_type);
            }
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
            
            void visit(::HIR::ExprNode_Let& node) override
            {
                upper_visitor.visit_type(node.m_type);
                ::HIR::ExprVisitorDef::visit(node);
            }
            void visit(::HIR::ExprNode_Cast& node) override
            {
                upper_visitor.visit_type(node.m_type);
                ::HIR::ExprVisitorDef::visit(node);
            }
            
            void visit(::HIR::ExprNode_CallPath& node) override
            {
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
        };
        
        if( &*expr != nullptr )
        {
            Visitor v { *this };
            (*expr).visit(v);
        }
    }
};

void ConvertHIR_ExpandAliases(::HIR::Crate& crate)
{
    Expander    exp { crate };
    exp.visit_crate( crate );
}
