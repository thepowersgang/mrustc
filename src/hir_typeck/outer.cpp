/*
 * Check types (and evaluate constants) at the module level
 */
#include <hir/hir.hpp>
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>

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
        StaticTraitResolve  m_resolve;
        
        const ::HIR::Trait* m_current_trait;
        const ::HIR::PathChain* m_current_trait_path;
    
        ::std::vector< ::HIR::TypeRef* >    m_self_types;
        
        typedef ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   t_trait_imports;
        t_trait_imports m_traits;
    public:
        Visitor(::HIR::Crate& crate):
            crate(crate),
            m_resolve(crate),
            m_current_trait(nullptr)
        {
        }
        
    private:
        struct ModTraitsGuard {
            Visitor* v;
            t_trait_imports old_imports;
            
            ~ModTraitsGuard() {
                this->v->m_traits = mv$(this->old_imports);
            }
        };
        ModTraitsGuard push_mod_traits(const ::HIR::Module& mod) {
            static Span sp;
            DEBUG("");
            auto rv = ModTraitsGuard {  this, mv$(this->m_traits)  };
            for( const auto& trait_path : mod.m_traits ) {
                DEBUG("- " << trait_path);
                m_traits.push_back( ::std::make_pair( &trait_path, &this->crate.get_trait_by_path(sp, trait_path) ) );
            }
            return rv;
        }
        
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
                ),
            (Closure,
                TODO(sp, "update_self_type - Closure");
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
                    DEBUG("TODO: Check equality bound " << e.type << " == " << e.other_type);
                    )
                )
            }
        }
        
    public:
        void visit_type(::HIR::TypeRef& ty) override
        {
            static Span _sp;
            const Span& sp = _sp;
            ::HIR::Visitor::visit_type(ty);
            
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Path, e,
                TU_MATCH( ::HIR::Path::Data, (e.path.m_data), (pe),
                (Generic,
                    ),
                (UfcsUnknown,
                    TODO(sp, "Should UfcsKnown be encountered here?");
                    ),
                (UfcsInherent,
                    TODO(sp, "Locate impl block for UFCS Inherent");
                    ),
                (UfcsKnown,
                    if( pe.type->m_data.is_Path() && pe.type->m_data.as_Path().binding.is_Opaque() ) {
                        // - Opaque type, opaque result
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                    }
                    else if( pe.type->m_data.is_Generic() ) {
                        // - Generic type, opaque resut. (TODO: Sometimes these are known - via generic bounds)
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                    }
                    else {
                        StaticTraitResolve::ImplRef best_impl;
                        m_resolve.find_impl(sp, pe.trait.m_path, pe.trait.m_params, *pe.type, [&](auto impl) {
                            DEBUG("[visit_type] Found " << impl);
                            if( best_impl.more_specific_than(impl) )
                                return false;
                            best_impl = mv$(impl);
                            if( ! best_impl.type_is_specializable(pe.item.c_str()) )
                                return true;
                            return false;
                            });
                        if( best_impl.is_valid() ) {
                            // If the type is still specialisable, and there's geerics in the type.
                            if( best_impl.type_is_specializable(pe.item.c_str()) && pe.type->contains_generics() ) {
                                // Mark it as opaque (because monomorphisation could change things)
                                e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                            }
                            else {
                                auto new_ty = best_impl.get_type(pe.item.c_str());
                                if( new_ty == ::HIR::TypeRef() ) {
                                    ERROR(sp, E0000, "Associated type '"<<pe.item<<"' could not be found in " << pe.trait);
                                }
                                DEBUG("Replaced " << ty << " with " << new_ty);
                                ty = mv$(new_ty);
                            }
                        }
                        else {
                            ERROR(sp, E0000, "Couldn't find an impl of " << pe.trait << " for " << *pe.type);
                        }
                    }
                    )
                )
            )
        }
        
        void visit_generic_path(::HIR::GenericPath& p, PathContext pc) override
        {
            TRACE_FUNCTION_F("p = " << p);
            const auto& params = get_params_for_item(Span(), crate, p.m_path, pc);
            auto& args = p.m_params;
            
            check_parameters(Span(), params, args);
            DEBUG("p = " << p);
            
            ::HIR::Visitor::visit_generic_path(p, pc);
        }
        
    private:
        bool locate_trait_item_in_bounds(const Span& sp, ::HIR::Visitor::PathContext pc,  const ::HIR::TypeRef& tr, const ::HIR::GenericParams& params,  ::HIR::Path::Data& pd) {
            //const auto& name = pd.as_UfcsUnknown().item;
            for(const auto& b : params.m_bounds)
            {
                TU_IFLET(::HIR::GenericBound, b, TraitBound, e,
                    DEBUG("- " << e.type << " : " << e.trait.m_path);
                    if( e.type == tr ) {
                        DEBUG(" - Match");
                        if( locate_in_trait_and_set(sp, pc, e.trait.m_path, this->crate.get_trait_by_path(sp, e.trait.m_path.m_path),  pd) ) {
                            return true;
                        }
                    }
                );
                // - 
            }
            return false;
        }
        static ::HIR::Path::Data get_ufcs_known(::HIR::Path::Data::Data_UfcsUnknown e,  ::HIR::GenericPath trait_path, const ::HIR::Trait& trait)
        {
            return ::HIR::Path::Data::make_UfcsKnown({ mv$(e.type), mv$(trait_path), mv$(e.item), mv$(e.params)} );
        }
        static bool locate_item_in_trait(::HIR::Visitor::PathContext pc, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd)
        {
            const auto& e = pd.as_UfcsUnknown();
            
            switch(pc)
            {
            case ::HIR::Visitor::PathContext::VALUE:
                if( trait.m_values.find( e.item ) != trait.m_values.end() ) {
                    return true;
                }
                break;
            case ::HIR::Visitor::PathContext::TRAIT:
                break;
            case ::HIR::Visitor::PathContext::TYPE:
                if( trait.m_types.find( e.item ) != trait.m_types.end() ) {
                    return true;
                }
                break;
            }
            return false;
        }
        bool locate_in_trait_and_set(const Span& sp, ::HIR::Visitor::PathContext pc, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd) {
            if( locate_item_in_trait(pc, trait,  pd) ) {
                pd = get_ufcs_known(mv$(pd.as_UfcsUnknown()), make_generic_path(trait_path.m_path, trait), trait);
                return true;
            }
            // Search supertraits (recursively)
            for( unsigned int i = 0; i < trait.m_parent_traits.size(); i ++ )
            {
                const auto& par_trait_path = trait.m_parent_traits[i].m_path;
                //const auto& par_trait_ent = *trait.m_parent_trait_ptrs[i];
                const auto& par_trait_ent = this->crate.get_trait_by_path(sp, par_trait_path.m_path);
                if( locate_in_trait_and_set(sp, pc, par_trait_path, par_trait_ent,  pd) ) {
                    return true;
                }
            }
            return false;
        }
        bool locate_in_trait_impl_and_set(::HIR::Visitor::PathContext pc, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd) {
            static Span sp;
            
            auto& e = pd.as_UfcsUnknown();
            //if( this->m_resolve.trait_contains_type(sp, trait_path, trait, e.item,  out_path) )
            if( this->locate_item_in_trait(pc, trait,  pd) ) {
                const auto& type = *e.type;
                
                return this->crate.find_trait_impls(trait_path.m_path, type, [](const auto& x)->const auto&{return x;}, [&](const auto& impl) {
                    DEBUG("FOUND impl" << impl.m_params.fmt_args() << " " << trait_path.m_path << impl.m_trait_args << " for " << impl.m_type);
                    // TODO: Check bounds
                    for(const auto& bound : impl.m_params.m_bounds) {
                        DEBUG("- TODO: Bound " << bound);
                        return false;
                    }
                    pd = get_ufcs_known(mv$(e), make_generic_path(trait_path.m_path, trait), trait);
                    return true;
                    });
            }
            else {
                DEBUG("- Item " << e.item << " not in trait " << trait_path.m_path);
            }
            
            
            // Search supertraits (recursively)
            for( unsigned int i = 0; i < trait.m_parent_traits.size(); i ++ )
            {
                const auto& par_trait_path = trait.m_parent_traits[i].m_path;
                //const auto& par_trait_ent = *trait.m_parent_trait_ptrs[i];
                const auto& par_trait_ent = this->crate.get_trait_by_path(sp, par_trait_path.m_path);
                // TODO: Modify path parameters based on the current trait's params
                if( locate_in_trait_impl_and_set(pc, par_trait_path, par_trait_ent,  pd) ) {
                    return true;
                }
            }
            return false;
        }
        static ::HIR::GenericPath make_generic_path(::HIR::SimplePath sp, const ::HIR::Trait& trait)
        {
            auto trait_path_g = ::HIR::GenericPath( mv$(sp) );
            for(unsigned int i = 0; i < trait.m_params.m_types.size(); i ++ ) {
                trait_path_g.m_params.m_types.push_back( ::HIR::TypeRef(trait.m_params.m_types[i].m_name, i) );
            }
            return trait_path_g;
        }
        void visit_path_UfcsUnknown(const Span& sp, ::HIR::Path& p, ::HIR::Visitor::PathContext pc)
        {
            TRACE_FUNCTION_F("UfcsUnknown - p=" << p);
            auto& e = p.m_data.as_UfcsUnknown();
            
            this->visit_type( *e.type );
            this->visit_path_params( e.params );
            
            // Search for matching impls in current generic blocks
            if( m_resolve.m_item_generics != nullptr && locate_trait_item_in_bounds(sp, pc, *e.type, *m_resolve.m_item_generics,  p.m_data) ) {
                return ;
            }
            if( m_resolve.m_impl_generics != nullptr && locate_trait_item_in_bounds(sp, pc, *e.type, *m_resolve.m_impl_generics,  p.m_data) ) {
                return ;
            }
            
            TU_IFLET(::HIR::TypeRef::Data, e.type->m_data, Generic, te,
                // If processing a trait, and the type is 'Self', search for the type/method on the trait
                // - TODO: This could be encoded by a `Self: Trait` bound in the generics, but that may have knock-on issues?
                if( te.name == "Self" && m_current_trait ) {
                    auto trait_path = ::HIR::GenericPath( m_current_trait_path->to_path() );
                    for(unsigned int i = 0; i < m_current_trait->m_params.m_types.size(); i ++ ) {
                        trait_path.m_params.m_types.push_back( ::HIR::TypeRef(m_current_trait->m_params.m_types[i].m_name, i) );
                    }
                    if( this->locate_in_trait_and_set(sp, pc, trait_path, *m_current_trait,  p.m_data) ) {
                        // Success!
                        return ;
                    }
                }
                ERROR(sp, E0000, "Failed to find impl with '" << e.item << "' for " << *e.type);
                return ;
            )
            else {
                // 1. Search for applicable inherent methods (COMES FIRST!)
                for( const auto& impl : this->crate.m_type_impls )
                {
                    if( !impl.matches_type(*e.type) ) {
                        continue ;
                    }
                    DEBUG("- matched inherent impl " << *e.type);
                    // Search for item in this block
                    switch( pc )
                    {
                    case ::HIR::Visitor::PathContext::VALUE:
                        if( impl.m_methods.find(e.item) == impl.m_methods.end() ) {
                            continue ;
                        }
                        // Found it, just keep going (don't care about details here)
                        break;
                    case ::HIR::Visitor::PathContext::TRAIT:
                    case ::HIR::Visitor::PathContext::TYPE:
                        continue ;
                    }
                    
                    auto new_data = ::HIR::Path::Data::make_UfcsInherent({ mv$(e.type), mv$(e.item), mv$(e.params)} );
                    p.m_data = mv$(new_data);
                    DEBUG("- Resolved, replace with " << p);
                    return ;
                }
                // 2. Search all impls of in-scope traits for this method on this type
                for( const auto& trait_info : m_traits )
                {
                    const auto& trait = *trait_info.second;
                    
                    switch(pc)
                    {
                    case ::HIR::Visitor::PathContext::VALUE:
                        if( trait.m_values.find(e.item) == trait.m_values.end() )
                            continue ;
                        break;
                    case ::HIR::Visitor::PathContext::TRAIT:
                    case ::HIR::Visitor::PathContext::TYPE:
                        if( trait.m_types.find(e.item) == trait.m_types.end() )
                            continue ;
                        break;
                    }
                    DEBUG("- Trying trait " << *trait_info.first);
                    
                    auto trait_path = ::HIR::GenericPath( *trait_info.first );
                    for(unsigned int i = 0; i < trait.m_params.m_types.size(); i ++ ) {
                        trait_path.m_params.m_types.push_back( ::HIR::TypeRef() );
                    }
                    
                    // TODO: Search supertraits
                    // TODO: Should impls be searched first, or item names?
                    // - Item names add complexity, but impls are slower
                    if( this->locate_in_trait_impl_and_set(pc, mv$(trait_path), trait,  p.m_data) ) {
                        return ;
                    }
                }
            }
            
            // Couldn't find it
            ERROR(sp, E0000, "Failed to find impl with '" << e.item << "' for " << *e.type << " (in " << p << ")");
        }
    
    public:
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
                BUG(Span(), "Encountered unknown-trait UFCS path during outer typeck - " << p);
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
        
        void visit_module(::HIR::PathChain p, ::HIR::Module& mod) override
        {
            auto _ = this->push_mod_traits( mod );
            ::HIR::Visitor::visit_module(p, mod);
        }
        
        void visit_trait(::HIR::PathChain p, ::HIR::Trait& item) override
        {
            m_current_trait = &item;
            m_current_trait_path = &p;
            
            auto _ = m_resolve.set_item_generics(item.m_params);
            ::HIR::TypeRef tr { "Self", 0xFFFF };
            m_self_types.push_back(&tr);
            ::HIR::Visitor::visit_trait(p, item);
            m_self_types.pop_back();
            
            m_current_trait = nullptr;
        }
        void visit_struct(::HIR::PathChain p, ::HIR::Struct& item) override
        {
            auto _ = m_resolve.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_struct(p, item);
        }
        
        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = m_resolve.set_impl_generics(impl.m_params);
            m_self_types.push_back( &impl.m_type );
            
            ::HIR::Visitor::visit_type_impl(impl);
            // Check that the type is valid
            
            m_self_types.pop_back();
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            auto _ = m_resolve.set_impl_generics(impl.m_params);
            m_self_types.push_back( &impl.m_type );
            
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            // Check that the type+trait is valid
            
            m_self_types.pop_back();
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl)
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type << " { }");
            auto _ = m_resolve.set_impl_generics(impl.m_params);
            m_self_types.push_back( &impl.m_type );
            
            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
            // Check that the type+trait is valid
            
            m_self_types.pop_back();
        }
    };
}


void Typecheck_ModuleLevel(::HIR::Crate& crate)
{
    Visitor v { crate };
    v.visit_crate(crate);
}

