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
        
        const ::HIR::GenericParams* m_impl_params;
        const ::HIR::GenericParams* m_item_params;
        const ::HIR::Trait* m_current_trait;
        
    public:
        Visitor(const ::HIR::Crate& crate):
            m_crate(crate),
            m_impl_params(nullptr),
            m_item_params(nullptr),
            m_current_trait(nullptr)
        {}
        
        void visit_module(::HIR::Module& mod) override
        {
            for( const auto& trait_path : mod.m_traits )
                m_traits.push_back( ::std::make_pair( &trait_path, &this->find_trait(trait_path) ) );
            ::HIR::Visitor::visit_module(mod);
            for(unsigned int i = 0; i < mod.m_traits.size(); i ++ )
                m_traits.pop_back();
        }

        void visit_function(::HIR::Function& fcn) override {
            m_item_params = &fcn.m_params;
            ::HIR::Visitor::visit_function(fcn);
            m_item_params = nullptr;
        }
        void visit_trait(::HIR::Trait& trait) override {
            m_current_trait = &trait;
            m_impl_params = &trait.m_params;
            ::HIR::Visitor::visit_trait(trait);
            m_impl_params = nullptr;
            m_current_trait = nullptr;
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

        bool locate_trait_item_in_bounds(::HIR::Visitor::PathContext pc,  const ::HIR::TypeRef& tr, const ::std::string& name,  const ::HIR::GenericParams& params) {
            DEBUG("TODO: Search for trait impl for " << tr << " with " << name << " in params");
            return false;
        }

        void visit_path(::HIR::Path& p, ::HIR::Visitor::PathContext pc) override
        {
            DEBUG("p = " << p);
            TU_IFLET(::HIR::Path::Data, p.m_data, UfcsUnknown, e,
                DEBUG("UfcsUnknown - p=" << p);
                
                this->visit_type( *e.type );
                this->visit_path_params( e.params );
                
                // TODO: Search for matching impls in current generic blocks
                if( m_item_params != nullptr && locate_trait_item_in_bounds(pc, *e.type, e.item, *m_item_params) ) {
                    return ;
                }
                if( m_impl_params != nullptr && locate_trait_item_in_bounds(pc, *e.type, e.item, *m_impl_params) ) {
                    return ;
                }
                
                TU_IFLET(::HIR::TypeRef::Data, e.type->m_data, Generic, te,
                    // If processing a trait, and the type is 'Self', search for the type/method on the trait
                    // - TODO: This could be encoded by a `Self: Trait` bound in the generics, but that may have knock-on issues?
                    if( te.name == "Self" && m_current_trait ) {
                        switch(pc)
                        {
                        case ::HIR::Visitor::PathContext::VALUE: {
                            auto it1 = m_current_trait->m_values.find( e.item );
                            if( it1 != m_current_trait->m_values.end() ) {
                                TODO(Span(), "Found in Self trait - need path to trait");
                                // TODO: What's the easiest way to get the path to this trait?
                                //auto new_data = ::HIR::Path::Data::make_UfcsKnown({ mv$(e.type), ::HIR::GenericPath(trait_path), mv$(e.item), mv$(e.params)} );
                                //p.m_data = mv$(new_data);
                                //DEBUG("- Resolved, replace with " << p);
                                
                                return ;
                            }
                            } break;
                        case ::HIR::Visitor::PathContext::TRAIT:
                            break;
                        case ::HIR::Visitor::PathContext::TYPE: {
                            auto it1 = m_current_trait->m_types.find( e.item );
                            if( it1 != m_current_trait->m_types.end() ) {
                                TODO(Span(), "Found in Self trait - need path to trait");
                                return ;
                            }
                            } break;
                        }
                    }
                    ERROR(Span(), E0000, "Failed to find impl with '" << e.item << "' for " << *e.type);
                    return ;
                )
                else {
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
                        case ::HIR::Visitor::PathContext::TYPE:
                            continue ;
                        }
                        
                        auto new_data = ::HIR::Path::Data::make_UfcsInherent({ mv$(e.type), mv$(e.item), mv$(e.params)} );
                        p.m_data = mv$(new_data);
                        DEBUG("- Resolved, replace with " << p);
                        return ;
                    }
                }
                
                // Couldn't find it
                DEBUG("Failed to find impl with '" << e.item << "' for " << *e.type);
            )
            else {
                ::HIR::Visitor::visit_path(p, pc);
            }
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
