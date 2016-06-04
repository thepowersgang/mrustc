/*
 * Resolve unkown UFCS traits into inherent or trait
 *
 * HACK - Will likely be replaced with a proper typeck pass
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>

namespace {
    class Visitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
        
        ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   m_traits;
        
    public:
        Visitor(const ::HIR::Crate& crate):
            m_crate(crate)
        {}
        
        void visit_module(::HIR::Module& mod) override
        {
            for( const auto& trait_path : mod.m_traits )
                m_traits.push_back( ::std::make_pair( &trait_path, &this->find_trait(trait_path) ) );
            ::HIR::Visitor::visit_module(mod);
            for(unsigned int i = 0; i < mod.m_traits.size(); i ++ )
                m_traits.pop_back();
        }


        void visit_expr(::HIR::ExprPtr& expr) override
        {
            struct ExprVisitor:
                public ::HIR::ExprVisitorDef
            {
                Visitor& upper_visitor;
                
                ExprVisitor(Visitor& uv):
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
                
                void visit(::HIR::ExprNode_Block& node) override
                {
                    for( const auto& trait_path : node.m_traits )
                        upper_visitor.m_traits.push_back( ::std::make_pair( &trait_path, &upper_visitor.find_trait(trait_path) ) );
                    ::HIR::ExprVisitorDef::visit(node);
                    for(unsigned int i = 0; i < node.m_traits.size(); i ++ )
                        upper_visitor.m_traits.pop_back();
                }
            };
            
            if( expr.get() != nullptr )
            {
                ExprVisitor v { *this };
                (*expr).visit(v);
            }
        }

        void visit_path(::HIR::Path& p, ::HIR::Visitor::PathContext pc) override
        {
            TU_IFLET(::HIR::Path::Data, p.m_data, UfcsUnknown, e,
                DEBUG("UfcsUnknown - p=" << p);
                // 1. Search all impls of in-scope traits for this method on this type
                for( const auto& trait_info : m_traits )
                {
                    const auto& trait_path = *trait_info.first;
                    const auto& trait = *trait_info.second;
                    
                    switch( pc )
                    {
                    case ::HIR::Visitor::PathContext::VALUE: {
                        auto it1 = trait.m_values.find( e.item );
                        if( it1 == trait.m_values.end() ) {
                            continue ;
                        }
                        // Found it, just keep going (don't care about details here)
                        } break;
                    case ::HIR::Visitor::PathContext::TRAIT:
                    case ::HIR::Visitor::PathContext::TYPE: {
                        auto it1 = trait.m_types.find( e.item );
                        if( it1 == trait.m_types.end() ) {
                            continue ;
                        }
                        // Found it, just keep going (don't care about details here)
                        } break;
                    }
                        
                    auto trait_impl_it = m_crate.m_trait_impls.equal_range( trait_path );
                    if( trait_impl_it.first == trait_impl_it.second ) {
                        continue ;
                    }
                    for( auto it = trait_impl_it.first; it != trait_impl_it.second; it ++ )
                    {
                        const auto& impl = it->second;
                        if( !impl.matches_type(*e.type) ) {
                            continue ;
                        }
                        
                        auto new_data = ::HIR::Path::Data::make_UfcsKnown({ mv$(e.type), ::HIR::GenericPath(trait_path), mv$(e.item), mv$(e.params)} );
                        p.m_data = mv$(new_data);
                        DEBUG("- Resolved, replace with " << p);
                        return ;
                    }
                }
                
                // 2. No trait matched, search for inherent impl
                for( const auto& impl : m_crate.m_type_impls )
                {
                    if( !impl.matches_type(*e.type) ) {
                        continue ;
                    }
                    DEBUG("- matched impl " << *e.type);
                    // TODO: Search for item
                    switch( pc )
                    {
                    case ::HIR::Visitor::PathContext::VALUE: {
                        auto it1 = impl.m_methods.find( e.item );
                        if( it1 == impl.m_methods.end() ) {
                            continue ;
                        }
                        // Found it, just keep going (don't care about details here)
                        } break;
                    case ::HIR::Visitor::PathContext::TRAIT:
                    case ::HIR::Visitor::PathContext::TYPE: {
                        continue ;
                        // Found it, just keep going (don't care about details here)
                        } break;
                    }
                    
                    auto new_data = ::HIR::Path::Data::make_UfcsInherent({ mv$(e.type), mv$(e.item), mv$(e.params)} );
                    p.m_data = mv$(new_data);
                    DEBUG("- Resolved, replace with " << p);
                    return ;
                }
                
                // Couldn't find it
                DEBUG("Failed to find impl with '" << e.item << "' for " << *e.type);
            )
        }
        
        
        const ::HIR::Trait& find_trait(const ::HIR::SimplePath& path) const
        {
            if( path.m_crate_name != "" )
                TODO(Span(), "find_trait in crate");
            
            const ::HIR::Module* mod = &m_crate.m_root_module;
            for( unsigned int i = 0; i < path.m_components.size() - 1; i ++ )
            {
                const auto& pc = path.m_components[i];
                auto it = mod->m_mod_items.find( pc );
                if( it == mod->m_mod_items.end() ) {
                    BUG(Span(), "Couldn't find component " << i << " of " << path);
                }
                TU_MATCH_DEF( ::HIR::TypeItem, (it->second->ent), (e2),
                (
                    BUG(Span(), "Node " << i << " of path " << path << " wasn't a module");
                    ),
                (Module,
                    mod = &e2;
                    )
                )
            }
            auto it = mod->m_mod_items.find( path.m_components.back() );
            if( it == mod->m_mod_items.end() ) {
                BUG(Span(), "Could not find type name in " << path);
            }
            
            TU_IFLET( ::HIR::TypeItem, it->second->ent, Trait, e,
                return e;
            )
            else {
                BUG(Span(), "Trait path " << path << " didn't point to a trait");
            }
        }
    };

}

void ConvertHIR_ResolveUFCS(::HIR::Crate& crate)
{
    Visitor exp { crate };
    exp.visit_crate( crate );
}
