/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/expr_cs__enum.cpp
 * - "Constraint Solver" type inferrence (rule enumeration)
 */
#include "expr_cs.hpp"
#include "expr_visit.hpp"
#include <hir/expr.hpp>

#include <algorithm>

namespace
{
    inline ::HIR::SimplePath get_parent_path(const ::HIR::SimplePath& sp) {
        auto rv = sp;
        rv.m_components.pop_back();
        return rv;
    }
    inline ::HIR::GenericPath get_parent_path(const ::HIR::GenericPath& gp) {
        auto rv = gp.clone();
        rv.m_path.m_components.pop_back();
        return rv;
    }
}

namespace typecheck
{
    bool visit_call_populate_cache(Context& context, const Span& sp, ::HIR::Path& path, ::HIR::ExprCallCache& cache) __attribute__((warn_unused_result));
    bool visit_call_populate_cache_UfcsInherent(Context& context, const Span& sp, ::HIR::Path& path, ::HIR::ExprCallCache& cache, const ::HIR::Function*& fcn_ptr);


    class OwnedImplMatcher:
        public ::HIR::MatchGenerics
    {
        ::HIR::PathParams& impl_params;
    public:
        OwnedImplMatcher(::HIR::PathParams& impl_params):
            impl_params(impl_params)
        {}

        ::HIR::Compare match_ty(const ::HIR::GenericRef& g, const ::HIR::TypeRef& ty, ::HIR::t_cb_resolve_type _resolve_cb) override {
            assert( g.binding < impl_params.m_types.size() );
            impl_params.m_types[g.binding] = ty.clone();
            return ::HIR::Compare::Equal;
        }
        ::HIR::Compare match_val(const ::HIR::GenericRef& g, const ::HIR::ConstGeneric& sz) override {
            assert( g.binding < impl_params.m_values.size() );
            ASSERT_BUG(Span(), impl_params.m_values[g.binding] == ::HIR::ConstGeneric(), "TODO: Multiple values? " << impl_params.m_values[g.binding] << " and " << sz);
            impl_params.m_values[g.binding] = sz.clone();
            return ::HIR::Compare::Equal;
        }
    };


    void populate_defaults(const Span& sp, Context& context, const MonomorphStatePtr& ms, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params)
    {
#if 1
        for(size_t i = 0; i < param_defs.m_types.size(); i ++)
        {
            const auto& ty = params.m_types[i];
            const auto& typ = param_defs.m_types[i];
            if( const auto* te = ty.data().opt_Infer() )
            {
                if( !typ.m_default.data().is_Infer() )
                {
                    if(auto* ent = context.get_ivar_possibilities(sp, te->index))
                    {
                        auto def_ty = ms.monomorph_type(sp, typ.m_default);
                        DEBUG("Added default for " << ty << ": " << def_ty);
                        ent->types_default.insert(std::move(def_ty));
                    }
                }
            }
        }
#endif
    }
    template<typename T>
    void fix_param_count_(const Span& sp, Context& context, const ::HIR::TypeRef& self_ty, bool use_defaults, const T& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params)
    {
        if( params.m_types.size() == param_defs.m_types.size() ) {
            // Nothing to do, all good
        }
        else if( params.m_types.size() > param_defs.m_types.size() ) {
            ERROR(sp, E0000, "Too many type parameters passed to " << path);
        }
        else {
            while( params.m_types.size() < param_defs.m_types.size() ) {
                const auto& typ = param_defs.m_types[params.m_types.size()];
                if( use_defaults )
                {
                    if( typ.m_default.data().is_Infer() ) {
                        ERROR(sp, E0000, "Omitted type parameter with no default in " << path);
                    }
                    else if( monomorphise_type_needed(typ.m_default) ) {
                        auto cb = MonomorphStatePtr(&self_ty, nullptr, nullptr);
                        params.m_types.push_back( cb.monomorph_type(sp, typ.m_default) );
                    }
                    else {
                        params.m_types.push_back( typ.m_default.clone_shallow() );
                    }
                }
                else
                {
                    params.m_types.push_back( context.m_ivars.new_ivar_tr() );
                    // TODO: It's possible that the default could be added using `context.possible_equate_type_def` to give inferrence a fallback
                }
            }
        }

        if( params.m_values.size() == param_defs.m_values.size() ) {
            // Nothing to do, all good
        }
        else if( params.m_values.size() > param_defs.m_values.size() ) {
            ERROR(sp, E0000, "Too many const parameters passed to " << path);
        }
        else {
            while( params.m_values.size() < param_defs.m_values.size() ) {
                //const auto& def = param_defs.m_values[params.m_values.size()];
                params.m_values.push_back({});
                context.m_ivars.add_ivars(params.m_values.back());
            }
        }
    }
    void fix_param_count(const Span& sp, Context& context, const ::HIR::TypeRef& self_ty, bool use_defaults, const ::HIR::Path& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params) {
        fix_param_count_(sp, context, self_ty, use_defaults, path, param_defs, params);
    }
    void fix_param_count(const Span& sp, Context& context, const ::HIR::TypeRef& self_ty, bool use_defaults, const ::HIR::GenericPath& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params) {
        fix_param_count_(sp, context, self_ty, use_defaults, path, param_defs, params);
    }
    
    void apply_bounds_as_rules(Context& context, const Span& sp, const ::HIR::GenericParams& params_def, const Monomorphiser& ms, bool is_impl_level)
    {
        TRACE_FUNCTION;
        for(const auto& bound : params_def.m_bounds)
        {
            TU_MATCH_HDRA( (bound), {)
            TU_ARMA(Lifetime, be) {
                }
            TU_ARMA(TypeLifetime, be) {
                }
            TU_ARMA(TraitBound, be) {
                auto real_type = ms.monomorph_type(sp, be.type);
                auto real_trait = ms.monomorph_genericpath(sp, be.trait.m_path, false);
                DEBUG("Bound " << be.type << ":  " << be.trait);
                DEBUG("= (" << real_type << ": " << real_trait << ")");
                const auto& trait_params = real_trait.m_params;

                const auto& trait_path = be.trait.m_path.m_path;
                // If there's no type bounds, emit a trait bound
                // - Otherwise, the assocated type bounds will serve the same purpose
                if( be.trait.m_type_bounds.size() == 0 )
                {
                    context.add_trait_bound(sp, real_type, trait_path, trait_params.clone());
                }

                for( const auto& assoc : be.trait.m_type_bounds ) {
                    ::HIR::GenericPath  type_trait_path = ms.monomorph_genericpath(sp, assoc.second.source_trait, true);

                    auto other_ty = ms.monomorph_type(sp, assoc.second.type, true);

                    context.equate_types_assoc(sp, other_ty,  type_trait_path.m_path, mv$(type_trait_path.m_params.m_types), real_type, assoc.first.c_str());
                }
                }
            TU_ARMA(TypeEquality, be) {
                auto real_type_left = context.m_resolve.expand_associated_types(sp, ms.monomorph_type(sp, be.type));
                auto real_type_right = context.m_resolve.expand_associated_types(sp, ms.monomorph_type(sp, be.other_type));
                context.equate_types(sp, real_type_left, real_type_right);
                }
            }
        }

        for(size_t i = 0; i < params_def.m_types.size(); i++)
        {
            if( params_def.m_types[i].m_is_sized )
            {
                ::HIR::TypeRef  ty("", (is_impl_level ? 0 : 256) + i);
                context.require_sized(sp, ms.get_type(Span(), ty.data().as_Generic()));
            }
        }
    }


    /// (HELPER) Populate the cache for nodes that use visit_call
    /// TODO: If the function has multiple mismatched options, tell the caller to try again later?
    bool visit_call_populate_cache(Context& context, const Span& sp, ::HIR::Path& path, ::HIR::ExprCallCache& cache)
    {
        TRACE_FUNCTION_FR(path, path);
        assert(cache.m_arg_types.size() == 0);

        const ::HIR::Function*  fcn_ptr = nullptr;

        struct Monomorph:
            public Monomorphiser
        {
            Context& context;
            const HIR::TypeRef* self_ty;
            const HIR::PathParams* impl_params;
            const HIR::PathParams& fcn_params;
            Monomorph(Context& context, const HIR::TypeRef* self_ty, const HIR::PathParams* impl_params, const HIR::PathParams& fcn_params)
                : context(context)
                , self_ty(self_ty)
                , impl_params(impl_params)
                , fcn_params(fcn_params)
            {
            }

            ::HIR::TypeRef get_type(const Span& sp, const HIR::GenericRef& e) const override
            {
                if( e.name == "Self" || e.is_self() )
                {
                    if( self_ty )
                    {
                        return self_ty->clone();
                    }
                    else
                    {
                        TODO(sp, "Handle 'Self' when monomorphising");
                    }
                }
                else if( e.binding < 256 )
                {
                    if( impl_params )
                    {
                        auto idx = e.idx();
                        ASSERT_BUG(sp, idx < impl_params->m_types.size(), "Generic param (impl) out of input range - " << e << " >= " << impl_params->m_types.size());
                        return context.get_type(impl_params->m_types[idx]).clone();
                    }
                    else
                    {
                        BUG(sp, "Impl-level parameter on free function (" << e << ")");
                    }
                }
                else if( e.binding < 512 )
                {
                    auto idx = e.idx();
                    ASSERT_BUG(sp, idx < fcn_params.m_types.size(), "Generic param out of input range - " << e << " >= " << fcn_params.m_types.size());
                    return context.get_type(fcn_params.m_types[idx]).clone();
                }
                else {
                    BUG(sp, "Generic bounding out of total range (" << e << ")");
                }
            }

            ::HIR::ConstGeneric get_value(const Span& sp, const HIR::GenericRef& e) const override
            {
                if( e.binding < 256 )
                {
                    ASSERT_BUG(sp, impl_params, "Impl-level value parameter on free function (" << e << ")");
                    auto idx = e.idx();
                    ASSERT_BUG(sp, idx < impl_params->m_values.size(), "Generic value (impl) out of input range - " << e << " >= " << impl_params->m_values.size());
                    return context.m_ivars.get_value(impl_params->m_values[idx]).clone();
                }
                else if( e.binding < 512 )
                {
                    auto idx = e.idx();
                    ASSERT_BUG(sp, idx < fcn_params.m_values.size(), "Generic value out of input range - " << e << " >= " << fcn_params.m_values.size());
                    return context.m_ivars.get_value(fcn_params.m_values[idx]).clone();
                }
                else {
                    BUG(sp, "Generic value bounding out of total range (" << e << ")");
                }
            }
        };

        TU_MATCH_HDRA( (path.m_data), {)
        TU_ARMA(Generic, e) {
            const auto& fcn = context.m_crate.get_function_by_path(sp, e.m_path);
            fix_param_count(sp, context, ::HIR::TypeRef(), false, path, fcn.m_params,  e.m_params);
            fcn_ptr = &fcn;
            cache.m_fcn_params = &fcn.m_params;

            //const auto& params_def = fcn.m_params;
            const auto& path_params = e.m_params;
            cache.m_monomorph.reset(new Monomorph(context, nullptr, nullptr, path_params));
            }
        TU_ARMA(UfcsKnown, e) {
            const auto& trait = context.m_crate.get_trait_by_path(sp, e.trait.m_path);
            fix_param_count(sp, context, e.type, true, path, trait.m_params, e.trait.m_params);
            if( trait.m_values.count(e.item) == 0 ) {
                BUG(sp, "Method '" << e.item << "' of trait " << e.trait.m_path << " doesn't exist");
            }
            const auto& fcn = trait.m_values.at(e.item).as_Function();
            fix_param_count(sp, context, e.type, false, path, fcn.m_params,  e.params);
            cache.m_fcn_params = &fcn.m_params;
            cache.m_top_params = &trait.m_params;

            // Add a bound requiring the Self type impl the trait
            context.add_trait_bound(sp, e.type,  e.trait.m_path, e.trait.m_params.clone());

            fcn_ptr = &fcn;

            cache.m_monomorph.reset(new Monomorph(context, &e.type, &e.trait.m_params, e.params));
        }
        TU_ARMA(UfcsUnknown, e) {
            // TODO: Eventually, the HIR `Resolve UFCS` pass will be removed, leaving this code responsible for locating the item.
            TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
            }
        TU_ARMA(UfcsInherent, e) {
            // NOTE: This case is kinda long, so it's refactored out into a helper
            if( !visit_call_populate_cache_UfcsInherent(context, sp, path, cache, fcn_ptr) ) {
                return false;
            }
            }
        }

        assert( fcn_ptr );
        cache.m_fcn = fcn_ptr;
        const auto& fcn = *fcn_ptr;
        const auto& monomorph = *cache.m_monomorph;

        // --- Monomorphise the argument/return types (into current context)
        for(const auto& arg : fcn.m_args) {
            TRACE_FUNCTION_FR("ARG " << path << " - " << arg.first << ": " << arg.second, "Arg " << arg.first << " : " << cache.m_arg_types.back());
            cache.m_arg_types.push_back( monomorph.monomorph_type(sp, arg.second, false) );
        }
        {
            TRACE_FUNCTION_FR("RET " << path << " - " << fcn.m_return, "Ret " << cache.m_arg_types.back());
            cache.m_arg_types.push_back( monomorph.monomorph_type(sp, fcn.m_return, false) );
        }

        // --- Apply bounds by adding them to the associated type ruleset
        apply_bounds_as_rules(context, sp, *cache.m_fcn_params, monomorph, /*is_impl_level=*/false);

        return true;
    }
    bool visit_call_populate_cache_UfcsInherent(Context& context, const Span& sp, ::HIR::Path& path, ::HIR::ExprCallCache& cache, const ::HIR::Function*& fcn_ptr)
    {
        auto& e = path.m_data.as_UfcsInherent();

        const ::HIR::TypeImpl* impl_ptr = nullptr;
        // Detect multiple applicable methods and get the caller to try again later if there are multiple
        unsigned int count = 0;
        context.m_crate.find_type_impls(e.type, context.m_ivars.callback_resolve_infer(),
            [&](const auto& impl) {
                DEBUG("- impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                auto it = impl.m_methods.find(e.item);
                if( it == impl.m_methods.end() )
                    return false;
                fcn_ptr = &it->second.data;
                impl_ptr = &impl;
                count ++;
                return false;
            });
        if( !fcn_ptr ) {
            ERROR(sp, E0000, "Failed to locate function " << path);
        }
        if( count > 1 ) {
            // Return a status to the caller so it can try again when there may be more information
            return false;
        }
        assert(impl_ptr);
        DEBUG("Found impl" << impl_ptr->m_params.fmt_args() << " " << impl_ptr->m_type);
        fix_param_count(sp, context, e.type, false, path, fcn_ptr->m_params,  e.params);
        cache.m_fcn_params = &fcn_ptr->m_params;


        // If the impl block has parameters, figure out what types they map to
        // - The function params are already mapped (from fix_param_count)
        auto& impl_params = e.impl_params;
        if( impl_ptr->m_params.is_generic() )
        {
            // Default-construct entires in the `impl_params` array
            impl_params.m_types.resize( impl_ptr->m_params.m_types.size() );
            impl_params.m_values.resize( impl_ptr->m_params.m_values.size() );
            OwnedImplMatcher matcher(impl_params);

            auto cmp = impl_ptr->m_type.match_test_generics_fuzz(sp, e.type, context.m_ivars.callback_resolve_infer(), matcher);
            if( cmp == ::HIR::Compare::Fuzzy )
            {
                // If the match was fuzzy, it could be due to a compound being matched against an ivar
                DEBUG("- Fuzzy match, adding ivars and equating");
                for(auto& ty : impl_params.m_types) {
                    if( ty == ::HIR::TypeRef() ) {
                        // Allocate a new ivar for the param
                        ty = context.m_ivars.new_ivar_tr();
                    }
                }


                // Monomorphise the impl type with the new ivars, and equate to e.type
                // TODO: Use a copy of `MonomorphStatePtr` that calls `context.get_type`
                auto impl_monomorph_cb = MonomorphStatePtr(&e.type, &impl_params, nullptr);
                auto impl_ty_mono = impl_monomorph_cb.monomorph_type(sp, impl_ptr->m_type, false);
                DEBUG("- impl_ty_mono = " << impl_ty_mono);

                context.equate_types(sp, impl_ty_mono, e.type);
            }

            // Fill unknown parametrs with ivars
            for(auto& ty : impl_params.m_types) {
                if( ty == ::HIR::TypeRef() ) {
                    // Allocate a new ivar for the param
                    ty = context.m_ivars.new_ivar_tr();
                }
            }
        }

        // Create monomorphise callback
        const auto& fcn_params = e.params;
        // TODO: Use a copy of `MonomorphStatePtr` that calls `context.get_type`
        cache.m_monomorph.reset( new MonomorphStatePtr(&e.type, &impl_params, &fcn_params) );

        // Add trait bounds for all impl and function bounds
        apply_bounds_as_rules(context, sp, impl_ptr->m_params, *cache.m_monomorph, /*is_impl_level=*/true);

        // Equate `Self` and `impl_ptr->m_type` (after monomorph)
        {
            ::HIR::TypeRef tmp;
            const auto& impl_ty_m = cache.m_monomorph->maybe_monomorph_type(sp, tmp, impl_ptr->m_type);

            context.equate_types(sp, e.type, impl_ty_m);
        }

        return true;
    }


    // -----------------------------------------------------------------------
    // IVar generation visitor
    //
    // Iterates the HIR expression tree and adds ivars to all types
    // -----------------------------------------------------------------------
    class ExprVisitor_AddIvars:
        public HIR::ExprVisitorDef
    {
        Context& context;
    public:
        ExprVisitor_AddIvars(Context& context):
            context(context)
        {
        }

        void visit_type(::HIR::TypeRef& ty)
        {
            this->context.add_ivars(ty);
            if(auto* te = ty.data_mut().opt_Path() )
            {
                if( te->path.m_data.is_Generic() )
                {
                    auto& params = te->path.m_data.as_Generic().m_params;
                    const HIR::GenericParams* param_defs = nullptr;
                    TU_MATCH_HDRA( (te->binding), { )
                    TU_ARMA(Struct, pbe)    param_defs = &pbe->m_params;
                    TU_ARMA(Enum, pbe)    param_defs = &pbe->m_params;
                    TU_ARMA(Union, pbe)    param_defs = &pbe->m_params;
                    TU_ARMA(ExternType, pbe) {}
                    TU_ARMA(Opaque, pbe) {}
                    TU_ARMA(Unbound, pbe) {}
                    }
                    if(param_defs)
                    {
                        populate_defaults(Span(), context, MonomorphStatePtr(nullptr, &params, nullptr), *param_defs, params);
                    }
                }
            }
        }
    };

    // -----------------------------------------------------------------------
    // Enumeration visitor
    //
    // Iterates the HIR expression tree and extracts type "equations"
    // -----------------------------------------------------------------------
    class ExprVisitor_Enum:
        public ::HIR::ExprVisitor
    {
        Context& context;
        const ::HIR::TypeRef&   ret_type;
        struct RetTarget {
            const ::HIR::TypeRef*   ret_type;
            const ::HIR::TypeRef*   yield_type;

            RetTarget(const ::HIR::TypeRef& ret_type): ret_type(&ret_type), yield_type(nullptr) {}
            RetTarget(const ::HIR::TypeRef& ret_type, const ::HIR::TypeRef& yield_type): ret_type(&ret_type), yield_type(&yield_type) {}
        };
        ::std::vector<RetTarget>   closure_ret_types;

        ::std::vector<bool> inner_coerce_enabled_stack;

        ::std::vector< ::HIR::ExprNode_Loop*>  loop_blocks;    // Used for `break` type markings

        // TEMP: List of in-scope traits for buildup
        ::HIR::t_trait_list m_traits;
    public:
        ExprVisitor_Enum(Context& context, ::HIR::t_trait_list base_traits, const ::HIR::TypeRef& ret_type):
            context(context),
            ret_type(ret_type),
            m_traits( mv$(base_traits) )
        {
        }

        void visit(::HIR::ExprNode_Block& node) override
        {
            TRACE_FUNCTION_FR(&node << " { ... }", &node << " " << this->context.get_type(node.m_res_type));

            const auto is_diverge = [&](const ::HIR::TypeRef& rty)->bool {
                const auto& ty = this->context.get_type(rty);
                // TODO: Search the entire type for `!`? (What about pointers to it? or Option/Result?)
                // - A correct search will search for unconditional (ignoring enums with a non-! variant) non-rawptr instances of ! in the type
                return ty.data().is_Diverge();
                };

            bool diverges = false;
            this->push_traits( node.m_traits );
            if( node.m_nodes.size() > 0 )
            {
                this->push_inner_coerce(false);
                for( unsigned int i = 0; i < node.m_nodes.size(); i ++ )
                {
                    auto& snp = node.m_nodes[i];
                    this->context.add_ivars( snp->m_res_type );
                    snp->visit(*this);

                    // If this statement yields !, then mark the block as diverging
                    if( is_diverge(snp->m_res_type) ) {
                        diverges = true;
                    }
                    else {
                        struct RevisitDefaultUnit: public Context::Revisitor {
                            HIR::ExprNode* node;
                            RevisitDefaultUnit(HIR::ExprNode* node): node(node) {}
                            const Span& span(void) const { return node->span(); }
                            void fmt(std::ostream& os) const {
                                os << "RevisitDefaultUnit(" << node << ": " << node->m_res_type << ")";
                            }
                            bool revisit(Context& context, bool is_fallback) {
                                DEBUG("is_fallback=" << is_fallback);
                                const auto& ty = context.get_type(node->m_res_type);
                                if(const auto* i = ty.data().opt_Infer()) {
                                    if( i->ty_class != HIR::InferClass::None ) {
                                        // Bounded ivar, remove this rule.
                                        return true;
                                    }
                                    if( is_fallback ) {
                                        context.equate_types(node->span(), ty, HIR::TypeRef::new_unit());
                                        return true;
                                    }
                                    //context.possible_equate_ivar_bounds(node->span(), i->index, make_vec2(ty.clone(), 
                                    return false;
                                }
                                else {
                                    return true;
                                }
                            }
                        };
                        this->context.add_revisit_adv(std::make_unique<RevisitDefaultUnit>(&*snp));
                    }
                }
                this->pop_inner_coerce();
            }

            if( node.m_value_node )
            {
                auto& snp = node.m_value_node;
                DEBUG("Block yields final value");
                this->context.add_ivars( snp->m_res_type );
                this->context.equate_types(snp->span(), node.m_res_type, snp->m_res_type);
                this->context.require_sized(snp->span(), snp->m_res_type);
                snp->visit(*this);
            }
            else if( node.m_nodes.size() > 0 )
            {
                // NOTE: If the final statement in the block diverges, mark this as diverging
                const auto& snp = node.m_nodes.back();
                bool defer = false;
                if( !diverges )
                {
                    if(const auto* e = this->context.get_type(snp->m_res_type).data().opt_Infer())
                    {
                        switch(e->ty_class)
                        {
                        case ::HIR::InferClass::Integer:
                        case ::HIR::InferClass::Float:
                            diverges = false;
                            break;
                        default:
                            defer = true;
                            break;
                        }
                    }
                    else if( is_diverge(snp->m_res_type) ) {
                        diverges = true;
                    }
                    else {
                        diverges = false;
                    }
                }

                // If a statement in this block diverges
                if( defer ) {
                    DEBUG("Block final node returns _, derfer diverge check");
                    this->context.add_revisit(node);
                }
                else if( diverges ) {
                    DEBUG("Block diverges, yield !");
                    this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_diverge());
                }
                else {
                    DEBUG("Block doesn't diverge but doesn't yield a value, yield ()");
                    this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
                }
            }
            else
            {
                // Result should be `()`
                DEBUG("Block is empty, yield ()");
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
            }
            this->pop_traits( node.m_traits );
        }
        void visit(::HIR::ExprNode_Asm& node) override
        {
            TRACE_FUNCTION_F(&node << " asm! ...");

            this->push_inner_coerce( false );
            for(auto& v : node.m_outputs)
            {
                this->context.add_ivars( v.value->m_res_type );
                v.value->visit(*this);
            }
            for(auto& v : node.m_inputs)
            {
                this->context.add_ivars( v.value->m_res_type );
                v.value->visit(*this);
            }
            this->pop_inner_coerce();
            // TODO: Revisit to check that the input are integers, and the outputs are integer lvalues
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
        }
        void visit(::HIR::ExprNode_Asm2& node) override
        {
            TRACE_FUNCTION_F(&node << " asm! ...");

            this->push_inner_coerce( false );
            for(auto& v : node.m_params)
            {
                TU_MATCH_HDRA( (v), { )
                TU_ARMA(Const, e) {
                    this->context.add_ivars( e->m_res_type );
                    visit_node_ptr(e);
                    }
                TU_ARMA(Sym, e) {
                    }
                TU_ARMA(RegSingle, e) {
                    this->context.add_ivars( e.val->m_res_type );
                    visit_node_ptr(e.val);
                    }
                TU_ARMA(Reg, e) {
                    if(e.val_in) {
                        this->context.add_ivars( e.val_in->m_res_type );
                        visit_node_ptr(e.val_in);
                    }
                    if(e.val_out) {
                        this->context.add_ivars( e.val_out->m_res_type );
                        visit_node_ptr(e.val_out);
                    }
                    }
                }
            }
            this->pop_inner_coerce();
            // TODO: Revisit to check that the input are integers, and the outputs are integer lvalues
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
        }

        void visit(::HIR::ExprNode_Return& node) override
        {
            TRACE_FUNCTION_F(&node << " return ...");
            this->context.add_ivars( node.m_value->m_res_type );

            const auto& ret_ty = ( this->closure_ret_types.size() > 0 ? *this->closure_ret_types.back().ret_type : this->ret_type );
            this->context.equate_types_coerce(node.span(), ret_ty, node.m_value);

            this->push_inner_coerce( true );
            node.m_value->visit( *this );
            this->pop_inner_coerce();
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_diverge());
        }
        void visit(::HIR::ExprNode_Yield& node) override
        {
            TRACE_FUNCTION_F(&node << " yield ...");
            this->context.add_ivars( node.m_value->m_res_type );

            if( this->closure_ret_types.empty() || this->closure_ret_types.back().yield_type == nullptr )
                ERROR(node.span(), E0000, "`yield` outside a generator closure");
            const auto& ret_ty = *this->closure_ret_types.back().yield_type;
            this->context.equate_types_coerce(node.span(), ret_ty, node.m_value);

            this->push_inner_coerce( true );
            node.m_value->visit( *this );
            this->pop_inner_coerce();
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
        }

        void visit(::HIR::ExprNode_Loop& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
            TRACE_FUNCTION_F(&node << " loop ('" << node.m_label << ") { ... }");
            // Push this node to a stack so `break` statements can update the yeilded value
            this->loop_blocks.push_back( &node );
            node.m_diverges = true;    // Set to `false` if a break is hit

            this->context.add_ivars(node.m_code->m_res_type);
            this->context.equate_types(node.span(), node.m_code->m_res_type, ::HIR::TypeRef::new_unit());
            node.m_code->visit( *this );

            this->loop_blocks.pop_back( );

            if( node.m_diverges ) {
                // NOTE: This doesn't set the ivar to !, but marks it as a ! ivar (similar to the int/float markers)
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_diverge());
                DEBUG("Loop diverged");
            }
        }
        void visit(::HIR::ExprNode_LoopControl& node) override
        {
            TRACE_FUNCTION_F(&node << " " << (node.m_continue ? "continue" : "break") << " '" << node.m_label);
            // Break types
            if( !node.m_continue )
            {
                ::HIR::ExprNode_Loop*   loop_node_ptr;
                if( node.m_label != "" )
                {
                    auto it = ::std::find_if(this->loop_blocks.rbegin(), this->loop_blocks.rend(), [&](const auto& np){ return np->m_label == node.m_label; });
                    if( it == this->loop_blocks.rend() ) {
                        ERROR(node.span(), E0000, "Could not find loop '" << node.m_label << " for break");
                    }
                    loop_node_ptr = &**it;
                }
                else
                {
                    loop_node_ptr = nullptr;
                    for(auto it = this->loop_blocks.rbegin(); it != this->loop_blocks.rend(); ++it)
                    {
                        if( !(*it)->m_require_label )
                        {
                            loop_node_ptr = *it;
                            break;
                        }
                    }
                    if( !loop_node_ptr ) {
                        ERROR(node.span(), E0000, "Break statement with no acive loop");
                    }
                }


                DEBUG("Break out of loop " << loop_node_ptr);
                auto& loop_node = *loop_node_ptr;
                loop_node.m_diverges = false;

                if( node.m_value ) {
                    this->context.add_ivars(node.m_value->m_res_type);
                    node.m_value->visit(*this);
                    this->context.equate_types(node.span(), loop_node.m_res_type, node.m_value->m_res_type);
                    this->context.require_sized(node.span(), node.m_value->m_res_type);
                }
                else {
                    this->context.equate_types(node.span(), loop_node.m_res_type, ::HIR::TypeRef::new_unit());
                }
            }
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_diverge());
        }

        void visit(::HIR::ExprNode_Let& node) override
        {
            TRACE_FUNCTION_F(&node << " let " << node.m_pattern << ": " << node.m_type);

            this->context.add_ivars( node.m_type );
            this->context.handle_pattern(node.span(), node.m_pattern, node.m_type);

            if( node.m_value )
            {
                this->context.add_ivars( node.m_value->m_res_type );
                // If the type was omitted or was just `_`, equate
                if( node.m_type.data().is_Infer() ) {
                    this->context.equate_types( node.span(), node.m_type, node.m_value->m_res_type );
                    this->push_inner_coerce(true);
                }
                // otherwise coercions apply
                else {
                    this->context.equate_types_coerce( node.span(), node.m_type, node.m_value );
                    this->push_inner_coerce(true);
                }

                node.m_value->visit( *this );
                this->context.require_sized(node.span(), node.m_value->m_res_type);
                this->pop_inner_coerce();
            }
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
        }
        void visit(::HIR::ExprNode_Match& node) override
        {
            TRACE_FUNCTION_F(&node << " match ...");

            auto val_type = this->context.m_ivars.new_ivar_tr();

            {
                auto _ = this->push_inner_coerce_scoped(true);
                this->context.add_ivars(node.m_value->m_res_type);

                node.m_value->visit( *this );
                // TODO: If a coercion point (and ivar for the value) is placed here, it will allow `match &string { "..." ... }`
                // - But, this can break some parts of inferrence
                this->context.equate_types( node.span(), val_type, node.m_value->m_res_type );
                //this->context.equate_types_coerce( node.span(), val_type, node.m_value );
            }

            for(auto& arm : node.m_arms)
            {
                TRACE_FUNCTION_F("ARM " << arm.m_patterns);
                for(auto& pat : arm.m_patterns)
                {
                    this->context.handle_pattern(node.span(), pat, val_type);
                }

                if( arm.m_cond )
                {
                    auto _ = this->push_inner_coerce_scoped(false);
                    this->context.add_ivars( arm.m_cond->m_res_type );
                    this->context.equate_types(arm.m_cond->span(), ::HIR::TypeRef(::HIR::CoreType::Bool), arm.m_cond->m_res_type);
                    arm.m_cond->visit( *this );
                }

                this->context.add_ivars( arm.m_code->m_res_type );
                this->context.equate_types_coerce(node.span(), node.m_res_type, arm.m_code);
                arm.m_code->visit( *this );
            }

            if( node.m_arms.empty() ) {
                DEBUG("Empty match");
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_diverge());
            }
        }

        void visit(::HIR::ExprNode_If& node) override
        {
            TRACE_FUNCTION_F(&node << " if ...");

            this->context.add_ivars( node.m_cond->m_res_type );

            {
                auto _ = this->push_inner_coerce_scoped(false);
                this->context.equate_types(node.m_cond->span(), ::HIR::TypeRef(::HIR::CoreType::Bool), node.m_cond->m_res_type);
                node.m_cond->visit( *this );
            }

            this->context.add_ivars( node.m_true->m_res_type );
            if( node.m_false ) {
                this->context.equate_types_coerce(node.span(), node.m_res_type, node.m_true);
            }
            else {
                this->context.equate_types(node.span(), node.m_true->m_res_type, ::HIR::TypeRef::new_unit());
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
            }
            node.m_true->visit( *this );

            if( node.m_false ) {
                this->context.add_ivars( node.m_false->m_res_type );
                this->context.equate_types_coerce(node.span(), node.m_res_type, node.m_false);
                node.m_false->visit( *this );
            }
            else {
            }
        }


        void visit(::HIR::ExprNode_Assign& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);

            TRACE_FUNCTION_F(&node << "... = ...");
            this->context.add_ivars( node.m_slot ->m_res_type );
            this->context.add_ivars( node.m_value->m_res_type );

            // Plain assignment can't be overloaded, requires equal types
            if( node.m_op == ::HIR::ExprNode_Assign::Op::None ) {
                this->context.equate_types_coerce(node.span(), node.m_slot->m_res_type, node.m_value);
            }
            else {
                // Type inferrence using the +=
                // - "" as type name to indicate that it's just using the trait magic?
                const char *lang_item = nullptr;
                switch( node.m_op )
                {
                case ::HIR::ExprNode_Assign::Op::None:  throw "";
                case ::HIR::ExprNode_Assign::Op::Add: lang_item = "add_assign"; break;
                case ::HIR::ExprNode_Assign::Op::Sub: lang_item = "sub_assign"; break;
                case ::HIR::ExprNode_Assign::Op::Mul: lang_item = "mul_assign"; break;
                case ::HIR::ExprNode_Assign::Op::Div: lang_item = "div_assign"; break;
                case ::HIR::ExprNode_Assign::Op::Mod: lang_item = "rem_assign"; break;
                case ::HIR::ExprNode_Assign::Op::And: lang_item = "bitand_assign"; break;
                case ::HIR::ExprNode_Assign::Op::Or : lang_item = "bitor_assign" ; break;
                case ::HIR::ExprNode_Assign::Op::Xor: lang_item = "bitxor_assign"; break;
                case ::HIR::ExprNode_Assign::Op::Shr: lang_item = "shr_assign"; break;
                case ::HIR::ExprNode_Assign::Op::Shl: lang_item = "shl_assign"; break;
                }
                assert(lang_item);
                const auto& trait_path = this->context.m_crate.get_lang_item_path(node.span(), lang_item);

                auto ty = this->context.m_ivars.new_ivar_tr();
                this->context.equate_types_coerce(node.span(), ty, node.m_value);
                this->context.equate_types_assoc(node.span(), ::HIR::TypeRef(), trait_path, ::make_vec1(mv$(ty)),  node.m_slot->m_res_type.clone(), "");
            }

            node.m_slot->visit( *this );

            auto _2 = this->push_inner_coerce_scoped( node.m_op == ::HIR::ExprNode_Assign::Op::None );
            node.m_value->visit( *this );
            this->context.require_sized(node.span(), node.m_value->m_res_type);

            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
        }
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);

            TRACE_FUNCTION_F(&node << "... "<<::HIR::ExprNode_BinOp::opname(node.m_op)<<" ...");

            this->context.add_ivars( node.m_left ->m_res_type );
            this->context.add_ivars( node.m_right->m_res_type );

            const auto& left_ty = node.m_left ->m_res_type;
            ::HIR::TypeRef  right_ty_inner = this->context.m_ivars.new_ivar_tr();
            const auto& right_ty = right_ty_inner;//node.m_right->m_res_type;
            this->context.equate_types_coerce(node.span(), right_ty_inner, node.m_right);

            switch(node.m_op)
            {
            case ::HIR::ExprNode_BinOp::Op::CmpEqu:
            case ::HIR::ExprNode_BinOp::Op::CmpNEqu:
            case ::HIR::ExprNode_BinOp::Op::CmpLt:
            case ::HIR::ExprNode_BinOp::Op::CmpLtE:
            case ::HIR::ExprNode_BinOp::Op::CmpGt:
            case ::HIR::ExprNode_BinOp::Op::CmpGtE: {
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef(::HIR::CoreType::Bool));

                const char* item_name = nullptr;
                switch(node.m_op)
                {
                case ::HIR::ExprNode_BinOp::Op::CmpEqu:  item_name = "eq";  break;
                case ::HIR::ExprNode_BinOp::Op::CmpNEqu: item_name = "eq";  break;
                case ::HIR::ExprNode_BinOp::Op::CmpLt:   item_name = TARGETVER_LEAST_1_29 ? "partial_ord" : "ord"; break;
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:  item_name = TARGETVER_LEAST_1_29 ? "partial_ord" : "ord"; break;
                case ::HIR::ExprNode_BinOp::Op::CmpGt:   item_name = TARGETVER_LEAST_1_29 ? "partial_ord" : "ord"; break;
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:  item_name = TARGETVER_LEAST_1_29 ? "partial_ord" : "ord"; break;
                default: break;
                }
                assert(item_name);
                const auto& op_trait = this->context.m_crate.get_lang_item_path(node.span(), item_name);

                this->context.equate_types_assoc(node.span(), ::HIR::TypeRef(),  op_trait, ::make_vec1(right_ty.clone()), left_ty.clone(), "");
                break; }

            case ::HIR::ExprNode_BinOp::Op::BoolAnd:
            case ::HIR::ExprNode_BinOp::Op::BoolOr:
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef(::HIR::CoreType::Bool));
                this->context.equate_types(node.span(), left_ty , ::HIR::TypeRef(::HIR::CoreType::Bool));
                this->context.equate_types(node.span(), right_ty, ::HIR::TypeRef(::HIR::CoreType::Bool));
                break;
            default: {
                const char* item_name = nullptr;
                switch(node.m_op)
                {
                case ::HIR::ExprNode_BinOp::Op::CmpEqu:  throw "";
                case ::HIR::ExprNode_BinOp::Op::CmpNEqu: throw "";
                case ::HIR::ExprNode_BinOp::Op::CmpLt:   throw "";
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:  throw "";
                case ::HIR::ExprNode_BinOp::Op::CmpGt:   throw "";
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:  throw "";
                case ::HIR::ExprNode_BinOp::Op::BoolAnd: throw "";
                case ::HIR::ExprNode_BinOp::Op::BoolOr:  throw "";

                case ::HIR::ExprNode_BinOp::Op::Add: item_name = "add"; break;
                case ::HIR::ExprNode_BinOp::Op::Sub: item_name = "sub"; break;
                case ::HIR::ExprNode_BinOp::Op::Mul: item_name = "mul"; break;
                case ::HIR::ExprNode_BinOp::Op::Div: item_name = "div"; break;
                case ::HIR::ExprNode_BinOp::Op::Mod: item_name = "rem"; break;

                case ::HIR::ExprNode_BinOp::Op::And: item_name = "bitand"; break;
                case ::HIR::ExprNode_BinOp::Op::Or:  item_name = "bitor";  break;
                case ::HIR::ExprNode_BinOp::Op::Xor: item_name = "bitxor"; break;

                case ::HIR::ExprNode_BinOp::Op::Shr: item_name = "shr"; break;
                case ::HIR::ExprNode_BinOp::Op::Shl: item_name = "shl"; break;
                }
                assert(item_name);
                const auto& op_trait = this->context.m_crate.get_lang_item_path(node.span(), item_name);

                // NOTE: `true` marks the association as coming from a binary operation, which changes integer handling
                this->context.equate_types_assoc(node.span(), node.m_res_type,  op_trait, ::make_vec1(right_ty.clone()), left_ty.clone(), "Output", true);
                break; }
            }
            node.m_left ->visit( *this );
            auto _2 = this->push_inner_coerce_scoped(true);
            node.m_right->visit( *this );
        }
        void visit(::HIR::ExprNode_UniOp& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);

            TRACE_FUNCTION_F(&node << " " << ::HIR::ExprNode_UniOp::opname(node.m_op) << "...");
            this->context.add_ivars( node.m_value->m_res_type );
            const char* item_name = nullptr;
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Invert:
                item_name = "not";
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                item_name = "neg";
                break;
            }
            assert(item_name);
            const auto& op_trait = this->context.m_crate.get_lang_item_path(node.span(), item_name);
            this->context.equate_types_assoc(node.span(), node.m_res_type,  op_trait, ::HIR::PathParams {}, node.m_value->m_res_type.clone(), "Output", true);
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_Borrow& node) override
        {
            TRACE_FUNCTION_F(&node << " &_ ...");
            this->context.add_ivars( node.m_value->m_res_type );

            // TODO: Can Ref/RefMut trigger coercions?
            this->context.equate_types( node.span(), node.m_res_type,  ::HIR::TypeRef::new_borrow(node.m_type, node.m_value->m_res_type.clone()) );

            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_RawBorrow& node) override
        {
            TRACE_FUNCTION_F(&node << " &raw _ ...");
            this->context.add_ivars( node.m_value->m_res_type );

            this->context.equate_types( node.span(), node.m_res_type,  ::HIR::TypeRef::new_pointer(node.m_type, node.m_value->m_res_type.clone()) );

            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_Cast& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
            this->context.add_ivars(node.m_dst_type);

            TRACE_FUNCTION_F(&node << " ... as " << node.m_dst_type);

            node.m_value->visit( *this );

            this->context.equate_types( node.span(), node.m_res_type,  node.m_dst_type );
            // TODO: Only revisit if the cast type requires inferring.
            this->context.add_revisit(node);
        }
        void visit(::HIR::ExprNode_Unsize& node) override
        {
            // _Unsize is emitted for type annotations, and adds a coercion point to its inner
            this->context.add_ivars(node.m_dst_type);
            node.m_value->visit( *this );

            this->context.equate_types_coerce(node.m_value->span(), node.m_dst_type, node.m_value);
            this->context.equate_types( node.span(), node.m_res_type,  node.m_dst_type );
        }
        void visit(::HIR::ExprNode_Index& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);

            TRACE_FUNCTION_F(&node << " ... [ ... ]");
            this->context.add_ivars( node.m_value->m_res_type );
            node.m_cache.index_ty = this->context.m_ivars.new_ivar_tr();
            this->context.add_ivars( node.m_index->m_res_type );

            node.m_value->visit( *this );
            node.m_index->visit( *this );
            this->context.equate_types_coerce(node.m_index->span(), node.m_cache.index_ty, node.m_index);

            this->context.add_revisit(node);
        }
        void visit(::HIR::ExprNode_Deref& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);

            TRACE_FUNCTION_F(&node << " *...");
            this->context.add_ivars( node.m_value->m_res_type );

            node.m_value->visit( *this );

            this->context.add_revisit(node);
        }
        void visit(::HIR::ExprNode_Emplace& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
            TRACE_FUNCTION_F(&node << " ... <- ... ");
            this->context.add_ivars( node.m_place->m_res_type );
            this->context.add_ivars( node.m_value->m_res_type );

            node.m_place->visit( *this );
            auto _2 = this->push_inner_coerce_scoped(true);
            node.m_value->visit( *this );

            this->context.add_revisit(node);
        }

        void add_ivars_generic_path(const Span& sp, ::HIR::GenericPath& gp) {
            for(auto& ty : gp.m_params.m_types)
                this->context.add_ivars(ty);
        }
        void add_ivars_path(const Span& sp, ::HIR::Path& path) {
            TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
            (Generic,
                this->add_ivars_generic_path(sp, e);
                ),
            (UfcsKnown,
                this->context.add_ivars(e.type);
                this->add_ivars_generic_path(sp, e.trait);
                for(auto& ty : e.params.m_types)
                    this->context.add_ivars(ty);
                ),
            (UfcsUnknown,
                TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
                ),
            (UfcsInherent,
                this->context.add_ivars(e.type);
                for(auto& ty : e.params.m_types)
                    this->context.add_ivars(ty);
                )
            )
        }

        ::HIR::TypeRef get_structenum_ty(const Span& sp, bool is_struct, ::HIR::GenericPath& gp)
        {
            if( is_struct )
            {
                const auto& e = this->context.m_crate.get_typeitem_by_path(sp, gp.m_path);
                if( e.is_Struct() ) {
                    const auto& str = e.as_Struct();
                    fix_param_count(sp, this->context, ::HIR::TypeRef(), false, gp, str.m_params, gp.m_params);

                    return ::HIR::TypeRef::new_path( gp.clone(), ::HIR::TypePathBinding::make_Struct(&str) );
                }
                else if( e.is_Union() ) {
                    const auto& u = e.as_Union();
                    fix_param_count(sp, this->context, ::HIR::TypeRef(), false, gp, u.m_params, gp.m_params);

                    return ::HIR::TypeRef::new_path( gp.clone(), ::HIR::TypePathBinding::make_Union(&u) );
                }
                else {
                    BUG(sp, "Path " << gp << " doesn't refer to a struct/union");
                }
            }
            else
            {
                auto s_path = get_parent_path(gp.m_path);

                const auto& enm = this->context.m_crate.get_enum_by_path(sp, s_path);
                fix_param_count(sp, this->context, ::HIR::TypeRef(), false, gp, enm.m_params, gp.m_params);

                return ::HIR::TypeRef::new_path( ::HIR::GenericPath(mv$(s_path), gp.m_params.clone()), ::HIR::TypePathBinding::make_Enum(&enm) );
            }
        }

        void visit(::HIR::ExprNode_TupleVariant& node) override
        {
            const auto& sp = node.span();
            TRACE_FUNCTION_F(&node << " " << node.m_path << "(...) [" << (node.m_is_struct ? "struct" : "enum") << "]");
            for( auto& val : node.m_args ) {
                this->context.add_ivars( val->m_res_type );
            }
            this->context.m_ivars.add_ivars_params(node.m_path.m_params);

            // - Create ivars in path, and set result type
            const auto ty = this->get_structenum_ty(node.span(), node.m_is_struct, node.m_path);
            this->context.equate_types(node.span(), node.m_res_type, ty);

            const ::HIR::t_tuple_fields* fields_ptr = nullptr;
            TU_MATCH_HDRA( (ty.data().as_Path().binding), {)
            TU_ARMA(Unbound, e) {}
            TU_ARMA(Opaque, e) {}
            TU_ARMA(Enum, e) {
                const auto& var_name = node.m_path.m_path.m_components.back();
                const auto& enm = *e;
                size_t idx = enm.find_variant(var_name);
                const auto& var_ty = enm.m_data.as_Data()[idx].type;
                const auto& str = *var_ty.data().as_Path().binding.as_Struct();
                ASSERT_BUG(sp, str.m_data.is_Tuple(), "Pointed variant of TupleVariant (" << node.m_path << ") isn't a Tuple");
                fields_ptr = &str.m_data.as_Tuple();
                }
            TU_ARMA(Struct, e) {
                ASSERT_BUG(sp, e->m_data.is_Tuple(), "Pointed struct in TupleVariant (" << node.m_path << ") isn't a Tuple");
                fields_ptr = &e->m_data.as_Tuple();
                }
            TU_ARMA(Union, e) {
                BUG(sp, "TupleVariant pointing to a union");
                }
            TU_ARMA(ExternType, e) {
                BUG(sp, "TupleVariant pointing to a extern type");
                }
            }
            assert(fields_ptr);
            const ::HIR::t_tuple_fields& fields = *fields_ptr;
            if( fields.size() != node.m_args.size() ) {
                ERROR(node.span(), E0000, "Tuple variant constructor argument count doesn't match type - " << node.m_path);
            }

            auto monomorph_cb = MonomorphStatePtr(&ty, &node.m_path.m_params, nullptr);

            // Bind fields with type params (coercable)
            node.m_arg_types.resize( node.m_args.size() );
            for( unsigned int i = 0; i < node.m_args.size(); i ++ )
            {
                const auto& des_ty_r = fields[i].ent;
                const auto* des_ty = &des_ty_r;
                if( monomorphise_type_needed(des_ty_r) ) {
                    node.m_arg_types[i] = monomorph_cb.monomorph_type(sp, des_ty_r);
                    des_ty = &node.m_arg_types[i];
                }

                this->context.equate_types_coerce(node.span(), *des_ty,  node.m_args[i]);
            }

            auto _ = this->push_inner_coerce_scoped(true);
            for( auto& val : node.m_args ) {
                val->visit( *this );
                this->context.require_sized(node.span(), val->m_res_type);
            }
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override
        {
            const auto& sp = node.span();
            TRACE_FUNCTION_F(&node << " " << node.m_type << "{...} [" << (node.m_is_struct ? "struct" : "enum") << "]");
            auto _ = this->push_inner_coerce_scoped(true);

            this->context.add_ivars(node.m_type);

            for( auto& val : node.m_values ) {
                this->context.add_ivars( val.second->m_res_type );
            }
            if( node.m_base_value ) {
                this->context.add_ivars( node.m_base_value->m_res_type );
            }

            auto t = this->context.m_resolve.expand_associated_types(sp, mv$(node.m_type));
            node.m_type = HIR::TypeRef();
            if( node.m_is_struct )
            {
                ASSERT_BUG(sp, TU_TEST1(t.data(), Path, .path.m_data.is_Generic()), "Struct literal with non-Generic path - " << t);
                node.m_real_path = t.data().as_Path().path.m_data.as_Generic().clone();
            }
            else
            {
                ASSERT_BUG(sp, TU_TEST1(t.data(), Path, .path.m_data.is_UfcsInherent()), "Enum struct literal with non-UfcsInherent path - " << t);
                auto& it = t.data().as_Path().path.m_data.as_UfcsInherent().type;
                auto& name = t.data().as_Path().path.m_data.as_UfcsInherent().item;
                ASSERT_BUG(sp, TU_TEST1(it.data(), Path, .path.m_data.is_Generic()), "Struct literal with non-Generic path - " << t);
                node.m_real_path = it.data().as_Path().path.m_data.as_Generic().clone();
                node.m_real_path.m_path.m_components.push_back( name );
            }
            auto& ty_path = node.m_real_path;

            // - Create ivars in path, and set result type
            const auto ty = this->get_structenum_ty(node.span(), node.m_is_struct, ty_path);
            this->context.equate_types(node.span(), node.m_res_type, ty);
            if( node.m_base_value ) {
                this->context.equate_types(node.span(), node.m_base_value->m_res_type, ty);
            }

            const ::HIR::t_struct_fields* fields_ptr = nullptr;
            const ::HIR::GenericParams* generics = nullptr;
            TU_MATCH_HDRA( (ty.data().as_Path().binding), {)
            TU_ARMA(Unbound, e) {}
            TU_ARMA(Opaque, e) {}
            TU_ARMA(ExternType, e) {}   // Error?
            TU_ARMA(Enum, e) {
                const auto& var_name = ty_path.m_path.m_components.back();
                const auto& enm = *e;
                auto idx = enm.find_variant(var_name);
                ASSERT_BUG(sp, idx != SIZE_MAX, "");
                ASSERT_BUG(sp, enm.m_data.is_Data(), "");
                const auto& var = enm.m_data.as_Data()[idx];
                if( var.type == ::HIR::TypeRef::new_unit() ) {
                    ASSERT_BUG(node.span(), node.m_values.size() == 0, "Values provided for unit-like variant");
                    ASSERT_BUG(node.span(), ! node.m_base_value, "Values provided for unit-like variant");
                    return ;
                }
                const auto& str = *var.type.data().as_Path().binding.as_Struct();
                /*
                if( it->second.is_Unit() || it->second.is_Value() || it->second.is_Tuple() ) {
                }
                */
                ASSERT_BUG(sp, var.is_struct, "Struct literal for enum on non-struct variant");
                fields_ptr = &str.m_data.as_Named();
                generics = &enm.m_params;
                }
            TU_ARMA(Union, e) {
                fields_ptr = &e->m_variants;
                generics = &e->m_params;
                // Errors are done here, as from_ast may not know yet
                if(node.m_base_value)
                    ERROR(node.span(), E0000, "Union can't have a base value");
                ASSERT_BUG(node.span(), node.m_values.size() > 0, "Union literal with no values");
                ASSERT_BUG(node.span(), node.m_values.size() == 1, "Union literal with multiple values");
                }
            TU_ARMA(Struct, e) {
                if( e->m_data.is_Unit() || e->m_data.is_Tuple() )
                {
                    ASSERT_BUG(node.span(), node.m_values.size() == 0, "Values provided for unit-like struct");

                    if( node.m_base_value ) {
                        auto _ = this->push_inner_coerce_scoped(false);
                        node.m_base_value->visit( *this );
                    }
                    return ;
                }

                ASSERT_BUG(node.span(), e->m_data.is_Named(), "StructLiteral not pointing to a braced struct, instead " << e->m_data.tag_str() << " - " << ty);
                fields_ptr = &e->m_data.as_Named();
                generics = &e->m_params;
                }
            }
            ASSERT_BUG(node.span(), fields_ptr, "");
            assert(generics);
            const ::HIR::t_struct_fields& fields = *fields_ptr;

            auto monomorph_cb = MonomorphStatePtr(&ty, &ty_path.m_params, nullptr);

            node.m_value_types.resize( fields.size() );

            // Bind fields with type params (coercable)
            for( auto& val : node.m_values)
            {
                const auto& name = val.first;
                auto it = ::std::find_if(fields.begin(), fields.end(), [&](const auto& v)->bool{ return v.first == name; });
                ASSERT_BUG(node.span(), it != fields.end(), "Field '" << name << "' not found in struct " << ty_path);
                const auto& des_ty_r = it->second.ent;
                auto& des_ty_cache = node.m_value_types[it - fields.begin()];
                const auto* des_ty = &des_ty_r;

                DEBUG(name << " : " << des_ty_r);
                if( monomorphise_type_needed(des_ty_r) ) {
                    if( des_ty_cache == ::HIR::TypeRef() ) {
                        des_ty_cache = monomorph_cb.monomorph_type(node.span(), des_ty_r);
                    }
                    else {
                        // TODO: Is it an error when it's already populated?
                    }
                    des_ty = &des_ty_cache;
                }
                this->context.equate_types_coerce(node.span(), *des_ty,  val.second);
            }

            // Convert bounds on the type into rules
            apply_bounds_as_rules(context, node.span(), *generics, monomorph_cb, /*is_impl_level=*/true);

            for( auto& val : node.m_values ) {
                val.second->visit( *this );
                this->context.require_sized(node.span(), val.second->m_res_type);
            }
            if( node.m_base_value ) {
                auto _ = this->push_inner_coerce_scoped(false);
                node.m_base_value->visit( *this );
            }
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override
        {
            TRACE_FUNCTION_F(&node << " " << node.m_path << " [" << (node.m_is_struct ? "struct" : "enum") << "]");

            // TODO: Check?

            // - Create ivars in path, and set result type
            const auto ty = this->get_structenum_ty(node.span(), node.m_is_struct, node.m_path);
            this->context.equate_types(node.span(), node.m_res_type, ty);
        }

        void visit(::HIR::ExprNode_CallPath& node) override
        {
            this->visit_path(node.span(), node.m_path);
            TRACE_FUNCTION_F(&node << " " << node.m_path << "(...)");
            for( auto& val : node.m_args ) {
                this->context.add_ivars( val->m_res_type );
            }

            // Populate cache
            {
                if( !visit_call_populate_cache(this->context, node.span(), node.m_path, node.m_cache) ) {
                    TODO(node.span(), "Emit revisit when _CallPath is ambiguous - " << node.m_path);
                }
                assert( node.m_cache.m_arg_types.size() >= 1);
                unsigned int exp_argc = node.m_cache.m_arg_types.size() - 1;

                if( node.m_args.size() != exp_argc ) {
                    if( node.m_cache.m_fcn->m_variadic && node.m_args.size() > exp_argc ) {
                    }
                    else {
                        ERROR(node.span(), E0000, "Incorrect number of arguments to " << node.m_path
                            << " - exp " << exp_argc << " got " << node.m_args.size());
                    }
                }
            }


            // TODO: Figure out a way to disable coercions in desugared for loops (will speed up typecheck)

            // Link arguments
            // - NOTE: Uses the cache for the count because vaargs aren't checked (they're checked for suitability in expr_check.cpp)
            for(unsigned int i = 0; i < node.m_cache.m_arg_types.size() - 1; i ++)
            {
                this->context.equate_types_coerce(node.span(), node.m_cache.m_arg_types[i], node.m_args[i]);
            }
            this->context.equate_types(node.span(), node.m_res_type,  node.m_cache.m_arg_types.back());

            auto _ = this->push_inner_coerce_scoped(true);
            for( auto& val : node.m_args ) {
                val->visit( *this );
                this->context.require_sized(node.span(), val->m_res_type);
            }
            this->context.require_sized(node.span(), node.m_res_type);
        }
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            TRACE_FUNCTION_F(&node << " ...(...)");
            this->context.add_ivars( node.m_value->m_res_type );
            // Add ivars to node result types and create fresh ivars for coercion targets
            for( auto& val : node.m_args ) {
                this->context.add_ivars( val->m_res_type );
                node.m_arg_ivars.push_back( this->context.m_ivars.new_ivar_tr() );
            }

            {
                auto _ = this->push_inner_coerce_scoped(false);
                node.m_value->visit( *this );
            }
            auto _ = this->push_inner_coerce_scoped(true);
            for(unsigned int i = 0; i < node.m_args.size(); i ++ )
            {
                auto& val = node.m_args[i];
                this->context.equate_types_coerce(val->span(), node.m_arg_ivars[i],  val);
                val->visit( *this );
                this->context.require_sized(node.span(), val->m_res_type);
            }
            this->context.require_sized(node.span(), node.m_res_type);

            // Nothing can be done until type is known
            this->context.add_revisit(node);
        }
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            TRACE_FUNCTION_F(&node << " (...)."<<node.m_method<<"(...)");
            this->context.add_ivars( node.m_value->m_res_type );
            for( auto& val : node.m_args ) {
                this->context.add_ivars( val->m_res_type );
            }
            for( auto& ty : node.m_params.m_types ) {
                this->context.add_ivars( ty );
            }

            // - Search in-scope trait list for traits that provide a method of this name
            const RcString& method_name = node.m_method;
            ::HIR::t_trait_list    possible_traits;
            unsigned int max_num_params = 0;
            for(const auto& trait_ref : ::reverse(m_traits))
            {
                if( trait_ref.first == nullptr )
                    break;

                // TODO: Search supertraits too
                auto it = trait_ref.second->m_values.find(method_name);
                if( it == trait_ref.second->m_values.end() )
                    continue ;
                if( !it->second.is_Function() )
                    continue ;

                if( ::std::none_of( possible_traits.begin(), possible_traits.end(), [&](const auto&x){return x.second == trait_ref.second;}) ) {
                    possible_traits.push_back( trait_ref );
                    if( trait_ref.second->m_params.m_types.size() > max_num_params )
                        max_num_params = trait_ref.second->m_params.m_types.size();
                }
            }
            //  > Store the possible set of traits for later
            node.m_traits = mv$(possible_traits);
            for(unsigned int i = 0; i < max_num_params; i ++)
            {
                node.m_trait_param_ivars.push_back( this->context.m_ivars.new_ivar() );
            }

            {
                auto _ = this->push_inner_coerce_scoped(false);
                node.m_value->visit( *this );
            }
            auto _ = this->push_inner_coerce_scoped(true);
            for( auto& val : node.m_args ) {
                val->visit( *this );
                this->context.require_sized(node.span(), val->m_res_type);
            }
            this->context.require_sized(node.span(), node.m_res_type);

            // Resolution can't be done until lefthand type is known.
            // > Has to be done during iteraton
            this->context.add_revisit( node );
        }
        void visit(::HIR::ExprNode_Field& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
            TRACE_FUNCTION_F(&node << " (...)."<<node.m_field);
            this->context.add_ivars( node.m_value->m_res_type );

            node.m_value->visit( *this );

            this->context.add_revisit( node );
        }

        void visit(::HIR::ExprNode_Tuple& node) override
        {
            TRACE_FUNCTION_F(&node << " (...,)");
            for( auto& val : node.m_vals ) {
                this->context.add_ivars( val->m_res_type );
            }

            if( can_coerce_inner_result() )
            {
                DEBUG("Tuple inner coerce");
                const auto& ty = this->context.get_type(node.m_res_type);
                if( const auto* e = ty.data().opt_Tuple() )
                {
                    if( e->size() != node.m_vals.size() ) {
                        ERROR(node.span(), E0000, "Tuple literal node count mismatches with return type");
                    }
                }
                else if( ty.data().is_Infer() )
                {
                    ::std::vector< ::HIR::TypeRef>  tuple_tys;
                    for(const auto& val : node.m_vals ) {
                        (void)val;
                        tuple_tys.push_back( this->context.m_ivars.new_ivar_tr() );
                    }
                    this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef(mv$(tuple_tys)));
                }
                else {
                    // mismatch
                    ERROR(node.span(), E0000, "Tuple literal used where a non-tuple expected - " << ty);
                }
                const auto& inner_tys = this->context.get_type(node.m_res_type).data().as_Tuple();
                assert( inner_tys.size() == node.m_vals.size() );

                for(unsigned int i = 0; i < inner_tys.size(); i ++)
                {
                    this->context.equate_types_coerce(node.span(), inner_tys[i], node.m_vals[i]);
                }
            }
            else
            {
                // No inner coerce, just equate the return type.
                ::std::vector< ::HIR::TypeRef>  tuple_tys;
                for(const auto& val : node.m_vals ) {
                    tuple_tys.push_back( val->m_res_type.clone() );
                }
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef(mv$(tuple_tys)));
            }

            for( auto& val : node.m_vals ) {
                val->visit( *this );
                this->context.require_sized(node.span(), val->m_res_type);
            }
        }
        void visit(::HIR::ExprNode_ArrayList& node) override
        {
            TRACE_FUNCTION_F(&node << " [...,]");
            auto _ = this->push_inner_coerce_scoped(true);
            for( auto& val : node.m_vals ) {
                this->context.add_ivars( val->m_res_type );
            }

            auto array_ty = ::HIR::TypeRef::new_array( context.m_ivars.new_ivar_tr(), node.m_vals.size() );
            this->context.equate_types(node.span(), node.m_res_type, array_ty);
            // Cleanly equate into array (with coercions)
            const auto& inner_ty = array_ty.data().as_Array().inner;
            for( auto& val : node.m_vals ) {
                this->equate_types_inner_coerce(node.span(), inner_ty,  val);
            }

            for( auto& val : node.m_vals ) {
                val->visit( *this );
            }
        }
        void visit(::HIR::ExprNode_ArraySized& node) override
        {
            TRACE_FUNCTION_F(&node << " [...; " << node.m_size << "]");
            this->context.add_ivars( node.m_val->m_res_type );

            // Create result type (can't be known until after const expansion)
            // - Should it be created in const expansion?
            auto ty = ::HIR::TypeRef(HIR::TypeData::make_Array({context.m_ivars.new_ivar_tr(), node.m_size.clone() }));
            this->context.equate_types(node.span(), node.m_res_type, ty);
            // Equate with coercions
            const auto& inner_ty = ty.data().as_Array().inner;
            this->equate_types_inner_coerce(node.span(), inner_ty, node.m_val);
            //this->context.equate_types(node.span(), ::HIR::TypeRef(::HIR::CoreType::Usize), node.m_size->m_res_type);

            node.m_val->visit( *this );
        }

        void visit(::HIR::ExprNode_Literal& node) override
        {
            HIR::TypeRef    ty;
            TU_MATCH_HDRA( (node.m_data), {)
            TU_ARMA(Integer, e) {
                DEBUG("_Literal (: " << e.m_type << " = " << e.m_value << ")");
                if( e.m_type != ::HIR::CoreType::Str ) {
                    ty = ::HIR::TypeRef(e.m_type);
                }
                else {
                    ty = ::HIR::TypeRef::new_infer(~0, ::HIR::InferClass::Integer);
                }
                }
            TU_ARMA(Float, e) {
                DEBUG("_Literal (: " << node.m_res_type << " = " << e.m_value << ")");
                if( e.m_type != ::HIR::CoreType::Str ) {
                    ty = ::HIR::TypeRef(e.m_type);
                }
                else {
                    ty = ::HIR::TypeRef::new_infer(~0, ::HIR::InferClass::Float);
                }
                }
            TU_ARMA(Boolean, e) {
                DEBUG("_Literal ( " << (e ? "true" : "false") << ")");
                ty = ::HIR::TypeRef( ::HIR::CoreType::Bool );
                }
            TU_ARMA(String, e) {
                // TODO: &'static
                DEBUG("_Literal (&str)");
                ty = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef(::HIR::CoreType::Str) );
                }
            TU_ARMA(ByteString, e) {
                // TODO: &'static
                DEBUG("_Literal (&[u8])");
                ty = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef::new_array(::HIR::CoreType::U8, e.size()) );
                }
            }
            this->context.add_ivars(ty);
            this->context.equate_types(node.span(), node.m_res_type, ty);
        }
        void visit(::HIR::ExprNode_PathValue& node) override
        {
            const auto& sp = node.span();
            this->visit_path(sp, node.m_path);
            TRACE_FUNCTION_F(&node << " " << node.m_path);

            this->add_ivars_path(node.span(), node.m_path);

            TU_MATCH_HDRA( (node.m_path.m_data), {)
            TU_ARMA(Generic, e) {
                switch(node.m_target) {
                case ::HIR::ExprNode_PathValue::UNKNOWN:
                    BUG(sp, "_PathValue with target=UNKNOWN and a Generic path - " << e.m_path);
                case ::HIR::ExprNode_PathValue::FUNCTION: {
                    const auto& f = this->context.m_crate.get_function_by_path(sp, e.m_path);
                    fix_param_count(sp, this->context, ::HIR::TypeRef(), false, e, f.m_params, e.m_params);

                    auto ms = MonomorphStatePtr(nullptr, nullptr, &e.m_params);

                    ::HIR::FunctionType ft {
                        f.m_unsafe,
                        f.m_abi,
                        ms.monomorph_type(sp, f.m_return),
                        {}
                        };
                    for( const auto& arg : f.m_args )
                    {
                        ft.m_arg_types.push_back( ms.monomorph_type(sp, arg.second) );
                    }

                    // Apply bounds
                    apply_bounds_as_rules(this->context, sp, f.m_params, ms, /*is_impl_level=*/false);

                    auto ty = ::HIR::TypeRef( ::HIR::TypeData::make_Function(mv$(ft)) );
                    DEBUG("> " << node.m_path << " = " << ty);
                    this->context.equate_types(sp, node.m_res_type, ty);
                    } break;
                case ::HIR::ExprNode_PathValue::STRUCT_CONSTR: {
                    const auto& s = this->context.m_crate.get_struct_by_path(sp, e.m_path);
                    const auto& se = s.m_data.as_Tuple();
                    fix_param_count(sp, this->context, ::HIR::TypeRef(), false, e, s.m_params, e.m_params);

                    auto ms = MonomorphStatePtr(nullptr, &e.m_params, nullptr);

                    ::HIR::FunctionType ft {
                        false,
                        ABI_RUST,
                        ::HIR::TypeRef::new_path( node.m_path.clone(), ::HIR::TypePathBinding::make_Struct(&s) ),
                        {}
                        };
                    for( const auto& arg : se )
                    {
                        ft.m_arg_types.push_back( ms.monomorph_type(sp, arg.ent) );
                    }
                    //apply_bounds_as_rules(this->context, sp, s.m_params, monomorph_cb, /*is_impl_level=*/true);

                    auto ty = ::HIR::TypeRef( ::HIR::TypeData::make_Function(mv$(ft)) );
                    this->context.equate_types(sp, node.m_res_type, ty);
                    } break;
                case ::HIR::ExprNode_PathValue::ENUM_VAR_CONSTR: {
                    const auto& var_name = e.m_path.m_components.back();
                    auto enum_path = get_parent_path(e.m_path);
                    const auto& enm = this->context.m_crate.get_enum_by_path(sp, enum_path);
                    fix_param_count(sp, this->context, ::HIR::TypeRef(), false, e, enm.m_params, e.m_params);
                    size_t idx = enm.find_variant(var_name);
                    ASSERT_BUG(sp, idx != SIZE_MAX, "Missing variant - " << e.m_path);
                    ASSERT_BUG(sp, enm.m_data.is_Data(), "Enum " << enum_path << " isn't a data-holding enum");
                    const auto& var_ty = enm.m_data.as_Data()[idx].type;
                    const auto& str = *var_ty.data().as_Path().binding.as_Struct();
                    const auto& var_data = str.m_data.as_Tuple();

                    auto ms = MonomorphStatePtr(nullptr, &e.m_params, nullptr);
                    ::HIR::FunctionType ft {
                        false,
                        ABI_RUST,
                        ::HIR::TypeRef::new_path( ::HIR::GenericPath(mv$(enum_path), e.m_params.clone()), ::HIR::TypePathBinding::make_Enum(&enm) ),
                        {}
                        };
                    for( const auto& arg : var_data )
                    {
                        ft.m_arg_types.push_back( ms.monomorph_type(sp, arg.ent) );
                    }
                    //apply_bounds_as_rules(this->context, sp, enm.m_params, monomorph_cb, /*is_impl_level=*/true);

                    auto ty = ::HIR::TypeRef( ::HIR::TypeData::make_Function(mv$(ft)) );
                    this->context.equate_types(sp, node.m_res_type, ty);
                    } break;
                case ::HIR::ExprNode_PathValue::STATIC: {
                    const auto& v = this->context.m_crate.get_static_by_path(sp, e.m_path);
                    DEBUG("static v.m_type = " << v.m_type);
                    this->context.equate_types(sp, node.m_res_type, v.m_type);
                    } break;
                case ::HIR::ExprNode_PathValue::CONSTANT: {
                    const auto& v = this->context.m_crate.get_constant_by_path(sp, e.m_path);
                    DEBUG("const"<<v.m_params.fmt_args()<<" v.m_type = " << v.m_type);
                    if( v.m_params.m_types.size() > 0 ) {
                        TODO(sp, "Support generic constants in typeck");
                    }
                    this->context.equate_types(sp, node.m_res_type, v.m_type);
                    } break;
                }
                }
            TU_ARMA(UfcsUnknown, e) {
                BUG(sp, "Encountered UfcsUnknown");
                }
            TU_ARMA(UfcsKnown, e) {
                const auto& trait = this->context.m_crate.get_trait_by_path(sp, e.trait.m_path);
                fix_param_count(sp, this->context, e.type, true, e.trait, trait.m_params,  e.trait.m_params);

                // 1. Add trait bound to be checked.
                this->context.add_trait_bound(sp, e.type,  e.trait.m_path, e.trait.m_params.clone());

                // 2. Locate this item in the trait
                // - If it's an associated `const`, will have to revisit
                auto it = trait.m_values.find( e.item );
                if( it == trait.m_values.end() ) {
                    ERROR(sp, E0000, "`" << e.item << "` is not a value member of trait " << e.trait.m_path);
                }
                TU_MATCH_HDRA( (it->second), {)
                TU_ARMA(Constant, ie) {
                    auto ms = MonomorphStatePtr(&e.type, &e.trait.m_params, nullptr);
                    ::HIR::TypeRef  tmp;
                    const auto& ty = ms.maybe_monomorph_type(sp, tmp, ie.m_type);
                    this->context.equate_types(sp, node.m_res_type, ty);
                    }
                TU_ARMA(Static, ie) {
                    TODO(sp, "Monomorpise associated static type - " << ie.m_type);
                    }
                TU_ARMA(Function, ie) {
                    fix_param_count(sp, this->context, e.type, false, node.m_path, ie.m_params,  e.params);

                    auto ms = MonomorphStatePtr(&e.type, &e.trait.m_params, &e.params);
                    ::HIR::FunctionType ft {
                        ie.m_unsafe, ie.m_abi,
                        ms.monomorph_type(sp, ie.m_return) ,
                        {}
                        };
                    for(const auto& arg : ie.m_args)
                        ft.m_arg_types.push_back( ms.monomorph_type(sp, arg.second) );
                    apply_bounds_as_rules(this->context, sp, ie.m_params, ms, /*is_impl_level=*/false);
                    auto ty = ::HIR::TypeRef(mv$(ft));

                    this->context.equate_types(node.span(), node.m_res_type, ty);
                    }
                }
                }
            TU_ARMA(UfcsInherent, e) {
                // TODO: Share code with visit_call_populate_cache

                // - Locate function (and impl block)
                const ::HIR::Function* fcn_ptr = nullptr;
                const ::HIR::Constant* const_ptr = nullptr;
                const ::HIR::TypeImpl* impl_ptr = nullptr;
                // TODO: Support mutiple matches here (if there's a fuzzy match) and retry if so
                unsigned int count = 0;
                this->context.m_crate.find_type_impls(e.type, context.m_ivars.callback_resolve_infer(),
                    [&](const auto& impl) {
                        DEBUG("- impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                        {
                            auto it = impl.m_methods.find(e.item);
                            if( it != impl.m_methods.end() ) {
                                fcn_ptr = &it->second.data;
                                impl_ptr = &impl;
                                count += 1;
                                return false;
                                //return true;
                            }
                        }
                        {
                            auto it = impl.m_constants.find(e.item);
                            if( it != impl.m_constants.end() ) {
                                const_ptr = &it->second.data;
                                impl_ptr = &impl;
                                count += 1;
                                return false;
                            }
                        }
                        return false;
                    });
                if( count == 0 ) {
                    ERROR(sp, E0000, "Failed to locate associated value " << node.m_path);
                }
                if( count > 1 ) {
                    TODO(sp, "Revisit _PathValue when UfcsInherent has multiple options - " << node.m_path);
                }

                assert(fcn_ptr || const_ptr);
                assert(impl_ptr);

                if( fcn_ptr ) {
                    fix_param_count(sp, this->context, e.type, false, node.m_path, fcn_ptr->m_params,  e.params);
                }
                else {
                    fix_param_count(sp, this->context, e.type, false, node.m_path, const_ptr->m_params,  e.params);
                }

                // If the impl block has parameters, figure out what types they map to
                // - The function params are already mapped (from fix_param_count)
                auto& impl_params = e.impl_params;
                if( impl_ptr->m_params.m_types.size() > 0 )
                {
                    impl_params.m_types.resize( impl_ptr->m_params.m_types.size() );
                    impl_params.m_values.resize( impl_ptr->m_params.m_values.size() );
                    OwnedImplMatcher    matcher(impl_params);
                    // NOTE: Could be fuzzy.
                    bool r = impl_ptr->m_type.match_test_generics(sp, e.type, this->context.m_ivars.callback_resolve_infer(), matcher);
                    for(auto& ty : impl_params.m_types) {
                        // Create new ivars if there's holes
                        if( ty.data().is_Infer() && ty.data().as_Infer().index == ~0u ) {
                            this->context.add_ivars(ty);
                        }
                    }
                    if(!r)
                    {
                        auto t = MonomorphStatePtr(nullptr, &impl_params, nullptr).monomorph_type(sp, impl_ptr->m_type);
                        this->context.equate_types(node.span(), t, e.type);
                    }
                }


                if( fcn_ptr )
                {
                    // Create monomorphise callback
                    const auto& fcn_params = e.params;
                    // TODO: call `context.get_type` in this?
                    auto ms = MonomorphStatePtr(&e.type, &impl_params, &fcn_params);

                    // Bounds (both impl and fn)
                    apply_bounds_as_rules(this->context, sp, impl_ptr->m_params, ms, /*is_impl_level=*/true);
                    apply_bounds_as_rules(this->context, sp, fcn_ptr->m_params, ms, /*is_impl_level=*/false);

                    ::HIR::FunctionType ft {
                        fcn_ptr->m_unsafe, fcn_ptr->m_abi,
                        ms.monomorph_type(sp, fcn_ptr->m_return),
                        {}
                        };
                    for(const auto& arg : fcn_ptr->m_args)
                        ft.m_arg_types.push_back( ms.monomorph_type(sp, arg.second) );
                    auto ty = ::HIR::TypeRef(mv$(ft));

                    this->context.equate_types(node.span(), node.m_res_type, ty);
                }
                else    // !fcn_ptr, ergo const_ptr
                {
                    assert(const_ptr);
                    auto monomorph_cb = MonomorphStatePtr(&e.type, &impl_params,  &e.params);

                    ::HIR::TypeRef  tmp;
                    const auto& ty = monomorph_cb.maybe_monomorph_type(sp, tmp, const_ptr->m_type);

                    apply_bounds_as_rules(this->context, sp, impl_ptr->m_params, monomorph_cb, /*is_impl_level=*/true);
                    this->context.equate_types(node.span(), node.m_res_type, ty);
                }
                }
            }
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
            TRACE_FUNCTION_F(&node << " " << node.m_name << "{" << node.m_slot << "}");

            this->context.equate_types(node.span(), node.m_res_type,  this->context.get_var(node.span(), node.m_slot));
        }
        void visit(::HIR::ExprNode_ConstParam& node) override
        {
            TRACE_FUNCTION_F(&node << " " << node.m_name << "{" << node.m_binding << "}");

            this->context.equate_types(node.span(), node.m_res_type,  this->context.m_resolve.get_const_param_type(node.span(), node.m_binding));
        }

        void visit(::HIR::ExprNode_Closure& node) override
        {
            TRACE_FUNCTION_F(&node << " |...| ...");
            for(auto& arg : node.m_args) {
                this->context.add_ivars( arg.second );
                this->context.handle_pattern( node.span(), arg.first, arg.second );
            }
            this->context.add_ivars( node.m_return );
            this->context.add_ivars( node.m_code->m_res_type );

            // Closure result type
            ::std::vector< ::HIR::TypeRef>  arg_types;
            for(auto& arg : node.m_args) {
                arg_types.push_back( arg.second.clone() );
            }
            this->context.equate_types( node.span(), node.m_res_type, ::HIR::TypeRef::new_closure(&node, mv$(arg_types), node.m_return.clone()) );

            this->context.equate_types_coerce( node.span(), node.m_return, node.m_code );

            // Save/clear/restore loop labels
            auto saved_loops = ::std::move(this->loop_blocks);

            auto _ = this->push_inner_coerce_scoped(true);
            this->closure_ret_types.push_back( RetTarget(node.m_return) );
            node.m_code->visit( *this );
            this->closure_ret_types.pop_back( );

            this->loop_blocks = ::std::move(saved_loops);
        }

        void visit(::HIR::ExprNode_Generator& node) override
        {
            TRACE_FUNCTION_F(&node << " /*gen*/ || ...");
            //for(auto& arg : node.m_args) {
            //    this->context.add_ivars( arg.second );
            //    this->context.handle_pattern( node.span(), arg.first, arg.second );
            //}
            this->context.add_ivars( node.m_return );
            this->context.add_ivars( node.m_yield_ty );
            this->context.add_ivars( node.m_code->m_res_type );

            // Generator result type
            this->context.equate_types( node.span(), node.m_res_type, ::HIR::TypeRef::new_generator(&node) );

            this->context.equate_types_coerce( node.span(), node.m_return, node.m_code );

            // TODO: Save/clear/restore loop labels
            auto _ = this->push_inner_coerce_scoped(true);
            this->closure_ret_types.push_back( RetTarget(node.m_return, node.m_yield_ty) );
            node.m_code->visit( *this );
            this->closure_ret_types.pop_back( );
        }
        void visit(::HIR::ExprNode_GeneratorWrapper& node) override
        {
            BUG(node.span(), "ExprNode_GeneratorWrapper unexpected at this time");
        }

    private:
        void push_traits(const ::HIR::t_trait_list& list) {
            this->m_traits.insert( this->m_traits.end(), list.begin(), list.end() );
        }
        void pop_traits(const ::HIR::t_trait_list& list) {
            this->m_traits.erase( this->m_traits.end() - list.size(), this->m_traits.end() );
        }
        void visit_generic_path(const Span& sp, ::HIR::GenericPath& gp) {
            for(auto& ty : gp.m_params.m_types)
                this->context.add_ivars(ty);
        }
        void visit_path(const Span& sp, ::HIR::Path& path) {
            TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
            (Generic,
                this->visit_generic_path(sp, e);
                ),
            (UfcsKnown,
                this->context.add_ivars(e.type);
                this->visit_generic_path(sp, e.trait);
                for(auto& ty : e.params.m_types)
                    this->context.add_ivars(ty);
                ),
            (UfcsUnknown,
                TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
                ),
            (UfcsInherent,
                this->context.add_ivars(e.type);
                for(auto& ty : e.params.m_types)
                    this->context.add_ivars(ty);
                )
            )
        }

        class InnerCoerceGuard {
            ExprVisitor_Enum& t;
        public:
            InnerCoerceGuard(ExprVisitor_Enum& t): t(t) {}
            ~InnerCoerceGuard() {
                t.inner_coerce_enabled_stack.pop_back();
                DEBUG("inner_coerce POP (S) " << t.can_coerce_inner_result());
            }
        };
        InnerCoerceGuard push_inner_coerce_scoped(bool val) {
            DEBUG("inner_coerce PUSH (S) " << val);
            this->inner_coerce_enabled_stack.push_back(val);
            return InnerCoerceGuard(*this);
        }
        void push_inner_coerce(bool val) {
            DEBUG("inner_coerce PUSH " << val);
            this->inner_coerce_enabled_stack.push_back(val);
        }
        void pop_inner_coerce() {
            assert( this->inner_coerce_enabled_stack.size() );
            this->inner_coerce_enabled_stack.pop_back();
            DEBUG("inner_coerce POP " << can_coerce_inner_result());
        }
        bool can_coerce_inner_result() const {
            if( this->inner_coerce_enabled_stack.size() == 0 ) {
                return true;
            }
            else {
                return this->inner_coerce_enabled_stack.back();
            }
        }
        void equate_types_inner_coerce(const Span& sp, const ::HIR::TypeRef& target, ::HIR::ExprNodeP& node) {
            DEBUG("can_coerce_inner_result() = " << can_coerce_inner_result());
            if( can_coerce_inner_result() ) {
                this->context.equate_types_coerce(sp, target,  node);
            }
            else {
                this->context.equate_types(sp, target,  node->m_res_type);
            }
        }
    };
}


void Typecheck_Code_CS__EnumerateRules(
    Context& context, const typeck::ModuleState& ms,
    t_args& args, const ::HIR::TypeRef& result_type,
    ::HIR::ExprPtr& expr, ::std::unique_ptr<HIR::ExprNode>& root_ptr
    )
{
    TRACE_FUNCTION;

    const Span& sp = root_ptr->span();

    DEBUG("args = " << args);
    DEBUG("result_type = " << result_type);
    for( auto& arg : args ) {
        context.handle_pattern( Span(), arg.first, arg.second );
    }

    struct M: public Monomorphiser
    {
        Context& context;
        ::HIR::ExprPtr& expr;
        mutable  const ::HIR::TypeRef*  cur_self;
        M(Context& context, ::HIR::ExprPtr& expr)
            : context(context)
            , expr(expr)
            , cur_self(nullptr)
        {
        }

        ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& g) const override {
            if( g.binding == GENERIC_Self && cur_self )
                return cur_self->clone();
            return ::HIR::TypeRef(g);
        }
        ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& g) const override {
            return g;
        }

        ::HIR::TypeRef monomorph_type(const Span& sp, const ::HIR::TypeRef& tpl, bool allow_infer=true) const override {
            if( const auto* e = tpl.data().opt_ErasedType() )
            {
                // NOTE: `Typecheck Outer` visits erased types subtly differently (it recurses then handles)
                // - This code handles then recurses (as the return needs to be allocated earlier)
                // SO: We have to expand the list as it comes.
                if( expr.m_erased_types.size() <= e->m_index )
                {
                    expr.m_erased_types.resize(e->m_index + 1);
                }
                ASSERT_BUG(sp, expr.m_erased_types[e->m_index] == HIR::TypeRef(), "Multiple-visits to erased type #" << e->m_index);
                expr.m_erased_types[e->m_index] = context.m_ivars.new_ivar_tr();
                auto rv = expr.m_erased_types[e->m_index].clone();
                auto prev_cur_self = this->cur_self;
                this->cur_self = &rv;
                DEBUG(tpl << " -> " << rv);

                for(const auto& trait : e->m_traits)
                {
                    if( trait.m_type_bounds.size() == 0 )
                    {
                        
                        context.equate_types_assoc(sp, ::HIR::TypeRef(), trait.m_path.m_path, this->monomorph_path_params(sp, trait.m_path.m_params, allow_infer), rv, "", false);
                    }
                    else
                    {
                        for(const auto& aty : trait.m_type_bounds)
                        {
                            auto aty_cloned = this->monomorph_type(sp, aty.second.type);
                            auto params = this->monomorph_path_params(sp, trait.m_path.m_params, allow_infer);
                            context.equate_types_assoc(sp, std::move(aty_cloned), trait.m_path.m_path, std::move(params), rv, aty.first.c_str(), false);
                        }
                    }
                }

                this->cur_self = prev_cur_self;

                return rv;
            }
            else
            {
                return Monomorphiser::monomorph_type(sp, tpl, allow_infer);
            }
        }
    };
    // If the result type contans an erased type, replace that with a new ivar and emit trait bounds for it.
    ::HIR::TypeRef  new_res_ty = M(context, expr).monomorph_type(sp, result_type);
    // - Final check to ensure that all erased type indexes are visited
    for(size_t i = 0; i < expr.m_erased_types.size(); i ++)
    {
        ASSERT_BUG(sp, expr.m_erased_types[i] != HIR::TypeRef(), "Non-visited erased type #" << i);
    }

    if(true)
    {
        DEBUG("--- Pre-adding ivars");
        typecheck::ExprVisitor_AddIvars    visitor(context);
        context.add_ivars(root_ptr->m_res_type);
        root_ptr->visit(visitor);
    }

    DEBUG("--- Enumerating");
    typecheck::ExprVisitor_Enum    visitor(context, ms.m_traits, new_res_ty);
    context.add_ivars(root_ptr->m_res_type);
    root_ptr->visit(visitor);

    DEBUG("Return type = " << new_res_ty << ", root_ptr = " << typeid(*root_ptr).name() << " " << root_ptr->m_res_type);
    context.equate_types_coerce(sp, new_res_ty, root_ptr);
}
