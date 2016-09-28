/*
 * Expand `type` aliases in HIR
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>
#include <hir_typeck/common.hpp>    // monomorphise_type_with

::HIR::TypeRef ConvertHIR_ExpandAliases_GetExpansion(const ::HIR::Crate& crate, const ::HIR::Path& path)
{
    static Span sp;
    TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
    (Generic,
        const auto& ti = crate.get_typeitem_by_path(sp, e.m_path);
        TU_MATCH_DEF( ::HIR::TypeItem, (ti), (e2),
        (
            // Anything else - leave it be
            ),
        (TypeAlias,
            if( e.m_params.m_types.size() != e2.m_params.m_types.size() ) {
                ERROR(sp, E0000, "Mismatched parameter count in " << path);
            }
            if( e2.m_params.m_types.size() > 0 ) {
                // TODO: Better `monomorphise_type`
                return monomorphise_type_with(sp, e2.m_type, [&](const auto& gt)->const auto& {
                    const auto& ge = gt.m_data.as_Generic();
                    if( ge.binding == 0xFFFF ) {
                        BUG(sp, "Self encountered in expansion for " << path << " - " << e2.m_type);
                    }
                    else if( (ge.binding >> 8) == 0 ) {
                        auto idx = ge.binding & 0xFF;
                        ASSERT_BUG(sp, idx < e.m_params.m_types.size(), "");
                        return e.m_params.m_types[idx];
                    }
                    else {
                        BUG(sp, "Bad index " << ge.binding << " encountered in expansion for " << path << " - " << e2.m_type);
                    }
                    });
            }
            else {
                return e2.m_type.clone();
            }
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
            ::HIR::TypeRef  new_type = ConvertHIR_ExpandAliases_GetExpansion(m_crate, e.path);
            // Keep trying to expand down the chain
            unsigned int num_exp = 1;
            const unsigned int MAX_RECURSIVE_TYPE_EXPANSIONS = 100;
            while(num_exp < MAX_RECURSIVE_TYPE_EXPANSIONS)
            {
                TU_IFLET(::HIR::TypeRef::Data, (new_type.m_data), Path, (e),
                    auto nt = ConvertHIR_ExpandAliases_GetExpansion(m_crate, e.path);
                    if( nt == ::HIR::TypeRef() )
                        break;
                    num_exp ++;
                    new_type = mv$(nt);
                )
                else {
                    break;
                }
            }
            ASSERT_BUG(Span(), num_exp < MAX_RECURSIVE_TYPE_EXPANSIONS, "Recursion limit hit expanding " << ty << " (currently on " << new_type << ")");
            if( ! new_type.m_data.is_Infer() ) {
                DEBUG("Replacing " << ty << " with " << new_type << " (" << num_exp << " expansions)");
                ty = mv$(new_type);
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
                upper_visitor.visit_type(node.m_res_type);
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
        
        if( expr.get() != nullptr )
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
