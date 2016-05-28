/*
 * Check types (and evaluate constants) at the module level
 */
#include <hir/hir.hpp>
#include <hir/visitor.hpp>

namespace {
    
    const ::HIR::GenericParams& get_params_for_item(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path, ::HIR::Visitor::PathContext pc)
    {
        if( path.m_crate_name != "" )
            TODO(sp, "get_params_for_item in crate");
        
        const ::HIR::Module* mod = &crate.m_root_module;
        for( unsigned int i = 0; i < path.m_components.size() - 1; i ++ )
        {
            const auto& pc = path.m_components[i];
            auto it = mod->m_mod_items.find( pc );
            if( it == mod->m_mod_items.end() ) {
                BUG(sp, "Couldn't find component " << i << " of " << path);
            }
            TU_MATCH_DEF( ::HIR::TypeItem, (it->second->ent), (e2),
            (
                BUG(sp, "Node " << i << " of path " << path << " wasn't a module");
                ),
            (Module,
                mod = &e2;
                )
            )
        }
        
        switch( pc )
        {
        case ::HIR::Visitor::PathContext::VALUE: {
            auto it = mod->m_value_items.find( path.m_components.back() );
            if( it == mod->m_value_items.end() ) {
                BUG(sp, "Couldn't find final component of " << path);
            }
            
            TU_MATCH( ::HIR::ValueItem, (it->second->ent), (e),
            (Import,
                BUG(sp, "Value path pointed to import");
                ),
            (Function,
                return e.m_params;
                ),
            (Constant,
                return e.m_params;
                ),
            (Static,
                // TODO: Return an empty set?
                BUG(sp, "Attepted to get parameters for static");
                ),
            (StructConstructor,
                return get_params_for_item(sp, crate, e.ty, ::HIR::Visitor::PathContext::TYPE);
                ),
            (StructConstant,
                return get_params_for_item(sp, crate, e.ty, ::HIR::Visitor::PathContext::TYPE);
                )
            )
            } break;
        case ::HIR::Visitor::PathContext::TRAIT:
            // TODO: treat PathContext::TRAIT differently
        case ::HIR::Visitor::PathContext::TYPE: {
            auto it = mod->m_mod_items.find( path.m_components.back() );
            if( it == mod->m_mod_items.end() ) {
                BUG(sp, "Couldn't find final component of " << path);
            }
            
            TU_MATCH( ::HIR::TypeItem, (it->second->ent), (e),
            (Import,
                BUG(sp, "Type path pointed to import - " << path);
                ),
            (TypeAlias,
                BUG(sp, "Type path pointed to type alias - " << path);
                ),
            (Module,
                BUG(sp, "Type path pointed to module - " << path);
                ),
            (Struct,
                return e.m_params;
                ),
            (Enum,
                return e.m_params;
                ),
            (Trait,
                return e.m_params;
                )
            )
            } break;
        }
        throw "";
        
    }
    
    class Visitor:
        public ::HIR::Visitor
    {
        ::HIR::Crate& crate;
        
        ::HIR::GenericParams*   m_impl_generics;
        ::HIR::GenericParams*   m_item_generics;
    public:
        Visitor(::HIR::Crate& crate):
            crate(crate),
            m_impl_generics(nullptr),
            m_item_generics(nullptr)
        {
        }
        
        void visit_generic_path(::HIR::GenericPath& p, PathContext pc) override
        {
            const auto& params = get_params_for_item(Span(), crate, p.m_path, pc);
            auto& args = p.m_params;
            
            if( args.m_types.size() == 0 && params.m_types.size() > 0 ) {
                args.m_types.resize( params.m_types.size() );
                DEBUG("- Insert inferrence");
            }
            DEBUG("p = " << p);
        }
        void visit_path(::HIR::Path& p, ::HIR::Visitor::PathContext pc) override
        {
            assert(pc == ::HIR::Visitor::PathContext::TYPE);
            TU_MATCH(::HIR::Path::Data, (p.m_data), (e),
            (Generic,
                this->visit_generic_path(e, pc);
                ),
            (UfcsKnown,
                this->visit_type(*e.type);
                this->visit_generic_path(e.trait, ::HIR::Visitor::PathContext::TYPE);
                // TODO: Locate impl block and check parameters
                ),
            (UfcsInherent,
                this->visit_type(*e.type);
                // TODO: Locate impl block and check parameters
                ),
            (UfcsUnknown,
                BUG(Span(), "Encountered unknown-trait UFCS path during outer typeck");
                )
            )
        }
        
        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            assert(m_impl_generics == nullptr);
            m_impl_generics = &impl.m_params;
            
            ::HIR::Visitor::visit_type_impl(impl);
            // Check that the type is valid
            
            m_impl_generics = nullptr;
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            assert(m_impl_generics == nullptr);
            m_impl_generics = &impl.m_params;
            
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            // Check that the type+trait is valid
            
            m_impl_generics = nullptr;
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl)
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type << " { }");
            assert(m_impl_generics == nullptr);
            m_impl_generics = &impl.m_params;
            
            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
            // Check that the type+trait is valid
            
            m_impl_generics = nullptr;
        }
    };
}


void Typecheck_ModuleLevel(::HIR::Crate& crate)
{
    Visitor v { crate };
    v.visit_crate(crate);
}

