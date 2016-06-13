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
        
        typedef ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   t_trait_imports;
        t_trait_imports m_traits;
        
        const ::HIR::GenericParams* m_impl_params;
        const ::HIR::GenericParams* m_item_params;
        const ::HIR::Trait* m_current_trait;
        const ::HIR::PathChain* m_current_trait_path;
        
    public:
        Visitor(const ::HIR::Crate& crate):
            m_crate(crate),
            m_impl_params(nullptr),
            m_item_params(nullptr),
            m_current_trait(nullptr)
        {}
        
        struct ModTraitsGuard {
            Visitor* v;
            t_trait_imports old_imports;
            
            ~ModTraitsGuard() {
                this->v->m_traits = mv$(this->old_imports);
            }
        };
        ModTraitsGuard push_mod_traits(const ::HIR::Module& mod) {
            auto rv = ModTraitsGuard {  this, mv$(this->m_traits)  };
            for( const auto& trait_path : mod.m_traits )
                m_traits.push_back( ::std::make_pair( &trait_path, &this->find_trait(trait_path) ) );
            return rv;
        }
        void visit_module(::HIR::PathChain p, ::HIR::Module& mod) override
        {
            auto _ = this->push_mod_traits( mod );
            ::HIR::Visitor::visit_module(p, mod);
        }

        void visit_struct(::HIR::PathChain p, ::HIR::Struct& fcn) override {
            m_item_params = &fcn.m_params;
            ::HIR::Visitor::visit_struct(p, fcn);
            m_item_params = nullptr;
        }
        void visit_enum(::HIR::PathChain p, ::HIR::Enum& fcn) override {
            m_item_params = &fcn.m_params;
            ::HIR::Visitor::visit_enum(p, fcn);
            m_item_params = nullptr;
        }
        void visit_function(::HIR::PathChain p, ::HIR::Function& fcn) override {
            m_item_params = &fcn.m_params;
            ::HIR::Visitor::visit_function(p, fcn);
            m_item_params = nullptr;
        }
        void visit_trait(::HIR::PathChain p, ::HIR::Trait& trait) override {
            m_current_trait = &trait;
            m_current_trait_path = &p;
            m_impl_params = &trait.m_params;
            ::HIR::Visitor::visit_trait(p, trait);
            m_impl_params = nullptr;
            m_current_trait = nullptr;
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override {
            auto _ = this->push_mod_traits( this->m_crate.get_mod_by_path(Span(), impl.m_src_module) );
            m_impl_params = &impl.m_params;
            ::HIR::Visitor::visit_type_impl(impl);
            m_impl_params = nullptr;
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) {
            auto _ = this->push_mod_traits( this->m_crate.get_mod_by_path(Span(), impl.m_src_module) );
            m_impl_params = &impl.m_params;
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            m_impl_params = nullptr;
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
                    if( node.m_traits.size() == 0 && node.m_local_mod.m_components.size() > 0 ) {
                        const auto& mod = upper_visitor.m_crate.get_mod_by_path(node.span(), node.m_local_mod);
                        for( const auto& trait_path : mod.m_traits ) {
                            node.m_traits.push_back( ::std::make_pair( &trait_path, &upper_visitor.m_crate.get_trait_by_path(node.span(), trait_path) ) );
                        }
                    }
                    for( const auto& trait_ref : node.m_traits )
                        upper_visitor.m_traits.push_back( trait_ref );
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

        bool locate_trait_item_in_bounds(::HIR::Visitor::PathContext pc,  const ::HIR::TypeRef& tr, const ::HIR::GenericParams& params,  ::HIR::Path::Data& pd) {
            const auto& name = pd.as_UfcsUnknown().item;
            DEBUG("TODO: Search for trait impl for " << tr << " with " << name << " in params");
            for(const auto& b : params.m_bounds)
            {
                TU_IFLET(::HIR::GenericBound, b, TraitBound, e,
                    DEBUG("- " << e.type << " : " << e.trait.m_path);
                    if( e.type == tr ) {
                        DEBUG(" - Match");
                        if( locate_in_trait_and_set(pc, e.trait.m_path, this->find_trait(e.trait.m_path.m_path),  pd) ) {
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
        static ::HIR::GenericPath make_generic_path(::HIR::SimplePath sp, const ::HIR::Trait& trait)
        {
            auto trait_path_g = ::HIR::GenericPath( mv$(sp) );
            for(unsigned int i = 0; i < trait.m_params.m_types.size(); i ++ ) {
                trait_path_g.m_params.m_types.push_back( ::HIR::TypeRef(trait.m_params.m_types[i].m_name, i) );
            }
            return trait_path_g;
        }
        // Locate the item in `pd` and set `pd` to UfcsResolved if found
        // TODO: This code may end up generating paths without the type information they should contain
        bool locate_in_trait_and_set(::HIR::Visitor::PathContext pc, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd) {
            if( locate_item_in_trait(pc, trait,  pd) ) {
                pd = get_ufcs_known(mv$(pd.as_UfcsUnknown()), make_generic_path(trait_path.m_path, trait), trait);
                return true;
            }
            // Search supertraits (recursively)
            for( unsigned int i = 0; i < trait.m_parent_traits.size(); i ++ )
            {
                const auto& par_trait_path = trait.m_parent_traits[i].m_path;
                //const auto& par_trait_ent = *trait.m_parent_trait_ptrs[i];
                const auto& par_trait_ent = this->find_trait(par_trait_path.m_path);
                if( locate_in_trait_and_set(pc, par_trait_path, par_trait_ent,  pd) ) {
                    return true;
                }
            }
            return false;
        }
        
        bool locate_in_trait_impl_and_set(::HIR::Visitor::PathContext pc, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd) {
            auto& e = pd.as_UfcsUnknown();
            if( this->locate_item_in_trait(pc, trait,  pd) ) {
                const auto& type = *e.type;
                
                auto trait_impl_it = this->m_crate.m_trait_impls.equal_range( trait_path.m_path );
                if( trait_impl_it.first == trait_impl_it.second ) {
                    // Since this trait isn't implemented, none of the supertraits matter
                    return false;
                }
                for( auto it = trait_impl_it.first; it != trait_impl_it.second; ++ it )
                {
                    const auto& impl = it->second;
                    DEBUG("impl" << impl.m_params.fmt_args() << " " << trait_path.m_path << impl.m_trait_args << " for " << impl.m_type);
                    if( impl.matches_type(type) )
                    {
                        pd = get_ufcs_known(mv$(e), make_generic_path(trait_path.m_path, trait), trait);
                        return true;
                    }
                }
            }
            else {
                DEBUG("- Item " << e.item << " not in trait " << trait_path.m_path);
            }
            
            
            // Search supertraits (recursively)
            for( unsigned int i = 0; i < trait.m_parent_traits.size(); i ++ )
            {
                const auto& par_trait_path = trait.m_parent_traits[i].m_path;
                //const auto& par_trait_ent = *trait.m_parent_trait_ptrs[i];
                const auto& par_trait_ent = this->find_trait(par_trait_path.m_path);
                // TODO: Modify path parameters based on the current trait's params
                if( locate_in_trait_impl_and_set(pc, par_trait_path, par_trait_ent,  pd) ) {
                    return true;
                }
            }
            return false;
        }

        void visit_path(::HIR::Path& p, ::HIR::Visitor::PathContext pc) override
        {
            auto sp = Span();
            
            DEBUG("p = " << p);
            TU_IFLET(::HIR::Path::Data, p.m_data, UfcsUnknown, e,
                DEBUG("UfcsUnknown - p=" << p);
                
                this->visit_type( *e.type );
                this->visit_path_params( e.params );
                
                // Search for matching impls in current generic blocks
                if( m_item_params != nullptr && locate_trait_item_in_bounds(pc, *e.type, *m_item_params,  p.m_data) ) {
                    return ;
                }
                if( m_impl_params != nullptr && locate_trait_item_in_bounds(pc, *e.type, *m_impl_params,  p.m_data) ) {
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
                        if( locate_in_trait_and_set(pc, trait_path, *m_current_trait,  p.m_data) ) {
                            // Success!
                            return ;
                        }
                    }
                    ERROR(sp, E0000, "Failed to find impl with '" << e.item << "' for " << *e.type);
                    return ;
                )
                else {
                    // 1. Search for applicable inherent methods (COMES FIRST!)
                    for( const auto& impl : m_crate.m_type_impls )
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
                        DEBUG("- Looking for impl of " << *trait_info.first << " for " << *e.type);
                        
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
            )
            else {
                ::HIR::Visitor::visit_path(p, pc);
            }
        }
        
        
        const ::HIR::Trait& find_trait(const ::HIR::SimplePath& path) const
        {
            return m_crate.get_trait_by_path(Span(), path);
        }
    };

}

void ConvertHIR_ResolveUFCS(::HIR::Crate& crate)
{
    Visitor exp { crate };
    exp.visit_crate( crate );
}
