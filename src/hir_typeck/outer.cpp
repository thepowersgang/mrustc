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
        //::HIR::GenericParams*   m_item_generics;
        ::std::vector< ::HIR::TypeRef* >    m_self_types;
    public:
        Visitor(::HIR::Crate& crate):
            crate(crate),
            m_impl_generics(nullptr)/*,
            m_item_generics(nullptr)*/
        {
        }
        
    private:
        void update_self_type(const Span& sp, ::HIR::TypeRef& ty)
        {
            TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
            (Generic,
                if(e.name == "Self") {
                    if( m_self_types.empty() )
                        ERROR(sp, E0000, "Self appeared in unexpected location");
                    if( !m_self_types.back() )
                        ERROR(sp, E0000, "Self appeared in unexpected location");
                    ty = m_self_types.back()->clone();
                    return;
                }
                ),
            
            (Infer,
                ),
            (Diverge,
                ),
            (Primitive,
                ),
            (Path,
                TODO(sp, "update_self_type - Path");
                ),
            (TraitObject,
                // NOTE: Can't mention Self anywhere
                TODO(sp, "update_self_type - TraitObject");
                ),
            (Tuple,
                for(auto& sty : e)
                    update_self_type(sp, sty);
                ),
            (Array,
                update_self_type(sp, *e.inner);
                ),
            (Slice,
                update_self_type(sp, *e.inner);
                ),
            (Borrow,
                update_self_type(sp, *e.inner);
                ),
            (Pointer,
                update_self_type(sp, *e.inner);
                ),
            (Function,
                TODO(sp, "update_self_type - Function");
                )
            )
        }
        void check_parameters(const Span& sp, const ::HIR::GenericParams& param_def,  ::HIR::PathParams& param_vals)
        {
            while( param_vals.m_types.size() < param_def.m_types.size() ) {
                unsigned int i = param_vals.m_types.size(); 
                if( param_def.m_types[i].m_default.m_data.is_Infer() ) {
                    ERROR(sp, E0000, "Unspecified parameter with no default");
                }
                
                // Replace and expand
                param_vals.m_types.push_back( param_def.m_types[i].m_default.clone() );
                auto& ty = param_vals.m_types.back();
                update_self_type(sp, ty);
            }
            
            if( param_vals.m_types.size() != param_def.m_types.size() ) {
                ERROR(sp, E0000, "Incorrect number of parameters - expected " << param_def.m_types.size() << ", got " << param_vals.m_types.size());
            }
            
            // TODO: Check generic bounds
            for( const auto& bound : param_def.m_bounds )
            {
                TU_MATCH(::HIR::GenericBound, (bound), (e),
                (Lifetime,
                    ),
                (TypeLifetime,
                    ),
                (TraitBound,
                    // TODO: Check for an implementation of this trait
                    DEBUG("TODO: Check bound " << e.type << " : " << e.trait.m_path);
                    ),
                (TypeEquality,
                    // TODO: Check that two types are equal in this case
                    TODO(sp, "TypeEquality - " << e.type << " == " << e.other_type);
                    )
                )
            }
        }
        
    public:
        void visit_generic_path(::HIR::GenericPath& p, PathContext pc) override
        {
            TRACE_FUNCTION_F("p = " << p);
            const auto& params = get_params_for_item(Span(), crate, p.m_path, pc);
            auto& args = p.m_params;
            
            check_parameters(Span(), params, args);
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
                m_self_types.push_back(&*e.type);
                this->visit_generic_path(e.trait, ::HIR::Visitor::PathContext::TYPE);
                m_self_types.pop_back();
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
        
        void visit_params(::HIR::GenericParams& params) override
        {
            for(auto& tps : params.m_types)
                this->visit_type( tps.m_default );
            
            for(auto& bound : params.m_bounds )
            {
                TU_MATCH(::HIR::GenericBound, (bound), (e),
                (Lifetime,
                    ),
                (TypeLifetime,
                    this->visit_type(e.type);
                    ),
                (TraitBound,
                    this->visit_type(e.type);
                    m_self_types.push_back(&e.type);
                    this->visit_generic_path(e.trait.m_path, ::HIR::Visitor::PathContext::TYPE);
                    m_self_types.pop_back();
                    ),
                //(NotTrait, struct {
                //    ::HIR::TypeRef  type;
                //    ::HIR::GenricPath    trait;
                //    }),
                (TypeEquality,
                    this->visit_type(e.type);
                    this->visit_type(e.other_type);
                    )
                )
            }
        }
        
        void visit_trait(::HIR::PathChain p, ::HIR::Trait& item) override
        {
            ::HIR::TypeRef tr { "Self", 0 };
            m_self_types.push_back(&tr);
            ::HIR::Visitor::visit_trait(p, item);
            m_self_types.pop_back();
        }
        
        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            assert(m_impl_generics == nullptr);
            m_impl_generics = &impl.m_params;
            m_self_types.push_back( &impl.m_type );
            
            ::HIR::Visitor::visit_type_impl(impl);
            // Check that the type is valid
            
            m_self_types.pop_back();
            m_impl_generics = nullptr;
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            assert(m_impl_generics == nullptr);
            m_impl_generics = &impl.m_params;
            m_self_types.push_back( &impl.m_type );
            
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            // Check that the type+trait is valid
            
            m_self_types.pop_back();
            m_impl_generics = nullptr;
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl)
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type << " { }");
            assert(m_impl_generics == nullptr);
            m_impl_generics = &impl.m_params;
            m_self_types.push_back( &impl.m_type );
            
            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
            // Check that the type+trait is valid
            
            m_self_types.pop_back();
            m_impl_generics = nullptr;
        }
    };
}


void Typecheck_ModuleLevel(::HIR::Crate& crate)
{
    Visitor v { crate };
    v.visit_crate(crate);
}

