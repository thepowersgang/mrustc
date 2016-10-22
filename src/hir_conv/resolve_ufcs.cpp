/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/resolve_ufcs.cpp
 * - Resolve unkown UFCS traits into inherent or trait
 * - HACK: Will likely be replaced with a proper typeck pass (no it won't)
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>

namespace {
    class Visitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
        
        typedef ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   t_trait_imports;
        t_trait_imports m_traits;
        
        StaticTraitResolve  m_resolve;
        const ::HIR::Trait* m_current_trait;
        const ::HIR::ItemPath* m_current_trait_path;
        
    public:
        Visitor(const ::HIR::Crate& crate):
            m_crate(crate),
            m_resolve(crate),
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
            DEBUG("");
            auto rv = ModTraitsGuard {  this, mv$(this->m_traits)  };
            for( const auto& trait_path : mod.m_traits ) {
                DEBUG("- " << trait_path);
                m_traits.push_back( ::std::make_pair( &trait_path, &this->find_trait(trait_path) ) );
            }
            return rv;
        }
        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            auto _ = this->push_mod_traits( mod );
            ::HIR::Visitor::visit_module(p, mod);
        }

        void visit_struct(::HIR::ItemPath p, ::HIR::Struct& item) override {
            auto _ = m_resolve.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_struct(p, item);
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            auto _ = m_resolve.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_enum(p, item);
        }
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            auto _ = m_resolve.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_function(p, item);
        }
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& trait) override {
            m_current_trait = &trait;
            m_current_trait_path = &p;
            //auto _ = m_resolve.set_item_generics(trait.m_params);
            auto _ = m_resolve.set_impl_generics(trait.m_params);
            ::HIR::Visitor::visit_trait(p, trait);
            m_current_trait = nullptr;
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override {
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << impl.m_type << " (mod=" << impl.m_src_module << ")");
            auto _t = this->push_mod_traits( this->m_crate.get_mod_by_path(Span(), impl.m_src_module) );
            auto _g = m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override {
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type << " (mod=" << impl.m_src_module << ")");
            auto _t = this->push_mod_traits( this->m_crate.get_mod_by_path(Span(), impl.m_src_module) );
            auto _g = m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
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
                    upper_visitor.visit_pattern(node.m_pattern);
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
                
                void visit(::HIR::ExprNode_PathValue& node) override
                {
                    upper_visitor.visit_path(node.m_path, ::HIR::Visitor::PathContext::VALUE);
                    ::HIR::ExprVisitorDef::visit(node);
                }
                
                void visit(::HIR::ExprNode_Match& node) override
                {
                    for(auto& arm : node.m_arms)
                    {
                        for(auto& pat : arm.m_patterns)
                            upper_visitor.visit_pattern(pat);
                    }
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
            //const auto& name = pd.as_UfcsUnknown().item;
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
                //trait_path_g.m_params.m_types.push_back( ::HIR::TypeRef(trait.m_params.m_types[i].m_name, i) );
                //trait_path_g.m_params.m_types.push_back( ::HIR::TypeRef() );
                trait_path_g.m_params.m_types.push_back( trait.m_params.m_types[i].m_default.clone() );
            }
            return trait_path_g;
        }
        // Locate the item in `pd` and set `pd` to UfcsResolved if found
        // TODO: This code may end up generating paths without the type information they should contain
        bool locate_in_trait_and_set(::HIR::Visitor::PathContext pc, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd) {
            // TODO: Get the span from caller
            static Span _sp;
            const auto& sp = _sp;
            if( locate_item_in_trait(pc, trait,  pd) ) {
                pd = get_ufcs_known(mv$(pd.as_UfcsUnknown()), trait_path.clone() /*make_generic_path(trait_path.m_path, trait)*/, trait);
                return true;
            }
            // Search supertraits (recursively)
            for( unsigned int i = 0; i < trait.m_parent_traits.size(); i ++ )
            {
                const auto& par_trait_path_tpl = trait.m_parent_traits[i].m_path;
                const auto* par_trait_path_ptr = &par_trait_path_tpl;
                ::HIR::GenericPath  par_trait_path_tmp;
                // HACK: Compares the param sets to avoid needing to monomorphise in some cases (e.g. Fn*
                if( monomorphise_genericpath_needed(par_trait_path_tpl) && par_trait_path_tpl.m_params != trait_path.m_params ) {
                    auto monomorph_cb = [&](const auto& ty)->const auto& {
                        const auto& ge = ty.m_data.as_Generic();
                        if( ge.binding == 0xFFFF ) {
                            TODO(sp, "Self when monomorphising trait args");
                        }
                        else if( ge.binding < 256 ) {
                            assert(ge.binding < trait_path.m_params.m_types.size());
                            return trait_path.m_params.m_types[ge.binding];
                        }
                        else {
                            ERROR(sp, E0000, "Unexpected generic binding " << ty);
                        }
                        };
                    par_trait_path_tmp = ::HIR::GenericPath(
                        par_trait_path_tpl.m_path,
                        monomorphise_path_params_with(sp, par_trait_path_tpl.m_params, monomorph_cb, false /*no infer*/)
                        );
                    par_trait_path_ptr = &par_trait_path_tmp;
                }
                const auto& par_trait_path = *par_trait_path_ptr;
                //const auto& par_trait_ent = *trait.m_parent_trait_ptrs[i];
                const auto& par_trait_ent = this->find_trait(par_trait_path.m_path);
                if( locate_in_trait_and_set(pc, par_trait_path, par_trait_ent,  pd) ) {
                    return true;
                }
            }
            return false;
        }
        
        bool locate_in_trait_impl_and_set(::HIR::Visitor::PathContext pc, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd) {
            static Span sp;
            
            auto& e = pd.as_UfcsUnknown();
            if( this->locate_item_in_trait(pc, trait,  pd) ) {
                const auto& type = *e.type;
                
                return this->m_resolve.find_impl(sp,  trait_path.m_path, nullptr, type, [&](const auto& impl){
                    pd = get_ufcs_known(mv$(e), make_generic_path(trait_path.m_path, trait), trait);
                    DEBUG("FOUND impl from " << impl);
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
            static Span sp;
            
            TU_IFLET(::HIR::Path::Data, p.m_data, UfcsUnknown, e,
                TRACE_FUNCTION_F("UfcsUnknown - p=" << p);
                
                this->visit_type( *e.type );
                this->visit_path_params( e.params );
                
                // Search for matching impls in current generic blocks
                if( m_resolve.m_item_generics != nullptr && locate_trait_item_in_bounds(pc, *e.type, *m_resolve.m_item_generics,  p.m_data) ) {
                    DEBUG("Found in item params, p = " << p);
                    return ;
                }
                if( m_resolve.m_impl_generics != nullptr && locate_trait_item_in_bounds(pc, *e.type, *m_resolve.m_impl_generics,  p.m_data) ) {
                    DEBUG("Found in impl params, p = " << p);
                    return ;
                }
                
                TU_IFLET(::HIR::TypeRef::Data, e.type->m_data, Generic, te,
                    // If processing a trait, and the type is 'Self', search for the type/method on the trait
                    // - TODO: This could be encoded by a `Self: Trait` bound in the generics, but that may have knock-on issues?
                    if( te.name == "Self" && m_current_trait ) {
                        auto trait_path = ::HIR::GenericPath( m_current_trait_path->get_simple_path() );
                        for(unsigned int i = 0; i < m_current_trait->m_params.m_types.size(); i ++ ) {
                            trait_path.m_params.m_types.push_back( ::HIR::TypeRef(m_current_trait->m_params.m_types[i].m_name, i) );
                        }
                        if( locate_in_trait_and_set(pc, trait_path, *m_current_trait,  p.m_data) ) {
                            // Success!
                            DEBUG("Found in Self, p = " << p);
                            return ;
                        }
                    }
                    ERROR(sp, E0000, "Failed to find bound with '" << e.item << "' for " << *e.type);
                    return ;
                )
                else {
                    // 1. Search for applicable inherent methods (COMES FIRST!)
                    if( m_crate.find_type_impls(*e.type, [&](const auto& t)->const auto& { return t; }, [&](const auto& impl) {
                        DEBUG("- matched inherent impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                        // Search for item in this block
                        switch( pc )
                        {
                        case ::HIR::Visitor::PathContext::VALUE:
                            if( impl.m_methods.find(e.item) != impl.m_methods.end() ) {
                            }
                            else if( impl.m_constants.find(e.item) != impl.m_constants.end() ) {
                            }
                            else {
                                return false;
                            }
                            // Found it, just keep going (don't care about details here)
                            break;
                        case ::HIR::Visitor::PathContext::TRAIT:
                        case ::HIR::Visitor::PathContext::TYPE:
                            return false;
                        }
                        
                        auto new_data = ::HIR::Path::Data::make_UfcsInherent({ mv$(e.type), mv$(e.item), mv$(e.params)} );
                        p.m_data = mv$(new_data);
                        DEBUG("- Resolved, replace with " << p);
                        return true;
                        }) ) {
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
            )
            else {
                ::HIR::Visitor::visit_path(p, pc);
            }
        }
        
        void visit_pattern(::HIR::Pattern& pat) override
        {
            static Span _sp = Span();
            const Span& sp = _sp;

            ::HIR::Visitor::visit_pattern(pat);
            
            TU_MATCH_DEF(::HIR::Pattern::Data, (pat.m_data), (e),
            (
                ),
            (Value,
                this->visit_pattern_Value(sp, pat, e.val);
                ),
            (Range,
                this->visit_pattern_Value(sp, pat, e.start);
                this->visit_pattern_Value(sp, pat, e.end);
                )
            )
        }
        void visit_pattern_Value(const Span& sp, const ::HIR::Pattern& pat, ::HIR::Pattern::Value& val)
        {
            TU_IFLET( ::HIR::Pattern::Value, val, Named, ve,
                TRACE_FUNCTION_F(ve.path);
                TU_MATCH( ::HIR::Path::Data, (ve.path.m_data), (pe),
                (Generic,
                    // Already done
                    ),
                (UfcsUnknown,
                    BUG(sp, "UfcsUnknown still in pattern value - " << pat);
                    ),
                (UfcsInherent,
                    bool rv = m_crate.find_type_impls(*pe.type, [&](const auto& t)->const auto& { return t; }, [&](const auto& impl) {
                        DEBUG("- matched inherent impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                        // Search for item in this block
                        auto it = impl.m_constants.find(pe.item);
                        if( it != impl.m_constants.end() ) {
                            ve.binding = &it->second.data;
                            return true;
                        }
                        return false;
                        });
                    if( !rv ) {
                        ERROR(sp, E0000, "Constant " << ve.path << " couldn't be found");
                    }
                    ),
                (UfcsKnown,
                    bool rv = this->m_resolve.find_impl(sp,  pe.trait.m_path, &pe.trait.m_params, *pe.type, [&](const auto& impl) {
                        if( !impl.m_data.is_TraitImpl() ) {
                            return true;
                        }
                        ve.binding = &impl.m_data.as_TraitImpl().impl->m_constants.at( pe.item ).data;
                        return true;
                        });
                    if( !rv ) {
                        ERROR(sp, E0000, "Constant " << ve.path << " couldn't be found");
                    }
                    )
                )
            )
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
