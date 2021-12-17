/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/resolve_ufcs.cpp
 * - Resolve unkown UFCS traits into inherent or trait
 * - HACK: Will likely be replaced with a proper typeck pass (no it won't)
 *
 * TODO: Remove this pass, except maybe for running EAT on outer types
 * - Expression code can handle picking UFCS functions better than this code can
 * - Outer EAT is nice, but StaticTraitResolve will need to handle non-EAT-ed types when doing lookups
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>
#include <algorithm>    // std::remove_if

namespace resolve_ufcs {
    class Visitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
        bool m_visit_exprs;
        bool m_run_eat;

        typedef ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   t_trait_imports;
        t_trait_imports m_traits;

        StaticTraitResolve  m_resolve;
        bool m_in_trait_def = false;
        const ::HIR::TypeRef* m_current_type = nullptr;
        const ::HIR::Trait* m_current_trait = nullptr;
        const ::HIR::ItemPath* m_current_trait_path = nullptr;
        bool m_in_expr = false;

    public:
        Visitor(const ::HIR::Crate& crate, bool visit_exprs):
            m_crate(crate),
            m_visit_exprs(visit_exprs),
            m_run_eat(visit_exprs), // Defaults to running when doing second-pass
            m_resolve(crate)
        {}

        struct ModTraitsGuard {
            Visitor* v;
            t_trait_imports old_imports;

            ModTraitsGuard(Visitor& v, t_trait_imports old_imports): v(&v), old_imports(mv$(old_imports)) {}
            ModTraitsGuard(ModTraitsGuard&& x): v(x.v), old_imports(mv$(x.old_imports)) { x.v = nullptr; }
            ModTraitsGuard& operator=(ModTraitsGuard&&) = delete;
            ~ModTraitsGuard() {
                if(v) {
                    DEBUG("Stack pop: " << this->v->m_traits.size() << " -> " << this->old_imports.size());
                    this->v->m_traits = mv$(this->old_imports);
                    v = nullptr;
                }
            }
        };
        ModTraitsGuard push_mod_traits(const ::HIR::Module& mod) {
            static Span sp;
            DEBUG("");
            ModTraitsGuard rv { *this, mv$(this->m_traits)  };
            for( const auto& trait_path : mod.m_traits ) {
                DEBUG("- " << trait_path);
                m_traits.push_back( ::std::make_pair( &trait_path, &m_crate.get_trait_by_path(sp, trait_path) ) );
            }
            return rv;
        }
        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            auto _ = this->push_mod_traits( mod );
            ::HIR::Visitor::visit_module(p, mod);
        }

        void visit_params(::HIR::GenericParams& params)
        {
            TRACE_FUNCTION_F(params.fmt_args() << params.fmt_bounds());

            // Custom visitor to prevent running of EAT on type paramerter defaults
            auto saved_run_eat = m_run_eat;
            m_run_eat = false;
            for(auto& tps : params.m_types) {
                this->visit_type( tps.m_default );
            }
            m_run_eat = saved_run_eat;

            for(auto& bound : params.m_bounds )
                visit_generic_bound(bound);
        }

        void visit_union(::HIR::ItemPath p, ::HIR::Union& item) override {
            auto _ = m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_union(p, item);
        }
        void visit_struct(::HIR::ItemPath p, ::HIR::Struct& item) override {
            auto _ = m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_struct(p, item);
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            auto _ = m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_enum(p, item);
        }
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            auto _ = m_resolve.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_function(p, item);
        }
        void visit_type_alias(::HIR::ItemPath p, ::HIR::TypeAlias& item) override {
            // NOTE: Disabled, because generics in type aliases are never checked
#if 0
            auto _ = m_resolve.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_type_alias(p, item);
#endif
        }
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& trait) override {
            //TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << impl.m_type << " (mod=" << impl.m_src_module << ")");
            m_in_trait_def = true;
            m_current_trait = &trait;
            m_current_trait_path = &p;
            //auto _ = m_resolve.set_cur_trait(p, trait);
            auto _ = m_resolve.set_impl_generics(trait.m_params);
            ::HIR::Visitor::visit_trait(p, trait);
            m_current_trait = nullptr;
            m_in_trait_def = false;
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override {
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << impl.m_type << " (mod=" << impl.m_src_module << ")");
            auto _t = this->push_mod_traits( this->m_crate.get_mod_by_path(Span(), impl.m_src_module) );
            auto _g = m_resolve.set_impl_generics(impl.m_params);
            m_current_type = &impl.m_type;
            ::HIR::Visitor::visit_type_impl(impl);
            m_current_type = nullptr;
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override {
            ::HIR::ItemPath    p( impl.m_type, trait_path, impl.m_trait_args );
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type << " (mod=" << impl.m_src_module << ")");
            auto _t = this->push_mod_traits( this->m_crate.get_mod_by_path(Span(), impl.m_src_module) );
            auto _g = m_resolve.set_impl_generics(impl.m_params);

            // TODO: Push a bound that `Self: ThisTrait`
            m_current_type = &impl.m_type;
            m_current_trait = &m_crate.get_trait_by_path(Span(), trait_path);
            m_current_trait_path = &p;

            // The implemented trait is always in scope
            m_traits.push_back( ::std::make_pair( &trait_path, m_current_trait) );
            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
            m_traits.pop_back( );

            m_current_trait = nullptr;
            m_current_type = nullptr;
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override {
            ::HIR::ItemPath    p( impl.m_type, trait_path, impl.m_trait_args );
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type << " (mod=" << impl.m_src_module << ")");
            auto _t = this->push_mod_traits( this->m_crate.get_mod_by_path(Span(), impl.m_src_module) );
            auto _g = m_resolve.set_impl_generics(impl.m_params);


            // HACK: Expand defaults for parameters in trait names here.
            {
                Span    sp;
                const auto& trait = m_crate.get_trait_by_path(sp, trait_path);
                auto ms = MonomorphStatePtr(&impl.m_type, &impl.m_trait_args, nullptr);

                while( impl.m_trait_args.m_types.size() < trait.m_params.m_types.size() )
                {
                    const auto& def = trait.m_params.m_types[ impl.m_trait_args.m_types.size() ];
                    auto ty = ms.monomorph_type(sp, def.m_default);
                    DEBUG("Add default trait arg " << ty << " from " << def.m_default);
                    impl.m_trait_args.m_types.push_back( mv$(ty) );
                }
            }

            // TODO: Handle resolution of all items in m_resolve.m_type_equalities
            // - params might reference each other, so `set_item_generics` has to have been called
            // - But `m_type_equalities` can end up with non-resolved UFCS paths
            for(auto& e : m_resolve.m_type_equalities)
            {
                visit_type(e.second);
            }

            // TODO: Push a bound that `Self: ThisTrait`
            m_current_type = &impl.m_type;
            m_current_trait = &m_crate.get_trait_by_path(Span(), trait_path);
            m_current_trait_path = &p;

            // The implemented trait is always in scope
            m_traits.push_back( ::std::make_pair( &trait_path, m_current_trait) );
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            m_traits.pop_back( );

            m_current_trait = nullptr;
            m_current_type = nullptr;
        }

        void visit_expr(::HIR::ExprPtr& expr) override
        {
            struct ExprVisitor:
                public ::HIR::ExprVisitorDef
            {
                Visitor& upper_visitor;
                ::std::unique_ptr< ::HIR::ExprNode> m_replacement;


                ExprVisitor(Visitor& uv):
                    upper_visitor(uv)
                {}

                void visit_type(::HIR::TypeRef& ty) override
                {
                    upper_visitor.visit_type(ty);
                }
                void visit_path(::HIR::Visitor::PathContext pc, ::HIR::Path& path) override
                {
                    upper_visitor.visit_path(path, pc);
                }
                void visit_pattern(const Span& sp, ::HIR::Pattern& pat) override
                {
                    upper_visitor.visit_pattern(pat);
                }


                void visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) {
                    ::HIR::ExprVisitorDef::visit_node_ptr(node_ptr);
                    if(m_replacement) {
                        m_replacement.swap(node_ptr);
                        m_replacement.reset();
                    }
                }



                // Custom to visit the inner expression
                void visit(::HIR::ExprNode_ArraySized& node) override
                {
                    auto& as = node.m_size;
                    if( as.is_Unevaluated() && as.as_Unevaluated().is_Unevaluated() )
                    {
                        upper_visitor.visit_expr(*as.as_Unevaluated().as_Unevaluated());
                    }
                    ::HIR::ExprVisitorDef::visit(node);
                }

                // Custom visitor for enum/struct constructors
                void visit(::HIR::ExprNode_CallPath& node) override
                {
                    ::HIR::ExprVisitorDef::visit(node);
                    const Span& sp = node.span();
                    if(node.m_path.m_data.is_Generic())
                    {
                        // If it points to an enum, rewrite
                        auto& gp = node.m_path.m_data.as_Generic();
                        if( gp.m_path.m_components.size() > 1 )
                        {
                            const auto& ent = upper_visitor.m_crate.get_typeitem_by_path(sp, gp.m_path, /*ign_crate*/false, true);
                            if( ent.is_Enum() )
                            {
                                // Rewrite!
                                m_replacement.reset(new ::HIR::ExprNode_TupleVariant(sp, mv$(gp), /*is_struct*/false, mv$(node.m_args)));
                                DEBUG(&node << ": Replacing with TupleVariant " << m_replacement.get());
                                return ;
                            }
                        }
                    }
                }
                // Custom visitor for enum/struct constructors
                void visit(::HIR::ExprNode_PathValue& node) override
                {
                    ::HIR::ExprVisitorDef::visit(node);
                    const Span& sp = node.span();
                    if(node.m_path.m_data.is_Generic())
                    {
                        // If it points to an enum, set binding
                        auto& gp = node.m_path.m_data.as_Generic();
                        if( gp.m_path.m_components.size() > 1 )
                        {
                            const auto& ent = upper_visitor.m_crate.get_typeitem_by_path(sp, gp.m_path, /*ign_crate*/false, true);
                            if( ent.is_Enum() )
                            {
                                const auto& enm = ent.as_Enum();
                                auto idx = enm.find_variant(gp.m_path.m_components.back());
                                if( enm.m_data.is_Value() || enm.m_data.as_Data().at(idx).type == HIR::TypeRef::new_unit() )
                                {
                                    m_replacement.reset(new ::HIR::ExprNode_UnitVariant(sp, mv$(gp), /*is_struct*/false));
                                    DEBUG(&node << ": Replacing with UnitVariant " << m_replacement.get());
                                }
                                else
                                {
                                    node.m_target = ::HIR::ExprNode_PathValue::ENUM_VAR_CONSTR;
                                }
                                return ;
                            }
                        }

                        // TODO: Struct?
                    }
                }

                // NOTE: Custom needed for trait scoping
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

            if( m_visit_exprs && expr.get() != nullptr )
            {
                m_in_expr = true;
                ExprVisitor v { *this };
                (*expr).visit(v);
                m_in_expr = false;
            }
        }

        bool locate_trait_item_in_bounds(::HIR::Visitor::PathContext pc,  const ::HIR::TypeRef& tr, const ::HIR::GenericParams& params,  ::HIR::Path::Data& pd) {
            static Span sp;
            //const auto& name = pd.as_UfcsUnknown().item;
            for(const auto& b : params.m_bounds)
            {
                TU_IFLET(::HIR::GenericBound, b, TraitBound, e,
                    DEBUG("- " << e.type << " : " << e.trait.m_path);
                    if( e.type == tr ) {
                        DEBUG(" - Match");
                        if( locate_in_trait_and_set(pc, e.trait.m_path, m_crate.get_trait_by_path(sp, e.trait.m_path.m_path),  pd) ) {
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
                trait_path_g.m_params.m_types.push_back( trait.m_params.m_types[i].m_default.clone_shallow() );
            }
            return trait_path_g;
        }
        // Locate the item in `pd` and set `pd` to UfcsResolved if found
        // TODO: This code may end up generating paths without the type information they should contain
        bool locate_in_trait_and_set(::HIR::Visitor::PathContext pc, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd) {
            TRACE_FUNCTION_F(trait_path);
            // TODO: Get the span from caller
            static Span _sp;
            const auto& sp = _sp;
            if( locate_item_in_trait(pc, trait,  pd) ) {
                pd = get_ufcs_known(mv$(pd.as_UfcsUnknown()), trait_path.clone() /*make_generic_path(trait_path.m_path, trait)*/, trait);
                return true;
            }

            auto pp = trait_path.m_params.clone();
            while( pp.m_types.size() < trait.m_params.m_types.size() )
            {
                auto idx = pp.m_types.size();
                const auto& def = trait.m_params.m_types[idx].m_default;
                if( def == HIR::TypeRef() ) {
                    ERROR(sp, E0000, "");
                }
                if( def == ::HIR::TypeRef("Self", 0xFFFF) )
                {
                    // TODO: This has to be the _exact_ same type, including future ivars.
                    pp.m_types.push_back( pd.as_UfcsUnknown().type.clone() );
                    continue ;
                }
                TODO(sp, "Monomorphise default arg " << def << " for trait path " << trait_path);
                //pp.m_types.push_back( def.
            }

            auto monomorph_cb = MonomorphStatePtr(&pd.as_UfcsUnknown().type, &pp, nullptr);
            //auto monomorph_cb = [&](const auto& ty)->const ::HIR::TypeRef& {
            //    const auto& ge = ty.data().as_Generic();
            //    if( ge.binding == 0xFFFF ) {
            //        // TODO: This has to be the _exact_ same type, including future ivars.
            //        return pd.as_UfcsUnknown().type;
            //    }
            //    else if( (ge.binding >> 8) == 0 ) {
            //        auto idx = ge.binding & 0xFF;
            //        ASSERT_BUG(sp, idx < trait.m_params.m_types.size(), "");
            //        if( idx < trait_path.m_params.m_types.size() )
            //            return trait_path.m_params.m_types[idx];
            //        // If the param is omitted, but has a default, use the default.
            //        else if( trait.m_params.m_types[idx].m_default != ::HIR::TypeRef() ) {
            //            const auto& def = trait.m_params.m_types[idx].m_default;
            //            if( ! monomorphise_type_needed(def) )
            //                return def;
            //            if( def == ::HIR::TypeRef("Self", 0xFFFF) )
            //                // TODO: This has to be the _exact_ same type, including future ivars.
            //                return pd.as_UfcsUnknown().type;
            //            TODO(sp, "Monomorphise default arg " << def << " for trait path " << trait_path);
            //        }
            //        else
            //            BUG(sp, "Binding out of range in " << ty << " for trait path " << trait_path);
            //    }
            //    else {
            //        ERROR(sp, E0000, "Unexpected generic binding " << ty);
            //    }
            //    };
            ::HIR::GenericPath  par_trait_path_tmp;
            auto monomorph_gp_if_needed = [&](const ::HIR::GenericPath& tpl)->const ::HIR::GenericPath& {
                // NOTE: This doesn't monomorph if the parameter set is the same
                if( monomorphise_genericpath_needed(tpl) && tpl.m_params != trait_path.m_params ) {
                    DEBUG("- Monomorph " << tpl);
                    return par_trait_path_tmp = monomorph_cb.monomorph_genericpath(sp, tpl, false /*no infer*/);
                }
                else {
                    return tpl;
                }
                };

            // Search supertraits (recursively)
            for(const auto& pt : trait.m_parent_traits)
            {
                const auto& par_trait_path = monomorph_gp_if_needed(pt.m_path);
                DEBUG("- Check " << par_trait_path);
                if( locate_in_trait_and_set(pc, par_trait_path, *pt.m_trait_ptr,  pd) ) {
                    return true;
                }
            }
            for(const auto& pt : trait.m_all_parent_traits)
            {
                const auto& par_trait_path = monomorph_gp_if_needed(pt.m_path);
                DEBUG("- Check (all) " << par_trait_path);
                if( locate_item_in_trait(pc, *pt.m_trait_ptr,  pd) ) {
                    // TODO: Don't clone if this is from the temp.
                    pd = get_ufcs_known(mv$(pd.as_UfcsUnknown()), par_trait_path.clone(), *pt.m_trait_ptr);
                    return true;
                }
            }
            return false;
        }

        bool set_from_trait_impl(const Span& sp, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait, ::HIR::Path::Data& pd)
        {
            auto& e = pd.as_UfcsUnknown();
            const auto& type = e.type;
            TRACE_FUNCTION_F("trait_path=" << trait_path << ", p=<" << type << " as _>::" << e.item);

            // TODO: This is VERY arbitary and possibly nowhere near what rustc does.
            // NOTE: `nullptr` passed for param count, as defaults are not yet expanded
            this->m_resolve.find_impl(sp,  trait_path.m_path, nullptr, type, [&](const auto& impl, bool fuzzy)->bool{
                auto pp = impl.get_trait_params();
                // Replace all placeholder parameters (group 2) with ivars (empty types)
                struct KillPlaceholders:
                    public Monomorphiser
                {
                    ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& ty) const override {
                        if( ty.is_placeholder() ) {
                            return HIR::TypeRef();
                        }
                        return HIR::TypeRef(ty);
                    }
                    ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& val) const override {
                        return val;
                    }
                };

                pp = KillPlaceholders().monomorph_path_params(sp, pp, true);
                DEBUG("FOUND impl from " << impl);
                // If this has already found an option...
                if(auto* inner_e = pd.opt_UfcsKnown())
                {
                    // Compare all path params, and set different params to _
                    assert( pp.m_types.size() == inner_e->trait.m_params.m_types.size() );
                    for(unsigned int i = 0; i < pp.m_types.size(); i ++ )
                    {
                        auto& e_ty = inner_e->trait.m_params.m_types[i];
                        const auto& this_ty = pp.m_types[i];
                        if( e_ty == ::HIR::TypeRef() ) {
                            // Already _, leave as is
                        }
                        else if( e_ty != this_ty ) {
                            e_ty = ::HIR::TypeRef();
                        }
                        else {
                            // Equal, good
                        }
                    }
                }
                else {
                    DEBUG("pp = " << pp);
                    // Otherwise, set to the current result.
                    pd = get_ufcs_known(mv$(e), ::HIR::GenericPath(trait_path.m_path, mv$(pp)), trait);
                }
                return false;
                });
            return pd.is_UfcsKnown();
        }

        bool locate_in_trait_impl_and_set(const Span& sp, ::HIR::Visitor::PathContext pc, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd)
        {
            if( this->locate_item_in_trait(pc, trait,  pd) ) {
                return set_from_trait_impl(sp, trait_path, trait, pd);
            }
            else {
                DEBUG("- Item " << pd.as_UfcsUnknown().item << " not in trait " << trait_path.m_path);
            }


            // Search supertraits (recursively)
            // NOTE: This runs before "Resolve HIR Markings", so m_all_parent_traits can't be used exclusively
            for(const auto& pt : trait.m_parent_traits)
            {
                // TODO: Modify path parameters based on the current trait's params
                if( locate_in_trait_impl_and_set(sp, pc, pt.m_path, *pt.m_trait_ptr,  pd) ) {
                    return true;
                }
            }
            for(const auto& pt : trait.m_all_parent_traits)
            {
                if( this->locate_item_in_trait(pc, *pt.m_trait_ptr,  pd) ) {
                    // TODO: Modify path parameters based on the current trait's params
                    return set_from_trait_impl(sp, pt.m_path, *pt.m_trait_ptr, pd);
                }
                else {
                    DEBUG("- Item " << pd.as_UfcsUnknown().item << " not in trait " << trait_path.m_path);
                }
            }
            return false;
        }

        bool resolve_UfcsUnknown_inherent(const ::HIR::Path& p, ::HIR::Visitor::PathContext pc, ::HIR::Path::Data& pd)
        {
            auto& e = pd.as_UfcsUnknown();
            return m_crate.find_type_impls(e.type, [&](const auto& t)->const auto& { return t; }, [&](const auto& impl) {
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
                pd = mv$(new_data);
                DEBUG("- Resolved, replace with " << p);
                return true;
                });
        }

        bool resolve_UfcsUnknown_trait(const ::HIR::Path& p, ::HIR::Visitor::PathContext pc, ::HIR::Path::Data& pd)
        {
            static Span sp;
            auto& e = pd.as_UfcsUnknown();
            DEBUG("m_traits.size() = " << m_traits.size());
            for( const auto& trait_info : m_traits )
            {
                const auto& trait = *trait_info.second;

                DEBUG( e.item << " in? " << *trait_info.first );
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

                // TODO: If there's only one trait with this name, assume it's the correct one.

                // TODO: Search supertraits
                // TODO: Should impls be searched first, or item names?
                // - Item names add complexity, but impls are slower
                if( this->locate_in_trait_impl_and_set(sp, pc, mv$(trait_path), trait,  pd) ) {
                    return true;
                }
            }
            return false;
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            // TODO: Add a span parameter.
            static Span sp;

            ::HIR::Visitor::visit_type(ty);

            // TODO: If this an associated type, check for default trait params

            if( m_run_eat )
            {
                unsigned counter = 0;
                while( m_resolve.expand_associated_types_single(sp, ty) )
                {
                    //ASSERT_BUG(sp, !visit_ty_with(ty, [&](const HIR::TypeRef& ty)->bool { return TU_TEST1(ty.data(), Generic, .is_placeholder()); }), "Encountered placeholder - " << ty);
                    visit_ty_with_mut(ty, [&](HIR::TypeRef& ty)->bool { if( TU_TEST1(ty.data(), Generic, .is_placeholder()) ) ty = HIR::TypeRef(); return false; });
                    ASSERT_BUG(sp, counter++ < 20, "Sanity limit exceeded when resolving UFCS in type " << ty);
                    // Invoke a special version of EAT that only processes a single item.
                    // - Keep recursing while this does replacements
                    ::HIR::Visitor::visit_type(ty);
                }
            }
        }

        void visit_path(::HIR::Path& p, ::HIR::Visitor::PathContext pc) override
        {
            static Span sp;

            if(auto* pe = p.m_data.opt_UfcsKnown())
            {
                // If the trait has missing type argumenst, replace them with the defaults
                auto& tp = pe->trait;
                const auto& trait = m_resolve.m_crate.get_trait_by_path(sp, tp.m_path);

                if(tp.m_params.m_types.size() < trait.m_params.m_types.size())
                {
                    //TODO(sp, "Defaults in UfcsKnown - " << p << " - " << tp.m_params << " vs " << trait.m_params.fmt_args());
                    // TOOD: Where does this usually get expanded then?
                }
            }

            // TODO: Would like to remove this, but it's required still (for expressions)
            if(auto* pe = p.m_data.opt_UfcsUnknown())
            {
                auto& e = *pe;
                TRACE_FUNCTION_FR("UfcsUnknown - p=" << p, p);

                this->visit_type( e.type );
                this->visit_path_params( e.params );

                // If processing a trait, and the type is 'Self', search for the type/method on the trait
                // - Explicitly encoded because `Self::Type` has a different meaning to `MyType::Type` (the latter will search bounds first)
                // - NOTE: Could be in an inherent block, where there's no trait
                if( /*m_current_type &&*/ m_current_trait && e.type == ::HIR::TypeRef("Self", GENERIC_Self) )
                {
                    ::HIR::GenericPath  trait_path;
                    if( m_current_trait_path->trait_path() )
                    {
                        trait_path = ::HIR::GenericPath( *m_current_trait_path->trait_path() );
                        trait_path.m_params = m_current_trait_path->trait_args()->clone();
                    }
                    else
                    {
                        trait_path = ::HIR::GenericPath( m_current_trait_path->get_simple_path() );
                        for(unsigned int i = 0; i < m_current_trait->m_params.m_types.size(); i ++ ) {
                            trait_path.m_params.m_types.push_back( ::HIR::TypeRef(m_current_trait->m_params.m_types[i].m_name, i) );
                        }
                    }
                    if( locate_in_trait_and_set(pc, trait_path, *m_current_trait,  p.m_data) ) {
                        assert(!p.m_data.is_UfcsUnknown());
                        // Success!
                        // - If in an expression (and not in a `trait` provided impl), clear the params
                        if( m_in_expr && !m_in_trait_def ) {
                            for(auto& t : p.m_data.as_UfcsKnown().trait.m_params.m_types)
                                t = ::HIR::TypeRef();
                        }
                        DEBUG("Found in Self, p = " << p);
                        return ;
                    }
                    DEBUG("- Item " << e.item << " not found in Self - ty=" << e.type);
                }

                // NOTE: Replace `Self` now
                // - Now that the only `Self`-specific logic is done, replace so the lookup code works.
                if(m_current_type)
                {
                    visit_path_tys_with_mut(p, [&](HIR::TypeRef& t)->bool{
                        if(t.data().is_Generic() && t.data().as_Generic().binding == GENERIC_Self) {
                            t = m_current_type->clone();
                        }
                        return false;
                        });
                }

                // Search for matching impls in current generic blocks
                if( m_resolve.m_item_generics != nullptr && locate_trait_item_in_bounds(pc, e.type, *m_resolve.m_item_generics,  p.m_data) ) {
                    DEBUG("Found in item params, p = " << p);
                    assert(!p.m_data.is_UfcsUnknown());
                    return ;
                }
                if( m_resolve.m_impl_generics != nullptr && locate_trait_item_in_bounds(pc, e.type, *m_resolve.m_impl_generics,  p.m_data) ) {
                    DEBUG("Found in impl params, p = " << p);
                    assert(!p.m_data.is_UfcsUnknown());
                    return ;
                }

                // TODO: Control ordering with a flag in UfcsUnknown
                // 1. Search for applicable inherent methods (COMES FIRST!)
                if( this->resolve_UfcsUnknown_inherent(p, pc, p.m_data) ) {
                    assert(!p.m_data.is_UfcsUnknown());
                    return ;
                }
                assert(p.m_data.is_UfcsUnknown());

                // If the type is the impl type, look for items AFTER generic lookup
                // TODO: Should this look up in-scope traits instead of hard-coding this hack?
                if( m_current_type && m_current_trait && e.type == *m_current_type )
                {
                    ::HIR::GenericPath  trait_path;
                    if( m_current_trait_path->trait_path() )
                    {
                        trait_path = ::HIR::GenericPath( *m_current_trait_path->trait_path() );
                        trait_path.m_params = m_current_trait_path->trait_args()->clone();
                    }
                    else
                    {
                        trait_path = ::HIR::GenericPath( m_current_trait_path->get_simple_path() );
                        for(unsigned int i = 0; i < m_current_trait->m_params.m_types.size(); i ++ ) {
                            trait_path.m_params.m_types.push_back( ::HIR::TypeRef(m_current_trait->m_params.m_types[i].m_name, i) );
                        }
                    }
                    if( locate_in_trait_and_set(pc, trait_path, *m_current_trait,  p.m_data) ) {
                        assert(!p.m_data.is_UfcsUnknown());
                        // Success!
                        if( m_in_expr ) {
                            for(auto& t : p.m_data.as_UfcsKnown().trait.m_params.m_types)
                                t = ::HIR::TypeRef();
                        }
                        DEBUG("Found in Self, p = " << p);
                        return ;
                    }
                    DEBUG("- Item " << e.item << " not found in Self - ty=" << e.type);
                }

                // If the inner type is a UFCS of a known trait, then search traits on that type
                if( e.type.data().is_Path() && e.type.data().as_Path().path.m_data.is_UfcsKnown() )
                {
                    auto& inner_pe = e.type.data().as_Path().path.m_data.as_UfcsKnown();
                    const auto& trait = m_crate.get_trait_by_path(sp, inner_pe.trait.m_path);
                    const auto& aty_def = trait.m_types.at(inner_pe.item);
                    auto mstate = MonomorphStatePtr(&inner_pe.type, &inner_pe.trait.m_params, nullptr);
                    for(const auto& t : aty_def.m_trait_bounds)
                    {
                        auto trait_path = mstate.monomorph_genericpath(sp, t.m_path, /*allow_infer*/true);
                        DEBUG("Searching ATY bound: " << trait_path);
                        // Search within this (bounded) trait for the outer item
                        if( this->locate_in_trait_impl_and_set(sp, pc, mv$(trait_path), *t.m_trait_ptr,  p.m_data) ) {
                            assert(!p.m_data.is_UfcsUnknown());
                            return ;
                        }
                    }
                    DEBUG("- Item " << e.item << " not found in ATY bounds");
                    // TODO: Search bounds with `where`?
                }

                // 2. Search all impls of in-scope traits for this method on this type
                if( this->resolve_UfcsUnknown_trait(p, pc, p.m_data) ) {
                    assert(!p.m_data.is_UfcsUnknown());
                    return ;
                }
                assert(p.m_data.is_UfcsUnknown());
                DEBUG("e.type = " << e.type);

                // If the inner is an enum, look for an enum variant? (check context)
                if( (pc == HIR::Visitor::PathContext::VALUE /*|| pc == HIR::Visitor::PathContext::PATTERN*/)
                    && e.type.data().is_Path()
                    && e.type.data().as_Path().binding.is_Enum()
                    )
                {
                    const auto& enm = *e.type.data().as_Path().binding.as_Enum();
                    auto idx = enm.find_variant(e.item);
                    DEBUG(idx);
                    if( idx != ~0u )
                    {
                        DEBUG("Found variant " << e.type << " #" << idx);
                        if( enm.m_data.is_Value() || !enm.m_data.as_Data()[idx].is_struct ) {
                            auto gp = e.type.data().as_Path().path.m_data.as_Generic().clone();
                            gp.m_path.m_components.push_back(e.item);
                            if( e.params.has_params() ) {
                                ERROR(sp, E0000, "Type parameters on UFCS enum variant - " << p);
                            }
                            p = std::move(gp);
                            return ;
                        }
                        else {
                        }
                    }
                }

                // Couldn't find it
                ERROR(sp, E0000, "Failed to find impl with '" << e.item << "' for " << e.type << " (in " << p << ")");
            }
            else
            {
                ::HIR::Visitor::visit_path(p, pc);
            }
        }

        void visit_pattern(::HIR::Pattern& pat) override
        {
            static Span _sp = Span();
            const Span& sp = _sp;

            ::HIR::Visitor::visit_pattern(pat);

            TU_MATCH_HDRA( (pat.m_data), {)
            default:
                break;
            TU_ARMA(Value, e) {
                this->visit_pattern_Value(sp, pat, e.val);
                if( e.val.is_Named() && e.val.as_Named().path.m_data.is_Generic() && e.val.as_Named().path.m_data.as_Generic().m_path.m_components.size() > 1 )
                {
                    auto& gp = e.val.as_Named().path.m_data.as_Generic();
                    if( const auto* enm_p = m_crate.get_typeitem_by_path(sp, gp.m_path, false, true).opt_Enum() )
                    {
                        unsigned idx = enm_p->find_variant(gp.m_path.m_components.back());
                        pat.m_data = ::HIR::Pattern::Data::make_PathValue({
                            mv$(gp),
                            ::HIR::Pattern::PathBinding::make_Enum({enm_p, idx})
                            });
                    }
                }
                }
            TU_ARMA(Range, e) {
                if(e.start) this->visit_pattern_Value(sp, pat, *e.start);
                if(e.end  ) this->visit_pattern_Value(sp, pat, *e.end);
                }
            }
        }
        void visit_pattern_Value(const Span& sp, const ::HIR::Pattern& pat, ::HIR::Pattern::Value& val)
        {
            TRACE_FUNCTION_F("pat=" << pat << ", val=" << val);
            if(auto* vep = val.opt_Named())
            {
                auto& ve = *vep;
                TRACE_FUNCTION_F(ve.path);
                TU_MATCH_HDRA( (ve.path.m_data), {)
                TU_ARMA(Generic, pe) {
                    // Already done
                    }
                TU_ARMA(UfcsUnknown, pe) {
                    BUG(sp, "UfcsUnknown still in pattern value - " << pat);
                    }
                TU_ARMA(UfcsInherent, pe) {
                    bool rv = m_crate.find_type_impls(pe.type, [&](const auto& t)->const auto& { return t; }, [&](const auto& impl) {
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
                    }
                TU_ARMA(UfcsKnown, pe) {
                    bool rv = this->m_resolve.find_impl(sp,  pe.trait.m_path, &pe.trait.m_params, pe.type, [&](const auto& impl, bool) {
                        if( !impl.m_data.is_TraitImpl() ) {
                            return true;
                        }
                        ve.binding = &impl.m_data.as_TraitImpl().impl->m_constants.at( pe.item ).data;
                        return true;
                        });
                    if( !rv ) {
                        ERROR(sp, E0000, "Constant " << ve.path << " couldn't be found");
                    }
                    }
                }
            }
        }
    };

    template<typename T>
    void sort_impl_group(::HIR::Crate::ImplGroup<std::unique_ptr<T>>& ig, ::std::function<void(::std::ostream& os, const T&)> fmt)
    {
        auto new_end = ::std::remove_if(ig.generic.begin(), ig.generic.end(), [&ig,&fmt](::std::unique_ptr<T>& ty_impl) {
            const auto& type = ty_impl->m_type;  // Using field accesses in templates feels so dirty
            const ::HIR::SimplePath*    path = type.get_sort_path();

            if( path )
            {
                DEBUG(*path << " += " << FMT_CB(os, fmt(os, *ty_impl)));
                ig.named[*path].push_back(mv$(ty_impl));
            }
            else if( type.data().is_Path() || type.data().is_Generic() )
            {
                return false;
            }
            else
            {
                ig.non_named.push_back(mv$(ty_impl));
            }
            return true;
            });
        ig.generic.erase(new_end, ig.generic.end());
    }

    // --- Indexing of trait impls ---
    template<typename T>
    void push_index_impl_group_list(::std::vector<const T*>& dst, const ::std::vector<std::unique_ptr<T>>& src)
    {
        for(const auto& e : src) {
            dst.push_back(&*e);
        }
    }
    template<typename T>
    void push_index_impl_group(::HIR::Crate::ImplGroup<const T*>& dst, const ::HIR::Crate::ImplGroup<std::unique_ptr<T>>& src)
    {
        for(const auto& e : src.named) {
            push_index_impl_group_list(dst.named[e.first], e.second);
        }
        push_index_impl_group_list(dst.non_named, src.non_named);
        push_index_impl_group_list(dst.generic  , src.generic  );
    }
    void push_index_impls(::HIR::Crate& dst, const ::HIR::Crate& src)
    {
        push_index_impl_group(dst.m_all_type_impls, src.m_type_impls);
        for(const auto& ig : src.m_trait_impls) {
            push_index_impl_group(dst.m_all_trait_impls[ig.first], ig.second);
        }
        for(const auto& ig : src.m_marker_impls) {
            push_index_impl_group(dst.m_all_marker_impls[ig.first], ig.second);
        }
    }

    // --- Indexing of inherent methods ---
    void push_index_inherent_methods_list(::HIR::InherentCache& icache, const HIR::SimplePath& lang_Box, const ::std::vector<std::unique_ptr< HIR::TypeImpl >>& src)
    {
        Span    sp;
        for(const auto& ti : src)
        {
            const auto& impl = *ti;
            DEBUG("impl" << impl.m_params.fmt_args() << " " << impl.m_type);
            icache.insert_all(sp, impl, lang_Box);
        }
    }
    void push_index_inherent_methods(::HIR::InherentCache& icache, const HIR::SimplePath& lang_Box, const ::HIR::Crate& src)
    {
        for(const auto& e : src.m_type_impls.named) {
            push_index_inherent_methods_list(icache, lang_Box, e.second);
        }
        push_index_inherent_methods_list(icache, lang_Box, src.m_type_impls.non_named);
        push_index_inherent_methods_list(icache, lang_Box, src.m_type_impls.generic  );
    }
}   // namespace ""
using namespace resolve_ufcs;

void ConvertHIR_ResolveUFCS_Outer(::HIR::Crate& crate)
{
    Visitor exp { crate, false };
    exp.visit_crate( crate );
}
void ConvertHIR_ResolveUFCS(::HIR::Crate& crate)
{
    Visitor exp { crate, true };
    exp.visit_crate( crate );
}

void ConvertHIR_ResolveUFCS_SortImpls(::HIR::Crate& crate)
{
    // Sort impls!
    sort_impl_group<HIR::TypeImpl>(crate.m_type_impls,
        [](::std::ostream& os, const HIR::TypeImpl& i){ os << "impl" << i.m_params.fmt_args() << " " << i.m_type; }
        );
    DEBUG("Type impl counts: " << crate.m_type_impls.named.size() << " path groups, " << crate.m_type_impls.non_named.size() << " primitive, " << crate.m_type_impls.generic.size() << " ungrouped");
    for(auto& impl_group : crate.m_trait_impls)
    {
        sort_impl_group<HIR::TraitImpl>(impl_group.second,
            [&](::std::ostream& os, const HIR::TraitImpl& i){ os << "impl" << i.m_params.fmt_args() << " " << impl_group.first << i.m_trait_args << " for " << i.m_type; }
            );
    }
    for(auto& impl_group : crate.m_marker_impls)
    {
        sort_impl_group<HIR::MarkerImpl>(impl_group.second,
            [&](::std::ostream& os, const HIR::MarkerImpl& i){ os << "impl" << i.m_params.fmt_args() << " " << impl_group.first << i.m_trait_args << " for " << i.m_type << " {}"; }
            );
    }


    // Create indexes
    push_index_impls(crate, crate);
    for(const auto& ec : crate.m_ext_crates) {
        push_index_impls(crate, *ec.second.m_data);
    }

    {
        const auto& lang_Box = crate.get_lang_item_path_opt("owned_box");
        push_index_inherent_methods(crate.m_inherent_method_cache, lang_Box, crate);
        for(const auto& ec : crate.m_ext_crates) {
            push_index_inherent_methods(crate.m_inherent_method_cache, lang_Box, *ec.second.m_data);
        }
    }
}
