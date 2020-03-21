/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/expr_cs.cpp
 * - Constraint Solver type inferrence
 */
#include "main_bindings.hpp"
#include <hir/expr.hpp>
#include <hir/hir.hpp>
#include <hir/visitor.hpp>
#include <algorithm>    // std::find_if

#include <hir_typeck/static.hpp>
#include "helpers.hpp"
#include "expr_visit.hpp"

namespace {
    inline HIR::ExprNodeP mk_exprnodep(HIR::ExprNode* en, ::HIR::TypeRef ty){ en->m_res_type = mv$(ty); return HIR::ExprNodeP(en); }

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

    bool type_contains_impl_placeholder(const ::HIR::TypeRef& t) {
        return visit_ty_with(t, [&](const auto& ty)->bool {
            if( ty.m_data.is_Generic() && ty.m_data.as_Generic().binding >> 8 == 2 ) {
                return true;
            }
            return false;
            });
    }
}
#define NEWNODE(TY, SP, CLASS, ...)  mk_exprnodep(new HIR::ExprNode##CLASS(SP ,## __VA_ARGS__), TY)

// PLAN: Build up a set of conditions that are easier to solve
struct Context
{
    class Revisitor
    {
    public:
        virtual ~Revisitor() = default;
        virtual const Span& span() const = 0;
        virtual void fmt(::std::ostream& os) const = 0;
        virtual bool revisit(Context& context, bool is_fallback) = 0;
    };

    struct Binding
    {
        RcString    name;
        ::HIR::TypeRef  ty;
        //unsigned int ivar;
    };

    /// Inferrence variable equalities
    struct Coercion
    {
        unsigned rule_idx;
        ::HIR::TypeRef  left_ty;
        ::HIR::ExprNodeP* right_node_ptr;

        friend ::std::ostream& operator<<(::std::ostream& os, const Coercion& v) {
            os << "R" << v.rule_idx << " " << v.left_ty << " := " << v.right_node_ptr << " " << &**v.right_node_ptr << " (" << (*v.right_node_ptr)->m_res_type << ")";
            return os;
        }
    };
    struct Associated
    {
        unsigned rule_idx;
        Span    span;
        ::HIR::TypeRef  left_ty;

        ::HIR::SimplePath   trait;
        ::HIR::PathParams   params;
        ::HIR::TypeRef  impl_ty;
        RcString    name;   // if "", no type is used (and left is ignored) - Just does trait selection

        // HACK: operators are special - the result when both types are primitives is ALWAYS the lefthand side
        bool    is_operator;

        friend ::std::ostream& operator<<(::std::ostream& os, const Associated& v) {
            os << "R" << v.rule_idx << " ";
            if( v.name == "" ) {
                os << "req ty " << v.impl_ty << " impl " << v.trait << v.params;
            }
            else {
                os << v.left_ty << " = " << "< `" << v.impl_ty << "` as `" << v.trait << v.params << "` >::" << v.name;
            }
            if( v.is_operator )
                os << " - op";
            return os;
        }
    };

    struct IVarPossible
    {
        bool force_disable = false;
        bool force_no_to = false;
        bool force_no_from = false;
        // Target types for coercion/unsizing (these types are known to exist in the function)
        ::std::vector<::HIR::TypeRef>   types_coerce_to;
        ::std::vector<::HIR::TypeRef>   types_unsize_to;
        // Source types for coercion/unsizing (these types are known to exist in the function)
        ::std::vector<::HIR::TypeRef>   types_coerce_from;
        ::std::vector<::HIR::TypeRef>   types_unsize_from;
        // Possible default types (from generic defaults)
        //::std::vector<::HIR::TypeRef>   types_default;
        // Possible types from trait impls (may introduce new types)
        ::std::vector<::HIR::TypeRef>   bounded;

        void reset() {
            //auto tmp = mv$(this->types_default);
            *this = IVarPossible();
            //this->types_default = mv$(tmp);
        }
        bool has_rules() const {
            if( !types_coerce_to.empty() )
                return true;
            if( !types_unsize_to.empty() )
                return true;
            if( !types_coerce_from.empty() )
                return true;
            if( !types_unsize_from.empty() )
                return true;
            //if( !types_default.empty() )
            //    return true;
            if( !bounded.empty() )
                return true;
            return false;
        }
    };

    const ::HIR::Crate& m_crate;

    ::std::vector<Binding>  m_bindings;
    HMTypeInferrence    m_ivars;
    TraitResolution m_resolve;

    unsigned next_rule_idx;
    // NOTE: unique_ptr used to reduce copy costs of the list
    ::std::vector< ::std::unique_ptr<Coercion> > link_coerce;
    ::std::vector<Associated> link_assoc;
    /// Nodes that need revisiting (e.g. method calls when the receiver isn't known)
    ::std::vector< ::HIR::ExprNode*>    to_visit;
    /// Callback-based revisits (e.g. for slice patterns handling slices/arrays)
    ::std::vector< ::std::unique_ptr<Revisitor> >   adv_revisits;

    // Keep track of if an ivar is used in a context where it has to be Sized
    // - If it is, then we can discount any unsized possibilities
    ::std::vector<bool> m_ivars_sized;
    ::std::vector< IVarPossible>    possible_ivar_vals;

    const ::HIR::SimplePath m_lang_Box;

    Context(const ::HIR::Crate& crate, const ::HIR::GenericParams* impl_params, const ::HIR::GenericParams* item_params, const ::HIR::SimplePath& mod_path):
        m_crate(crate),
        m_resolve(m_ivars, crate, impl_params, item_params, mod_path)
        ,next_rule_idx( 0 )
        ,m_lang_Box( crate.get_lang_item_path_opt("owned_box") )
    {
    }

    void dump() const;

    bool take_changed() { return m_ivars.take_changed(); }
    bool has_rules() const {
        return !(link_coerce.empty() && link_assoc.empty() && to_visit.empty() && adv_revisits.empty());
    }

    inline void add_ivars(::HIR::TypeRef& ty) {
        m_ivars.add_ivars(ty);
    }
    // - Equate two types, with no possibility of coercion
    //  > Errors if the types are incompatible.
    //  > Forces types if one side is an infer
    void equate_types(const Span& sp, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r);
    void equate_types_inner(const Span& sp, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r);
    // - Equate two types, allowing inferrence
    void equate_types_coerce(const Span& sp, const ::HIR::TypeRef& l, ::HIR::ExprNodeP& node_ptr);
    // - Equate a type to an associated type (if name == "", no equation is done, but trait is searched)
    void equate_types_assoc(
        const Span& sp, const ::HIR::TypeRef& l,
        const ::HIR::SimplePath& trait, ::std::vector< ::HIR::TypeRef> ty_args, const ::HIR::TypeRef& impl_ty, const char *name,
        bool is_op=false
        )
    {
        ::HIR::PathParams   pp;
        pp.m_types = mv$(ty_args);
        equate_types_assoc(sp, l, trait, mv$(pp), impl_ty, name, is_op);
    }
    void equate_types_assoc(const Span& sp, const ::HIR::TypeRef& l,  const ::HIR::SimplePath& trait, ::HIR::PathParams params, const ::HIR::TypeRef& impl_ty, const char *name, bool is_op);

    /// Adds a `ty: Sized` bound to the contained ivars.
    void require_sized(const Span& sp, const ::HIR::TypeRef& ty);

    // - Add a trait bound (gets encoded as an associated type bound)
    void add_trait_bound(const Span& sp, const ::HIR::TypeRef& impl_ty, const ::HIR::SimplePath& trait, ::HIR::PathParams params) {
        equate_types_assoc(sp, ::HIR::TypeRef(), trait, mv$(params), impl_ty, "", false);
    }

    /// Disable possibility checking for the type (as if <impossible> was added as a coerce_to)
    void equate_types_to_shadow(const Span& sp, const ::HIR::TypeRef& r) {
        equate_types_shadow(sp, r, true);
    }
    /// Disable possibility checking for the type (as if <impossible> was added as a coerce_from)
    void equate_types_from_shadow(const Span& sp, const ::HIR::TypeRef& l) {
        equate_types_shadow(sp, l, false);
    }
    void equate_types_shadow(const Span& sp, const ::HIR::TypeRef& ty, bool is_to);
    void equate_types_shadow_strong(const Span& sp, const ::HIR::TypeRef& ty);

    /// Possible type that this ivar can coerce to
    void possible_equate_type_coerce_to(unsigned int ivar_index, const ::HIR::TypeRef& t) {
        possible_equate_type(ivar_index, t, true , false);
    }
    /// Possible type that this ivar can unsize to
    void possible_equate_type_unsize_to(unsigned int ivar_index, const ::HIR::TypeRef& t) {
        possible_equate_type(ivar_index, t, true , true);
    }
    /// Possible type that this ivar can coerce from
    void possible_equate_type_coerce_from(unsigned int ivar_index, const ::HIR::TypeRef& t) {
        possible_equate_type(ivar_index, t, false, false);
    }
    /// Possible type that this ivar can unsize from
    void possible_equate_type_unsize_from(unsigned int ivar_index, const ::HIR::TypeRef& t) {
        possible_equate_type(ivar_index, t, false, true);
    }
    // Mark an ivar as having an unknown possibility (on the destination side)
    void possible_equate_type_disable_to(unsigned int ivar_index) {
        possible_equate_type_disable(ivar_index, true);
    }
    // Mark an ivar as having an unknown possibility (on the source side)
    void possible_equate_type_disable_from(unsigned int ivar_index) {
        possible_equate_type_disable(ivar_index, false);
    }
    /// Default type
    //void possible_equate_type_def(unsigned int ivar_index, const ::HIR::TypeRef& t);
    /// Add a possible type for an ivar (which is used if only one possibility meets available bounds)
    void possible_equate_type_bound(const Span& sp, unsigned int ivar_index, const ::HIR::TypeRef& t);

    void possible_equate_type(unsigned int ivar_index, const ::HIR::TypeRef& t, bool is_to, bool is_borrow);
    void possible_equate_type_disable(unsigned int ivar_index, bool is_to);
    void possible_equate_type_disable_strong(const Span& sp, unsigned int ivar_index);

    // - Add a pattern binding (forcing the type to match)
    void handle_pattern(const Span& sp, ::HIR::Pattern& pat, const ::HIR::TypeRef& type);
    void handle_pattern_direct_inner(const Span& sp, ::HIR::Pattern& pat, const ::HIR::TypeRef& type);
    void add_binding_inner(const Span& sp, const ::HIR::PatternBinding& pb, ::HIR::TypeRef type);

    void add_var(const Span& sp, unsigned int index, const RcString& name, ::HIR::TypeRef type);
    const ::HIR::TypeRef& get_var(const Span& sp, unsigned int idx) const;

    // - Add a revisit entry
    void add_revisit(::HIR::ExprNode& node);
    void add_revisit_adv(::std::unique_ptr<Revisitor> ent);

    const ::HIR::TypeRef& get_type(const ::HIR::TypeRef& ty) const { return m_ivars.get_type(ty); }

    /// Create an autoderef operation from val_node->m_res_type to ty_dst (handling implicit unsizing)
    ::HIR::ExprNodeP create_autoderef(::HIR::ExprNodeP val_node, ::HIR::TypeRef ty_dst) const;

private:
    void add_ivars_params(::HIR::PathParams& params) {
        m_ivars.add_ivars_params(params);
    }
};

static void fix_param_count(const Span& sp, Context& context, const ::HIR::TypeRef& self_ty, bool use_defaults, const ::HIR::Path& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params);
static void fix_param_count(const Span& sp, Context& context, const ::HIR::TypeRef& self_ty, bool use_defaults, const ::HIR::GenericPath& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params);

namespace {

    void apply_bounds_as_rules(Context& context, const Span& sp, const ::HIR::GenericParams& params_def, t_cb_generic monomorph_cb, bool is_impl_level)
    {
        TRACE_FUNCTION;
        for(const auto& bound : params_def.m_bounds)
        {
            TU_MATCH(::HIR::GenericBound, (bound), (be),
            (Lifetime,
                ),
            (TypeLifetime,
                ),
            (TraitBound,
                auto real_type = monomorphise_type_with(sp, be.type, monomorph_cb);
                auto real_trait = monomorphise_genericpath_with(sp, be.trait.m_path, monomorph_cb, false);
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
                    ::HIR::GenericPath  type_trait_path;
                    ASSERT_BUG(sp, be.trait.m_trait_ptr, "Trait pointer not set in " << be.trait.m_path);
                    // TODO: Store the source trait for this bound in the the bound list?
                    if( !context.m_resolve.trait_contains_type(sp, real_trait, *be.trait.m_trait_ptr, assoc.first.c_str(),  type_trait_path) )
                        BUG(sp, "Couldn't find associated type " << assoc.first << " in trait " << real_trait);

                    auto other_ty = monomorphise_type_with(sp, assoc.second, monomorph_cb, true);

                    context.equate_types_assoc(sp, other_ty,  type_trait_path.m_path, mv$(type_trait_path.m_params.m_types), real_type, assoc.first.c_str());
                }
                ),
            (TypeEquality,
                auto real_type_left = context.m_resolve.expand_associated_types(sp, monomorphise_type_with(sp, be.type, monomorph_cb));
                auto real_type_right = context.m_resolve.expand_associated_types(sp, monomorphise_type_with(sp, be.other_type, monomorph_cb));
                context.equate_types(sp, real_type_left, real_type_right);
                )
            )
        }

        for(size_t i = 0; i < params_def.m_types.size(); i++)
        {
            if( params_def.m_types[i].m_is_sized )
            {
                ::HIR::TypeRef  ty("", (is_impl_level ? 0 : 256) + i);
                context.require_sized(sp, monomorph_cb(ty));
            }
        }
    }

    bool visit_call_populate_cache(Context& context, const Span& sp, ::HIR::Path& path, ::HIR::ExprCallCache& cache) __attribute__((warn_unused_result));
    bool visit_call_populate_cache_UfcsInherent(Context& context, const Span& sp, ::HIR::Path& path, ::HIR::ExprCallCache& cache, const ::HIR::Function*& fcn_ptr);

    /// (HELPER) Populate the cache for nodes that use visit_call
    /// TODO: If the function has multiple mismatched options, tell the caller to try again later?
    bool visit_call_populate_cache(Context& context, const Span& sp, ::HIR::Path& path, ::HIR::ExprCallCache& cache)
    {
        TRACE_FUNCTION_FR(path, path);
        assert(cache.m_arg_types.size() == 0);

        const ::HIR::Function*  fcn_ptr = nullptr;

        TU_MATCH_HDRA( (path.m_data), {)
        TU_ARMA(Generic, e) {
            const auto& fcn = context.m_crate.get_function_by_path(sp, e.m_path);
            fix_param_count(sp, context, ::HIR::TypeRef(), false, path, fcn.m_params,  e.m_params);
            fcn_ptr = &fcn;
            cache.m_fcn_params = &fcn.m_params;

            //const auto& params_def = fcn.m_params;
            const auto& path_params = e.m_params;
            cache.m_monomorph_cb = [&](const ::HIR::TypeRef& gt)->const ::HIR::TypeRef& {
                    const auto& e = gt.m_data.as_Generic();
                    if( e.name == "Self" || e.binding == 0xFFFF )
                        TODO(sp, "Handle 'Self' when monomorphising");
                    if( e.binding < 256 ) {
                        BUG(sp, "Impl-level parameter on free function (#" << e.binding << " " << e.name << ")");
                    }
                    else if( e.binding < 512 ) {
                        auto idx = e.binding - 256;
                        if( idx >= path_params.m_types.size() ) {
                            BUG(sp, "Generic param out of input range - " << idx << " '"<<e.name<<"' >= " << path_params.m_types.size());
                        }
                        return context.get_type(path_params.m_types[idx]);
                    }
                    else {
                        BUG(sp, "Generic bounding out of total range");
                    }
                };
            }
        TU_ARMA(UfcsKnown, e) {
            const auto& trait = context.m_crate.get_trait_by_path(sp, e.trait.m_path);
            fix_param_count(sp, context, *e.type, true, path, trait.m_params, e.trait.m_params);
            if( trait.m_values.count(e.item) == 0 ) {
                BUG(sp, "Method '" << e.item << "' of trait " << e.trait.m_path << " doesn't exist");
            }
            const auto& fcn = trait.m_values.at(e.item).as_Function();
            fix_param_count(sp, context, *e.type, false, path, fcn.m_params,  e.params);
            cache.m_fcn_params = &fcn.m_params;
            cache.m_top_params = &trait.m_params;

            // Add a bound requiring the Self type impl the trait
            context.add_trait_bound(sp, *e.type,  e.trait.m_path, e.trait.m_params.clone());

            fcn_ptr = &fcn;

            const auto& trait_params = e.trait.m_params;
            const auto& path_params = e.params;
            cache.m_monomorph_cb = [&](const ::HIR::TypeRef& gt)->const ::HIR::TypeRef& {
                    const auto& ge = gt.m_data.as_Generic();
                    if( ge.binding == 0xFFFF ) {
                        return *e.type;
                    }
                    else if( ge.binding < 256 ) {
                        auto idx = ge.binding;
                        if( idx >= trait_params.m_types.size() ) {
                            BUG(sp, "Generic param (impl) out of input range - " << idx << " '"<<ge.name<<"' >= " << trait_params.m_types.size());
                        }
                        return context.get_type(trait_params.m_types[idx]);
                    }
                    else if( ge.binding < 512 ) {
                        auto idx = ge.binding - 256;
                        if( idx >= path_params.m_types.size() ) {
                            BUG(sp, "Generic param out of input range - " << idx << " '"<<ge.name<<"' >= " << path_params.m_types.size());
                        }
                        return context.get_type(path_params.m_types[idx]);
                    }
                    else {
                        BUG(sp, "Generic bounding out of total range");
                    }
                };
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
        const auto& monomorph_cb = cache.m_monomorph_cb;

        // --- Monomorphise the argument/return types (into current context)
        for(const auto& arg : fcn.m_args) {
            TRACE_FUNCTION_FR(path << " - Arg " << arg.first << ": " << arg.second, "Arg " << arg.first << " : " << cache.m_arg_types.back());
            cache.m_arg_types.push_back( monomorphise_type_with(sp, arg.second,  monomorph_cb, false) );
        }
        {
            TRACE_FUNCTION_FR(path << " - Ret " << fcn.m_return, "Ret " << cache.m_arg_types.back());
            cache.m_arg_types.push_back( monomorphise_type_with(sp, fcn.m_return,  monomorph_cb, false) );
        }

        // --- Apply bounds by adding them to the associated type ruleset
        apply_bounds_as_rules(context, sp, *cache.m_fcn_params, cache.m_monomorph_cb, /*is_impl_level=*/false);

        return true;
    }
    bool visit_call_populate_cache_UfcsInherent(Context& context, const Span& sp, ::HIR::Path& path, ::HIR::ExprCallCache& cache, const ::HIR::Function*& fcn_ptr)
    {
        auto& e = path.m_data.as_UfcsInherent();

        const ::HIR::TypeImpl* impl_ptr = nullptr;
        // Detect multiple applicable methods and get the caller to try again later if there are multiple
        unsigned int count = 0;
        context.m_crate.find_type_impls(*e.type, context.m_ivars.callback_resolve_infer(),
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
        fix_param_count(sp, context, *e.type, false, path, fcn_ptr->m_params,  e.params);
        cache.m_fcn_params = &fcn_ptr->m_params;


        // If the impl block has parameters, figure out what types they map to
        // - The function params are already mapped (from fix_param_count)
        auto& impl_params = e.impl_params;
        if( impl_ptr->m_params.m_types.size() > 0 )
        {
            // Default-construct entires in the `impl_params` array
            impl_params.m_types.resize( impl_ptr->m_params.m_types.size() );

            auto cmp = impl_ptr->m_type.match_test_generics_fuzz(sp, *e.type, context.m_ivars.callback_resolve_infer(), [&](auto idx, const auto& /*name*/, const auto& ty) {
                assert( idx < impl_params.m_types.size() );
                impl_params.m_types[idx] = ty.clone();
                return ::HIR::Compare::Equal;
                });
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


                // Monomorphise the impl type with the new ivars, and equate to *e.type
                auto impl_monomorph_cb = [&](const auto& gt)->const ::HIR::TypeRef& {
                    const auto& ge = gt.m_data.as_Generic();
                    if( ge.binding == 0xFFFF ) {
                        return context.get_type(*e.type);
                    }
                    else if( ge.binding < 256 ) {
                        auto idx = ge.binding;
                        if( idx >= impl_params.m_types.size() ) {
                            BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << impl_params.m_types.size());
                        }
                        return context.get_type(impl_params.m_types[idx]);
                    }
                    else {
                        BUG(sp, "Generic bounding out of total range - " << ge.binding);
                    }
                    };
                auto impl_ty_mono = monomorphise_type_with(sp, impl_ptr->m_type, impl_monomorph_cb, false);
                DEBUG("- impl_ty_mono = " << impl_ty_mono);

                context.equate_types(sp, impl_ty_mono, *e.type);
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
        cache.m_monomorph_cb = [&](const auto& gt)->const ::HIR::TypeRef& {
                const auto& ge = gt.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    return context.get_type(*e.type);
                }
                else if( ge.binding < 256 ) {
                    auto idx = ge.binding;
                    if( idx >= impl_params.m_types.size() ) {
                        BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << impl_params.m_types.size());
                    }
                    return context.get_type(impl_params.m_types[idx]);
                }
                else if( ge.binding < 512 ) {
                    auto idx = ge.binding - 256;
                    if( idx >= fcn_params.m_types.size() ) {
                        BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << fcn_params.m_types.size());
                    }
                    return context.get_type(fcn_params.m_types[idx]);
                }
                else if( ge.binding < 256*3 ) {
                    auto idx = ge.binding - 256*2;
                    TODO(sp, "Placeholder generics - " << idx);
                }
                else {
                    BUG(sp, "Generic bounding out of total range - " << ge.binding);
                }
            };

        // Add trait bounds for all impl and function bounds
        apply_bounds_as_rules(context, sp, impl_ptr->m_params, cache.m_monomorph_cb, /*is_impl_level=*/true);

        // Equate `Self` and `impl_ptr->m_type` (after monomorph)
        {
            ::HIR::TypeRef tmp;
            const auto& impl_ty_m = (monomorphise_type_needed(impl_ptr->m_type) ? tmp = monomorphise_type_with(sp, impl_ptr->m_type, cache.m_monomorph_cb) : impl_ptr->m_type);

            context.equate_types(sp, *e.type, impl_ty_m);
        }

        return true;
    }

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
        ::std::vector< const ::HIR::TypeRef*>   closure_ret_types;

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
                return ty.m_data.is_Diverge();// || (ty.m_data.is_Infer() && ty.m_data.as_Infer().ty_class == ::HIR::InferClass::Diverge);
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
                    if(const auto* e = this->context.get_type(snp->m_res_type).m_data.opt_Infer())
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
        void visit(::HIR::ExprNode_Return& node) override
        {
            TRACE_FUNCTION_F(&node << " return ...");
            this->context.add_ivars( node.m_value->m_res_type );

            const auto& ret_ty = ( this->closure_ret_types.size() > 0 ? *this->closure_ret_types.back() : this->ret_type );
            this->context.equate_types_coerce(node.span(), ret_ty, node.m_value);

            this->push_inner_coerce( true );
            node.m_value->visit( *this );
            this->pop_inner_coerce();
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_diverge());
        }

        void visit(::HIR::ExprNode_Loop& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
            TRACE_FUNCTION_F(&node << " loop ('" << node.m_label << ") { ... }");
            // Push this node to a stack so `break` statements can update the yeilded value
            this->loop_blocks.push_back( &node );
            node.m_diverges = true;    // Set to `false` if a break is hit

            // NOTE: This doesn't set the ivar to !, but marks it as a ! ivar (similar to the int/float markers)
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_diverge());

            this->context.add_ivars(node.m_code->m_res_type);
            this->context.equate_types(node.span(), node.m_code->m_res_type, ::HIR::TypeRef::new_unit());
            node.m_code->visit( *this );

            this->loop_blocks.pop_back( );

            if( node.m_diverges ) {
                DEBUG("Loop diverged");
            }
        }
        void visit(::HIR::ExprNode_LoopControl& node) override
        {
            TRACE_FUNCTION_F(&node << " " << (node.m_continue ? "continue" : "break") << " '" << node.m_label);
            // Break types
            if( !node.m_continue )
            {
                if( this->loop_blocks.empty() ) {
                    ERROR(node.span(), E0000, "Break statement with no acive loop");
                }

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
                    loop_node_ptr = this->loop_blocks.back();
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
                if( node.m_type.m_data.is_Infer() ) {
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
                this->equate_types_inner_coerce(node.span(), node.m_res_type, arm.m_code);
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
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
            }
            node.m_true->visit( *this );

            if( node.m_false ) {
                this->context.add_ivars( node.m_false->m_res_type );
                this->context.equate_types_coerce(node.span(), node.m_res_type, node.m_false);
                node.m_false->visit( *this );
            }
            else {
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
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

                this->context.equate_types_assoc(node.span(), ::HIR::TypeRef(), trait_path, ::make_vec1(node.m_value->m_res_type.clone()),  node.m_slot->m_res_type.clone(), "");
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
                case ::HIR::ExprNode_BinOp::Op::CmpLt:   item_name = TARGETVER_1_29 ? "partial_ord" : "ord"; break;
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:  item_name = TARGETVER_1_29 ? "partial_ord" : "ord"; break;
                case ::HIR::ExprNode_BinOp::Op::CmpGt:   item_name = TARGETVER_1_29 ? "partial_ord" : "ord"; break;
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:  item_name = TARGETVER_1_29 ? "partial_ord" : "ord"; break;
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
        void visit(::HIR::ExprNode_Cast& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);

            TRACE_FUNCTION_F(&node << " ... as " << node.m_res_type);
            this->context.add_ivars( node.m_value->m_res_type );

            node.m_value->visit( *this );

            // TODO: Only revisit if the cast type requires inferring.
            this->context.add_revisit(node);
        }
        void visit(::HIR::ExprNode_Unsize& node) override
        {
            BUG(node.span(), "Hit _Unsize");
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
                this->context.add_ivars(*e.type);
                this->add_ivars_generic_path(sp, e.trait);
                for(auto& ty : e.params.m_types)
                    this->context.add_ivars(ty);
                ),
            (UfcsUnknown,
                TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
                ),
            (UfcsInherent,
                this->context.add_ivars(*e.type);
                for(auto& ty : e.params.m_types)
                    this->context.add_ivars(ty);
                )
            )
        }

        ::HIR::TypeRef get_structenum_ty(const Span& sp, bool is_struct, ::HIR::GenericPath& gp)
        {
            if( is_struct )
            {
                const auto& str = this->context.m_crate.get_struct_by_path(sp, gp.m_path);
                fix_param_count(sp, this->context, ::HIR::TypeRef(), false, gp, str.m_params, gp.m_params);

                return ::HIR::TypeRef::new_path( gp.clone(), ::HIR::TypePathBinding::make_Struct(&str) );
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
            TU_MATCH_HDRA( (ty.m_data.as_Path().binding), {)
            TU_ARMA(Unbound, e) {}
            TU_ARMA(Opaque, e) {}
            TU_ARMA(Enum, e) {
                const auto& var_name = node.m_path.m_path.m_components.back();
                const auto& enm = *e;
                size_t idx = enm.find_variant(var_name);
                const auto& var_ty = enm.m_data.as_Data()[idx].type;
                const auto& str = *var_ty.m_data.as_Path().binding.as_Struct();
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

            const auto& ty_params = node.m_path.m_params.m_types;
            auto monomorph_cb = [&](const auto& gt)->const ::HIR::TypeRef& {
                const auto& ge = gt.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    return ty;
                }
                else if( ge.binding < 256 ) {
                    if( ge.binding >= ty_params.size() ) {
                        BUG(sp, "Type parameter index out of range (#" << ge.binding << " " << ge.name << ")");
                    }
                    return ty_params[ge.binding];
                }
                else {
                    BUG(sp, "Method-level parameter on struct (#" << ge.binding << " " << ge.name << ")");
                }
                };

            // Bind fields with type params (coercable)
            node.m_arg_types.resize( node.m_args.size() );
            for( unsigned int i = 0; i < node.m_args.size(); i ++ )
            {
                const auto& des_ty_r = fields[i].ent;
                const auto* des_ty = &des_ty_r;
                if( monomorphise_type_needed(des_ty_r) ) {
                    node.m_arg_types[i] = monomorphise_type_with(sp, des_ty_r, monomorph_cb);
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
            ASSERT_BUG(sp, TU_TEST1(t.m_data, Path, .path.m_data.is_Generic()), "Struct literal with non-Generic path - " << t);
            node.m_real_path = mv$(t.m_data.as_Path().path.m_data.as_Generic());
            auto& ty_path = node.m_real_path;

            // - Create ivars in path, and set result type
            const auto ty = this->get_structenum_ty(node.span(), node.m_is_struct, ty_path);
            this->context.equate_types(node.span(), node.m_res_type, ty);
            if( node.m_base_value ) {
                this->context.equate_types(node.span(), node.m_base_value->m_res_type, ty);
            }

            const ::HIR::t_struct_fields* fields_ptr = nullptr;
            const ::HIR::GenericParams* generics = nullptr;
            TU_MATCH_HDRA( (ty.m_data.as_Path().binding), {)
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
                const auto& str = *var.type.m_data.as_Path().binding.as_Struct();
                /*
                if( it->second.is_Unit() || it->second.is_Value() || it->second.is_Tuple() ) {
                }
                */
                ASSERT_BUG(sp, var.is_struct, "Struct literal for enum on non-struct variant");
                fields_ptr = &str.m_data.as_Named();
                generics = &enm.m_params;
                }
            TU_ARMA(Union, e) {
                TODO(node.span(), "StructLiteral of a union - " << ty);
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

            const auto& ty_params = ty_path.m_params.m_types;
            auto monomorph_cb = [&](const auto& gt)->const ::HIR::TypeRef& {
                const auto& ge = gt.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    return ty;
                }
                else if( ge.binding < 256 ) {
                    if( ge.binding >= ty_params.size() ) {
                        BUG(node.span(), "Type parameter index out of range (#" << ge.binding << " " << ge.name << ")");
                    }
                    return ty_params[ge.binding];
                }
                else {
                    BUG(node.span(), "Method-level parameter on struct (#" << ge.binding << " " << ge.name << ")");
                }
                };

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
                        des_ty_cache = monomorphise_type_with(node.span(), des_ty_r, monomorph_cb);
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
        void visit(::HIR::ExprNode_UnionLiteral& node) override
        {
            const Span& sp = node.span();
            TRACE_FUNCTION_F(&node << " " << node.m_path << "{ " << node.m_variant_name << ": ... }");
            this->context.add_ivars( node.m_value->m_res_type );

            const auto& unm = this->context.m_crate.get_union_by_path(sp, node.m_path.m_path);
            fix_param_count(sp, this->context, ::HIR::TypeRef(), false, node.m_path, unm.m_params, node.m_path.m_params);
            const auto ty = ::HIR::TypeRef::new_path( node.m_path.clone(), &unm );

            this->context.equate_types(node.span(), node.m_res_type, ty);

            auto monomorph_cb = monomorphise_type_get_cb(node.span(), &ty, &node.m_path.m_params, nullptr);

            // Convert bounds on the type into rules
            apply_bounds_as_rules(context, node.span(), unm.m_params, monomorph_cb, /*is_impl_level=*/true);

            auto it = ::std::find_if(unm.m_variants.begin(), unm.m_variants.end(), [&](const auto& v)->bool{ return v.first == node.m_variant_name; });
            assert(it != unm.m_variants.end());
            const auto& des_ty_r = it->second.ent;
            ::HIR::TypeRef  des_ty_cache;
            const auto* des_ty = &des_ty_r;
            if( monomorphise_type_needed(des_ty_r) ) {
                if( des_ty_cache == ::HIR::TypeRef() ) {
                    des_ty_cache = monomorphise_type_with(node.span(), des_ty_r, monomorph_cb);
                }
                else {
                    // TODO: Is it an error when it's already populated?
                }
                des_ty = &des_ty_cache;
            }
            //this->equate_types_inner_coerce(node.span(), *des_ty,  node.m_value);
            this->context.equate_types_coerce(node.span(), *des_ty,  node.m_value);

            node.m_value->visit(*this);
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
                if( const auto* e = ty.m_data.opt_Tuple() )
                {
                    if( e->size() != node.m_vals.size() ) {
                        ERROR(node.span(), E0000, "Tuple literal node count mismatches with return type");
                    }
                }
                else if( ty.m_data.is_Infer() )
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
                const auto& inner_tys = this->context.get_type(node.m_res_type).m_data.as_Tuple();
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
            const auto& inner_ty = *array_ty.m_data.as_Array().inner;
            for( auto& val : node.m_vals ) {
                this->equate_types_inner_coerce(node.span(), inner_ty,  val);
            }

            for( auto& val : node.m_vals ) {
                val->visit( *this );
            }
        }
        void visit(::HIR::ExprNode_ArraySized& node) override
        {
            TRACE_FUNCTION_F(&node << " [...; "<<node.m_size_val<<"]");
            this->context.add_ivars( node.m_val->m_res_type );

            // Create result type (can't be known until after const expansion)
            // - Should it be created in const expansion?
            auto ty = ::HIR::TypeRef::new_array( context.m_ivars.new_ivar_tr(), node.m_size_val );
            this->context.equate_types(node.span(), node.m_res_type, ty);
            // Equate with coercions
            const auto& inner_ty = *ty.m_data.as_Array().inner;
            this->equate_types_inner_coerce(node.span(), inner_ty, node.m_val);
            this->context.equate_types(node.span(), ::HIR::TypeRef(::HIR::CoreType::Usize), node.m_size->m_res_type);

            node.m_val->visit( *this );
        }

        void visit(::HIR::ExprNode_Literal& node) override
        {
            TU_MATCH(::HIR::ExprNode_Literal::Data, (node.m_data), (e),
            (Integer,
                DEBUG(" (: " << e.m_type << " = " << e.m_value << ")");
                assert(node.m_res_type.m_data.is_Primitive() || node.m_res_type.m_data.as_Infer().ty_class == ::HIR::InferClass::Integer);
                ),
            (Float,
                DEBUG(" (: " << node.m_res_type << " = " << e.m_value << ")");
                assert(node.m_res_type.m_data.is_Primitive() || node.m_res_type.m_data.as_Infer().ty_class == ::HIR::InferClass::Float);
                ),
            (Boolean,
                DEBUG(" ( " << (e ? "true" : "false") << ")");
                ),
            (String,
                ),
            (ByteString,
                )
            )
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

                    auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr, nullptr, &e.m_params);

                    ::HIR::FunctionType ft {
                        f.m_unsafe,
                        f.m_abi,
                        box$( monomorphise_type_with(sp, f.m_return, monomorph_cb) ),
                        {}
                        };
                    for( const auto& arg : f.m_args )
                    {
                        ft.m_arg_types.push_back( monomorphise_type_with(sp, arg.second, monomorph_cb) );
                    }

                    // Apply bounds
                    apply_bounds_as_rules(this->context, sp, f.m_params, monomorph_cb, /*is_impl_level=*/false);

                    auto ty = ::HIR::TypeRef( ::HIR::TypeData::make_Function(mv$(ft)) );
                    DEBUG("> " << node.m_path << " = " << ty);
                    this->context.equate_types(sp, node.m_res_type, ty);
                    } break;
                case ::HIR::ExprNode_PathValue::STRUCT_CONSTR: {
                    const auto& s = this->context.m_crate.get_struct_by_path(sp, e.m_path);
                    const auto& se = s.m_data.as_Tuple();
                    fix_param_count(sp, this->context, ::HIR::TypeRef(), false, e, s.m_params, e.m_params);

                    ::HIR::FunctionType ft {
                        false,
                        ABI_RUST,
                        box$( ::HIR::TypeRef::new_path( node.m_path.clone(), ::HIR::TypePathBinding::make_Struct(&s) ) ),
                        {}
                        };
                    for( const auto& arg : se )
                    {
                        ft.m_arg_types.push_back( monomorphise_type(sp, s.m_params, e.m_params, arg.ent) );
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
                    const auto& str = *var_ty.m_data.as_Path().binding.as_Struct();
                    const auto& var_data = str.m_data.as_Tuple();

                    ::HIR::FunctionType ft {
                        false,
                        ABI_RUST,
                        box$( ::HIR::TypeRef::new_path( ::HIR::GenericPath(mv$(enum_path), e.m_params.clone()), ::HIR::TypePathBinding::make_Enum(&enm) ) ),
                        {}
                        };
                    for( const auto& arg : var_data )
                    {
                        ft.m_arg_types.push_back( monomorphise_type(sp, enm.m_params, e.m_params, arg.ent) );
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
                fix_param_count(sp, this->context, *e.type, true, e.trait, trait.m_params,  e.trait.m_params);

                // 1. Add trait bound to be checked.
                this->context.add_trait_bound(sp, *e.type,  e.trait.m_path, e.trait.m_params.clone());

                // 2. Locate this item in the trait
                // - If it's an associated `const`, will have to revisit
                auto it = trait.m_values.find( e.item );
                if( it == trait.m_values.end() ) {
                    ERROR(sp, E0000, "`" << e.item << "` is not a value member of trait " << e.trait.m_path);
                }
                TU_MATCH( ::HIR::TraitValueItem, (it->second), (ie),
                (Constant,
                    auto cb = monomorphise_type_get_cb(sp, &*e.type, &e.trait.m_params, nullptr);
                    ::HIR::TypeRef  tmp;
                    const auto& ty = ( monomorphise_type_needed(ie.m_type) ? tmp = monomorphise_type_with(sp, ie.m_type, cb) : ie.m_type );
                    this->context.equate_types(sp, node.m_res_type, ty);
                    ),
                (Static,
                    TODO(sp, "Monomorpise associated static type - " << ie.m_type);
                    ),
                (Function,
                    fix_param_count(sp, this->context, *e.type, false, node.m_path, ie.m_params,  e.params);

                    auto monomorph_cb = monomorphise_type_get_cb(sp, &*e.type, &e.trait.m_params, &e.params);
                    ::HIR::FunctionType ft {
                        ie.m_unsafe, ie.m_abi,
                        box$( monomorphise_type_with(sp, ie.m_return,  monomorph_cb) ),
                        {}
                        };
                    for(const auto& arg : ie.m_args)
                        ft.m_arg_types.push_back( monomorphise_type_with(sp, arg.second,  monomorph_cb) );
                    apply_bounds_as_rules(this->context, sp, ie.m_params, monomorph_cb, /*is_impl_level=*/false);
                    auto ty = ::HIR::TypeRef(mv$(ft));

                    this->context.equate_types(node.span(), node.m_res_type, ty);
                    )
                )
                }
            TU_ARMA(UfcsInherent, e) {
                // TODO: Share code with visit_call_populate_cache

                // - Locate function (and impl block)
                const ::HIR::Function* fcn_ptr = nullptr;
                const ::HIR::Constant* const_ptr = nullptr;
                const ::HIR::TypeImpl* impl_ptr = nullptr;
                // TODO: Support mutiple matches here (if there's a fuzzy match) and retry if so
                unsigned int count = 0;
                this->context.m_crate.find_type_impls(*e.type, context.m_ivars.callback_resolve_infer(),
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
                    fix_param_count(sp, this->context, *e.type, false, node.m_path, fcn_ptr->m_params,  e.params);
                }
                else {
                    fix_param_count(sp, this->context, *e.type, false, node.m_path, const_ptr->m_params,  e.params);
                }

                // If the impl block has parameters, figure out what types they map to
                // - The function params are already mapped (from fix_param_count)
                auto& impl_params = e.impl_params;
                if( impl_ptr->m_params.m_types.size() > 0 )
                {
                    impl_params.m_types.resize( impl_ptr->m_params.m_types.size() );
                    // NOTE: Could be fuzzy.
                    bool r = impl_ptr->m_type.match_test_generics(sp, *e.type, this->context.m_ivars.callback_resolve_infer(), [&](auto idx, const auto& /*name*/, const auto& ty) {
                        assert( idx < impl_params.m_types.size() );
                        impl_params.m_types[idx] = ty.clone();
                        return ::HIR::Compare::Equal;
                        });
                    if(!r)
                    {
                        auto cb = monomorphise_type_get_cb(sp, nullptr, &impl_params, nullptr);
                        auto t = monomorphise_type_with(sp, impl_ptr->m_type, cb);
                        this->context.equate_types(node.span(), t, *e.type);
                    }
                    for(const auto& ty : impl_params.m_types)
                        assert( !( ty.m_data.is_Infer() && ty.m_data.as_Infer().index == ~0u) );
                }


                if( fcn_ptr )
                {
                    // Create monomorphise callback
                    const auto& fcn_params = e.params;
                    auto monomorph_cb = [&](const auto& gt)->const ::HIR::TypeRef& {
                            const auto& ge = gt.m_data.as_Generic();
                            if( ge.binding == 0xFFFF ) {
                                return this->context.get_type(*e.type);
                            }
                            else if( ge.binding < 256 ) {
                                auto idx = ge.binding;
                                if( idx >= impl_params.m_types.size() ) {
                                    BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << impl_params.m_types.size());
                                }
                                return this->context.get_type(impl_params.m_types[idx]);
                            }
                            else if( ge.binding < 512 ) {
                                auto idx = ge.binding - 256;
                                if( idx >= fcn_params.m_types.size() ) {
                                    BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << fcn_params.m_types.size());
                                }
                                return this->context.get_type(fcn_params.m_types[idx]);
                            }
                            else {
                                BUG(sp, "Generic bounding out of total range");
                            }
                        };

                    // Bounds (both impl and fn)
                    apply_bounds_as_rules(this->context, sp, impl_ptr->m_params, monomorph_cb, /*is_impl_level=*/true);
                    apply_bounds_as_rules(this->context, sp, fcn_ptr->m_params, monomorph_cb, /*is_impl_level=*/false);

                    ::HIR::FunctionType ft {
                        fcn_ptr->m_unsafe, fcn_ptr->m_abi,
                        box$( monomorphise_type_with(sp, fcn_ptr->m_return,  monomorph_cb) ),
                        {}
                        };
                    for(const auto& arg : fcn_ptr->m_args)
                        ft.m_arg_types.push_back( monomorphise_type_with(sp, arg.second,  monomorph_cb) );
                    auto ty = ::HIR::TypeRef(mv$(ft));

                    this->context.equate_types(node.span(), node.m_res_type, ty);
                }
                else    // !fcn_ptr, ergo const_ptr
                {
                    assert(const_ptr);
                    auto monomorph_cb = monomorphise_type_get_cb(sp, &*e.type, &impl_params,  &e.params);

                    ::HIR::TypeRef  tmp;
                    const auto& ty = ( monomorphise_type_needed(const_ptr->m_type) ? tmp = monomorphise_type_with(sp, const_ptr->m_type, monomorph_cb) : const_ptr->m_type );

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

            auto _ = this->push_inner_coerce_scoped(true);
            this->closure_ret_types.push_back( &node.m_return );
            node.m_code->visit( *this );
            this->closure_ret_types.pop_back( );
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
                this->context.add_ivars(*e.type);
                this->visit_generic_path(sp, e.trait);
                for(auto& ty : e.params.m_types)
                    this->context.add_ivars(ty);
                ),
            (UfcsUnknown,
                TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
                ),
            (UfcsInherent,
                this->context.add_ivars(*e.type);
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

    // -----------------------------------------------------------------------
    // Revisit Class
    //
    // Handles visiting nodes during inferrence passes
    // -----------------------------------------------------------------------
    class ExprVisitor_Revisit:
        public ::HIR::ExprVisitor
    {
        Context& context;
        bool m_completed;
        /// Tells vistors that inferrence has stalled, and that they can take
        /// more extreme actions (e.g. ignoring an ambigious inherent method
        /// and using a trait method instead)
        bool m_is_fallback;
    public:
        ExprVisitor_Revisit(Context& context, bool fallback=false):
            context(context),
            m_completed(false),
            m_is_fallback(fallback)
        {}

        bool node_completed() const {
            return m_completed;
        }

        void visit(::HIR::ExprNode_Block& node) override {

            const auto is_diverge = [&](const ::HIR::TypeRef& rty)->bool {
                const auto& ty = this->context.get_type(rty);
                // TODO: Search the entire type for `!`? (What about pointers to it? or Option/Result?)
                // - A correct search will search for unconditional (ignoring enums with a non-! variant) non-rawptr instances of ! in the type
                return ty.m_data.is_Diverge();
                };

            assert( !node.m_nodes.empty() );
            const auto& last_ty = this->context.get_type( node.m_nodes.back()->m_res_type );
            DEBUG("_Block: last_ty = " << last_ty);

            bool diverges = false;
            // NOTE: If the final statement in the block diverges, mark this as diverging
            if(const auto* e = last_ty.m_data.opt_Infer())
            {
                switch(e->ty_class)
                {
                case ::HIR::InferClass::Integer:
                case ::HIR::InferClass::Float:
                    diverges = false;
                    break;
                default:
                    this->context.equate_types_from_shadow(node.span(), node.m_res_type);
                    return ;
                }
            }
            else if( is_diverge(last_ty) ) {
                diverges = true;
            }
            else {
                diverges = false;
            }
            // If a statement in this block diverges
            if( diverges ) {
                DEBUG("_Block: diverges, yield !");
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_diverge());
            }
            else {
                DEBUG("_Block: doesn't diverge but doesn't yield a value, yield ()");
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
            }
            this->m_completed = true;
        }
        void visit(::HIR::ExprNode_Asm& node) override {
            // TODO: Revisit for validation
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Return& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Let& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Loop& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_LoopControl& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Match& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_If& node) override {
            no_revisit(node);
        }

        void visit(::HIR::ExprNode_Assign& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_BinOp& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_UniOp& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Borrow& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Cast& node) override {
            const auto& sp = node.span();
            const auto& tgt_ty = this->context.get_type(node.m_res_type);
            const auto& src_ty = this->context.get_type(node.m_value->m_res_type);
            TRACE_FUNCTION_F(src_ty << " as " << tgt_ty);

            if( this->context.m_ivars.types_equal(src_ty, tgt_ty) ) {
                this->m_completed = true;
                return ;
            }

            TU_MATCH_HDRA( (tgt_ty.m_data), {)
            TU_ARMA(Infer, e) {
                // Can't know anything
                //this->m_completed = true;
                DEBUG("- Target type is still _");
                }
            TU_ARMA(Diverge, e) {
                BUG(sp, "");
                }
            TU_ARMA(Primitive, e) {
                // Don't have anything to contribute
                // EXCEPT: `char` can only be casted from `u8` (but what about no-op casts?)
                // - Hint the input (default) to be `u8`
                if( e == ::HIR::CoreType::Char )
                {
                    if(this->m_is_fallback)
                    {
                        this->context.equate_types(sp, src_ty, ::HIR::CoreType::U8);
                    }

                    if( !this->context.get_type(src_ty).m_data.is_Infer() )
                    {
                        this->m_completed = true;
                    }
                }
                else
                {
                    this->m_completed = true;
                }
                }
            TU_ARMA(Path, e) {
                this->context.equate_types_coerce(sp, tgt_ty, node.m_value);
                this->m_completed = true;
                return ;
                }
            TU_ARMA(Generic, e) {
                TODO(sp, "_Cast Generic");
                }
            TU_ARMA(TraitObject, e) {
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                }
            TU_ARMA(ErasedType, e) {
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                }
            TU_ARMA(Array, e) {
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                }
            TU_ARMA(Slice, e) {
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                }
            TU_ARMA(Tuple, e) {
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                }
            TU_ARMA(Borrow, e) {
                // Emit a coercion and delete this revisit
                this->context.equate_types_coerce(sp, tgt_ty, node.m_value);
                this->m_completed = true;
                return ;
                }
            TU_ARMA(Pointer, e) {
                const auto& ity = this->context.get_type(*e.inner);
                TU_MATCH_HDRA( (src_ty.m_data), {)
                default:
                    ERROR(sp, E0000, "Invalid cast to pointer from " << src_ty);
                TU_ARMA(Function, s_e) {
                    // TODO: What is the valid set? *const () and *const u8 at least are allowed
                    if( ity == ::HIR::TypeRef::new_unit() || ity == ::HIR::CoreType::U8 || ity == ::HIR::CoreType::I8 ) {
                        this->m_completed = true;
                    }
                    else if( ity.m_data.is_Infer() ) {
                        // Keep around.
                    }
                    else {
                        ERROR(sp, E0000, "Invalid cast to " << this->context.m_ivars.fmt_type(tgt_ty) << " from " << src_ty);
                    }
                    }
                TU_ARMA(Primitive, s_e) {
                    switch(s_e)
                    {
                    case ::HIR::CoreType::Bool:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        ERROR(sp, E0000, "Invalid cast to pointer from " << src_ty);
                    default:
                        break;
                    }
                    // NOTE: Can't be to a fat pointer though - This is checked by the later pass (once all types are known and thus sized-ness is known)
                    this->m_completed = true;
                    }
                TU_ARMA(Infer, s_e) {
                    switch( s_e.ty_class )
                    {
                    case ::HIR::InferClass::Float:
                        ERROR(sp, E0000, "Invalid cast to pointer from floating point literal");
                    case ::HIR::InferClass::Integer:
                        this->context.equate_types(sp, src_ty, ::HIR::TypeRef(::HIR::CoreType::Usize));
                        this->m_completed = true;
                        break;
                    case ::HIR::InferClass::None:
                    case ::HIR::InferClass::Diverge:
                        break;
                    }
                    }
                TU_ARMA(Borrow, s_e) {
                    // Check class (destination must be weaker) and type
                    if( !(s_e.type >= e.type) ) {
                        ERROR(sp, E0000, "Invalid cast from " << src_ty << " to " << tgt_ty);
                    }
                    const auto& src_inner = this->context.get_type(*s_e.inner);

                    // NOTE: &mut T -> *mut U where T: Unsize<U> is allowed
                    // TODO: Wouldn't this be better served by a coercion point?
                    if( const auto* s_e_i = src_inner.m_data.opt_Infer() )
                    {
                        // If the type is an ivar, possible equate
                        this->context.possible_equate_type_unsize_to(s_e_i->index, *e.inner);
                    }
                    // - NOTE: Crude, and likely to break if ether inner isn't known.
                    else if( src_inner.m_data.is_Array() && *src_inner.m_data.as_Array().inner == *e.inner )
                    {
                        // Allow &[T; n] -> *const T - Convert into two casts
                        auto ty = ::HIR::TypeRef::new_pointer(e.type, src_inner.clone());
                        node.m_value = NEWNODE(ty.clone(), sp, _Cast, mv$(node.m_value), ty.clone());
                        this->m_completed = true;
                    }
                    else
                    {
                        const auto& lang_Unsize = this->context.m_crate.get_lang_item_path(sp, "unsize");
                        bool found = this->context.m_resolve.find_trait_impls(sp, lang_Unsize, ::HIR::PathParams(e.inner->clone()), *s_e.inner, [](auto , auto){ return true; });
                        if( found ) {
                            auto ty = ::HIR::TypeRef::new_borrow(e.type, e.inner->clone());
                            node.m_value = NEWNODE(ty.clone(), sp, _Unsize, mv$(node.m_value), ty.clone());
                            this->context.add_trait_bound(sp, *s_e.inner,  lang_Unsize, ::HIR::PathParams(e.inner->clone()));
                        }
                        else {
                            this->context.equate_types(sp, *e.inner, *s_e.inner);
                        }
                        this->m_completed = true;
                    }
                    }
                TU_ARMA(Pointer, s_e) {
                    // Allow with no link?
                    // TODO: In some rare cases, this ivar could be completely
                    // unrestricted. If in fallback mode
                    const auto& dst_inner = this->context.get_type(*e.inner);
                    if( dst_inner.m_data.is_Infer() )
                    {
                        if(this->m_is_fallback)
                        {
                            DEBUG("- Fallback mode, assume inner types are equal");
                            this->context.equate_types(sp, *e.inner, *s_e.inner);
                        }
                        else
                        {
                            return ;
                        }
                    }
                    else
                    {
                    }
                    this->m_completed = true;
                    }
                }
                }
            TU_ARMA(Function, e) {
                // NOTE: Valid if it's causing a fn item -> fn pointer coercion
                TU_MATCH_HDRA( (src_ty.m_data), {)
                default:
                    ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty) << " to " << this->context.m_ivars.fmt_type(src_ty));
                TU_ARMA(Function, s_e) {
                    // Check that the ABI and unsafety is correct
                    if( s_e.m_abi != e.m_abi || s_e.is_unsafe != e.is_unsafe || s_e.m_arg_types.size() != e.m_arg_types.size() )
                        ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty) << " to " << this->context.m_ivars.fmt_type(src_ty));
                    // TODO: Equate inner types
                    this->context.equate_types(sp, tgt_ty, src_ty);
                    }
                }
                }
            TU_ARMA(Closure, e) {
                BUG(sp, "Attempting to cast to a closure type - impossible");
                }
            }
        }
        void visit(::HIR::ExprNode_Unsize& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Index& node) override {
            const auto& lang_Index = this->context.m_crate.get_lang_item_path(node.span(), "index");
            const auto& val_ty = this->context.get_type(node.m_value->m_res_type);
            //const auto& idx_ty = this->context.get_type(node.m_index->m_res_type);
            const auto& idx_ty = this->context.get_type(node.m_cache.index_ty);
            TRACE_FUNCTION_F("Index: val=" << val_ty << ", idx=" << idx_ty << "");

            this->context.equate_types_from_shadow(node.span(), node.m_res_type);

            // NOTE: Indexing triggers autoderef
            unsigned int deref_count = 0;
            ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
            const auto* current_ty = &node.m_value->m_res_type;
            ::std::vector< ::HIR::TypeRef>  deref_res_types;

            // TODO: (CHECK) rustc doesn't use the index value type when finding the indexable item, mrustc does.
            ::HIR::PathParams   trait_pp;
            trait_pp.m_types.push_back( idx_ty.clone() );
            do {
                const auto& ty = this->context.get_type(*current_ty);
                DEBUG("(Index): (: " << ty << ")[: " << trait_pp.m_types[0] << "]");
                if( ty.m_data.is_Infer() ) {
                    return ;
                }

                ::HIR::TypeRef  possible_index_type;
                ::HIR::TypeRef  possible_res_type;
                unsigned int count = 0;
                bool rv = this->context.m_resolve.find_trait_impls(node.span(), lang_Index, trait_pp, ty, [&](auto impl, auto cmp) {
                    DEBUG("[visit(_Index)] cmp=" << cmp << " - " << impl);
                    possible_res_type = impl.get_type("Output");
                    count += 1;
                    if( cmp == ::HIR::Compare::Equal ) {
                        return true;
                    }
                    possible_index_type = impl.get_trait_ty_param(0);
                    return false;
                    });
                if( rv ) {
                    // TODO: Node's result type could be an &-ptr?
                    this->context.equate_types(node.span(), node.m_res_type,  possible_res_type);
                    break;
                }
                else if( count == 1 ) {
                    assert( possible_index_type != ::HIR::TypeRef() );
                    this->context.equate_types_assoc(node.span(), node.m_res_type,  lang_Index, mv$(trait_pp), ty, "Output", false);
                    break;
                }
                else if( count > 1 ) {
                    // Multiple fuzzy matches, don't keep dereferencing until we know.
                    current_ty = nullptr;
                    break;
                }
                else {
                    // Either no matches, or multiple fuzzy matches
                }

                deref_count += 1;
                current_ty = this->context.m_resolve.autoderef(node.span(), ty,  tmp_type);
                if( current_ty )
                    deref_res_types.push_back( current_ty->clone() );
            } while( current_ty );

            if( current_ty )
            {
                DEBUG("Found impl on type " << *current_ty << " with " << deref_count << " derefs");
                assert( deref_count == deref_res_types.size() );
                for(auto& ty_r : deref_res_types)
                {
                    auto ty = mv$(ty_r);

                    node.m_value = this->context.create_autoderef( mv$(node.m_value), mv$(ty) );
                    context.m_ivars.get_type(node.m_value->m_res_type);
                }

                m_completed = true;
            }
        }
        void visit(::HIR::ExprNode_Deref& node) override {
            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            TRACE_FUNCTION_F("Deref: ty=" << ty);

            TU_MATCH_HDRA( (ty.m_data), {)
            default: {
                const auto& op_trait = this->context.m_crate.get_lang_item_path(node.span(), "deref");
                this->context.equate_types_assoc(node.span(), node.m_res_type,  op_trait, {}, node.m_value->m_res_type.clone(), "Target");
                }
            TU_ARMA(Infer, e) {
                // Keep trying
                this->context.equate_types_from_shadow(node.span(), node.m_res_type);
                return ;
                }
            TU_ARMA(Borrow, e) {
                // - Not really needed, but this is cheaper.
                this->context.equate_types(node.span(), node.m_res_type, *e.inner);
                }
            TU_ARMA(Pointer, e) {
                // TODO: Figure out if this node is in an unsafe block.
                this->context.equate_types(node.span(), node.m_res_type, *e.inner);
                }
            }
            this->m_completed = true;
        }
        void visit_emplace_129(::HIR::ExprNode_Emplace& node) {
            const auto& sp = node.span();
            const auto& exp_ty = this->context.get_type(node.m_res_type);
            const auto& data_ty = this->context.get_type(node.m_value->m_res_type);
            const auto& placer_ty = this->context.get_type(node.m_place->m_res_type);
            const auto& lang_Boxed = this->context.m_lang_Box;
            TRACE_FUNCTION_F("exp_ty=" << exp_ty << ", data_ty=" << data_ty << ", placer_ty" << placer_ty);
            ASSERT_BUG(sp, node.m_type == ::HIR::ExprNode_Emplace::Type::Boxer, "1.29 mode with non-box _Emplace node");
            ASSERT_BUG(sp, placer_ty == ::HIR::TypeRef::new_unit(), "1.29 mode with box in syntax - placer type is " << placer_ty);

            ASSERT_BUG(sp, !lang_Boxed.m_components.empty(), "`owbed_box` not present when `box` operator used");

            // NOTE: `owned_box` shouldn't point to anything but a struct
            const auto& str = this->context.m_crate.get_struct_by_path(sp, lang_Boxed);
            // TODO: Store this type to avoid having to construct it every pass
            auto boxed_ty = ::HIR::TypeRef::new_path( ::HIR::GenericPath(lang_Boxed, {data_ty.clone()}), &str );

            // TODO: is there anyting special about this node that might need revisits?

            context.equate_types(sp, exp_ty, boxed_ty);
            this->m_completed = true;
        }
        void visit_emplace_119(::HIR::ExprNode_Emplace& node) {
            const auto& sp = node.span();
            const auto& exp_ty = this->context.get_type(node.m_res_type);
            const auto& data_ty = this->context.get_type(node.m_value->m_res_type);
            const auto& placer_ty = this->context.get_type(node.m_place->m_res_type);
            auto node_ty = node.m_type;
            TRACE_FUNCTION_F("_Emplace: exp_ty=" << exp_ty << ", data_ty=" << data_ty << ", placer_ty" << placer_ty);

            switch(node_ty)
            {
            case ::HIR::ExprNode_Emplace::Type::Noop:
                BUG(sp, "No-op _Emplace in typeck?");
                break;
            case ::HIR::ExprNode_Emplace::Type::Placer: {
                if( placer_ty.m_data.is_Infer() )
                {
                    // Can't do anything, the place is still unknown
                    DEBUG("Place unknown, wait");
                    //this->context.equate_types_to_shadow(sp, placer_ty);
                    this->context.equate_types_to_shadow(sp, data_ty);
                    return ;
                }

                // Where P = `placer_ty` and D = `data_ty`
                // Result type is <<P as Placer<D>>::Place as InPlace<D>>::Owner
                const auto& lang_Placer = this->context.m_crate.get_lang_item_path(sp, "placer_trait");
                const auto& lang_InPlace = this->context.m_crate.get_lang_item_path(sp, "in_place_trait");
                // - Bound P: Placer<D>
                this->context.equate_types_assoc(sp, {}, lang_Placer, ::make_vec1(data_ty.clone()), placer_ty, "");
                // - 
                auto place_ty = ::HIR::TypeRef::new_path( ::HIR::Path(placer_ty.clone(), ::HIR::GenericPath(lang_Placer, ::HIR::PathParams(data_ty.clone())), "Place"), {} );
                this->context.equate_types_assoc(sp, node.m_res_type, lang_InPlace, ::make_vec1(data_ty.clone()), place_ty, "Owner");
                break; }
            case ::HIR::ExprNode_Emplace::Type::Boxer: {
                const ::HIR::TypeRef* inner_ty;
                if( exp_ty.m_data.is_Infer() ) {
                    // If the expected result type is still an ivar, nothing can be done

                    // HACK: Add a possibility of the result type being ``Box<`data_ty`>``
                    // - This only happens if the `owned_box` lang item is present and this node is a `box` operation
                    const auto& lang_Boxed = this->context.m_lang_Box;
                    if( ! lang_Boxed.m_components.empty() )
                    {
                        // NOTE: `owned_box` shouldn't point to anything but a struct
                        const auto& str = this->context.m_crate.get_struct_by_path(sp, lang_Boxed);
                        // TODO: Store this type to avoid having to construct it every pass
                        auto boxed_ty = ::HIR::TypeRef::new_path( ::HIR::GenericPath(lang_Boxed, {data_ty.clone()}), &str );
                        this->context.possible_equate_type_coerce_from( exp_ty.m_data.as_Infer().index, boxed_ty );
                    }
                    this->context.equate_types_to_shadow(sp, data_ty);
                    return ;
                }
                // Assert that the expected result is a Path::Generic type.
                if( ! exp_ty.m_data.is_Path() ) {
                    ERROR(sp, E0000, "box/in can only produce GenericPath types, got " << exp_ty);
                }
                const auto& path = exp_ty.m_data.as_Path().path;
                if( ! path.m_data.is_Generic() ) {
                    ERROR(sp, E0000, "box/in can only produce GenericPath types, got " << exp_ty);
                }
                const auto& gpath = path.m_data.as_Generic();

                if( gpath.m_params.m_types.size() > 0 )
                {
                    // TODO: If there's only one, check if it's a valid coercion target, if not don't bother making the coercion.

                    // Take a copy of the type with all type parameters replaced with new ivars
                    auto newpath = ::HIR::GenericPath(gpath.m_path);
                    for( const auto& t : gpath.m_params.m_types )
                    {
                        (void)t;
                        newpath.m_params.m_types.push_back( this->context.m_ivars.new_ivar_tr() );
                    }
                    auto newty = ::HIR::TypeRef::new_path( mv$(newpath), exp_ty.m_data.as_Path().binding.clone() );

                    // Turn this revisit into a coercion point with the new result type
                    // - Mangle this node to be a passthrough to a copy of itself.

                    node.m_value = ::HIR::ExprNodeP( new ::HIR::ExprNode_Emplace(node.span(), node.m_type, mv$(node.m_place), mv$(node.m_value)) );
                    node.m_type = ::HIR::ExprNode_Emplace::Type::Noop;
                    node.m_value->m_res_type = mv$(newty);
                    inner_ty = &node.m_value->m_res_type;

                    this->context.equate_types_coerce(sp, exp_ty, node.m_value);
                }
                else
                {
                    inner_ty = &exp_ty;
                }

                // Insert a trait bound on the result type to impl `Placer/Boxer`
                //this->context.equate_types_assoc(sp, {}, ::HIR::SimplePath("core", { "ops", "Boxer" }), ::make_vec1(data_ty.clone()), *inner_ty, "");
                this->context.equate_types_assoc(sp, data_ty, this->context.m_crate.get_lang_item_path(sp, "boxed_trait"), {}, *inner_ty, "Data");
                break; }
            }

            this->m_completed = true;
        }
        void visit(::HIR::ExprNode_Emplace& node) override {
            switch(gTargetVersion)
            {
            case TargetVersion::Rustc1_19:
                return visit_emplace_119(node);
            case TargetVersion::Rustc1_29:
                return visit_emplace_129(node);
            }
            throw "BUG: Unhandled target version";
        }

        void visit(::HIR::ExprNode_TupleVariant& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_CallPath& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_CallValue& node) override {
            //const auto& sp = node.span();
            const auto& ty_o = this->context.get_type(node.m_value->m_res_type);
            TRACE_FUNCTION_F("CallValue: ty=" << ty_o);

            //this->context.equate_types_from_shadow(node.span(), node.m_res_type);
            this->context.equate_types_shadow_strong(node.span(), node.m_res_type);
            // - Shadow (prevent ivar guessing) every parameter
            for( const auto& arg_ty : node.m_arg_ivars ) {
                this->context.equate_types_to_shadow(node.span(), arg_ty);
            }

            if( ty_o.m_data.is_Infer() ) {
                // - Don't even bother
                return ;
            }

            const auto& lang_FnOnce = this->context.m_crate.get_lang_item_path(node.span(), "fn_once");
            const auto& lang_FnMut  = this->context.m_crate.get_lang_item_path(node.span(), "fn_mut");
            const auto& lang_Fn     = this->context.m_crate.get_lang_item_path(node.span(), "fn");


            // 1. Create a param set with a single tuple (of all argument types)
            ::HIR::PathParams   trait_pp;
            {
                ::std::vector< ::HIR::TypeRef>  arg_types;
                for(const auto& arg_ty : node.m_arg_ivars) {
                    arg_types.push_back( this->context.get_type(arg_ty).clone() );
                }
                trait_pp.m_types.push_back( ::HIR::TypeRef( mv$(arg_types) ) );
            }

            unsigned int deref_count = 0;
            ::HIR::TypeRef  tmp_type;   // for autoderef
            const auto* ty_p = &ty_o;

            bool keep_looping = false;
            do  // } while( keep_looping );
            {
                // Reset at the start of each loop
                keep_looping = false;

                const auto& ty = *ty_p;
                DEBUG("- ty = " << ty);
                if( const auto* e = ty.m_data.opt_Closure() )
                {
                    for( const auto& arg : e->m_arg_types )
                        node.m_arg_types.push_back(arg.clone());
                    node.m_arg_types.push_back(e->m_rettype->clone());
                    node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Unknown;
                }
                else if( const auto* e = ty.m_data.opt_Function() )
                {
                    for( const auto& arg : e->m_arg_types )
                        node.m_arg_types.push_back(arg.clone());
                    node.m_arg_types.push_back(e->m_rettype->clone());
                    node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Fn;
                }
                else if( ty.m_data.is_Infer() )
                {
                    // No idea yet
                    return ;
                }
                else if( const auto* e = ty.m_data.opt_Borrow() )
                {
                    deref_count++;
                    ty_p = &this->context.get_type(*e->inner);
                    DEBUG("Deref " << ty << " -> " << *ty_p);
                    keep_looping = true;
                    continue;
                }
                // TODO: If autoderef is possible, do it and continue. Only look for impls once autoderef fails
                else
                {
                    ::HIR::TypeRef  fcn_args_tup;
                    ::HIR::TypeRef  fcn_ret;

                    // TODO: Use `find_trait_impls` instead of two different calls
                    // - This will get the TraitObject impl search too

                    // Locate an impl of FnOnce (exists for all other Fn* traits)
                    // TODO: Sometimes there's impls that just forward for wrappers, which can lead to incorrect rules
                    // e.g. `&mut _` (where `_ = Box<...>`) later will pick the FnMut impl for `&mut T: FnMut` - but Box doesn't have those forwarding impls
                    // - Maybe just keep applying auto-deref until it's no longer possible?
                    unsigned int count = 0;
                    this->context.m_resolve.find_trait_impls(node.span(), lang_FnOnce, trait_pp, ty, [&](auto impl, auto cmp)->bool {
                        // TODO: Don't accept if too fuzzy
                        count++;

                        auto tup = impl.get_trait_ty_param(0);
                        if (!tup.m_data.is_Tuple())
                            ERROR(node.span(), E0000, "FnOnce expects a tuple argument, got " << tup);
                        fcn_args_tup = mv$(tup);

                        fcn_ret = impl.get_type("Output");
                        DEBUG("[visit:_CallValue] fcn_args_tup=" << fcn_args_tup << ", fcn_ret=" << fcn_ret);
                        return cmp == ::HIR::Compare::Equal;
                    });
                    DEBUG("Found " << count << " impls of FnOnce");
                    if(count > 1) {
                        return;
                    }
                    if( count == 1 )
                    {

                        // 3. Locate the most permissive implemented Fn* trait (Fn first, then FnMut, then assume just FnOnce)
                        // NOTE: Borrowing is added by the expansion to CallPath
                        if( this->context.m_resolve.find_trait_impls(node.span(), lang_Fn, trait_pp, ty, [&](auto impl, auto cmp) {
                                // TODO: Take the value of `cmp` into account
                                fcn_ret = impl.get_type("Output");
                                return true;
                                //return cmp == ::HIR::Compare::Equal;
                            }) )
                        {
                            DEBUG("-- Using Fn");
                            node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Fn;

                            this->context.equate_types_assoc(node.span(), node.m_res_type, lang_Fn, ::make_vec1(fcn_args_tup.clone()), ty, "Output");
                        }
                        else if( this->context.m_resolve.find_trait_impls(node.span(), lang_FnMut, trait_pp, ty, [&](auto impl, auto cmp) {
                                // TODO: Take the value of `cmp` into account
                                fcn_ret = impl.get_type("Output");
                                return true;
                                //return cmp == ::HIR::Compare::Equal;
                            }) )
                        {
                            DEBUG("-- Using FnMut");
                            node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::FnMut;

                            this->context.equate_types_assoc(node.span(), node.m_res_type, lang_FnMut, ::make_vec1(fcn_args_tup.clone()), ty, "Output");
                        }
                        else
                        {
                            DEBUG("-- Using FnOnce (default)");
                            node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::FnOnce;

                            this->context.equate_types_assoc(node.span(), node.m_res_type, lang_FnOnce, ::make_vec1(fcn_args_tup.clone()), ty, "Output");
                        }

                        // If the return type wasn't found in the impls, emit it as a UFCS
                        if(fcn_ret == ::HIR::TypeRef())
                        {
                            fcn_ret = ::HIR::TypeRef::new_path(::HIR::Path(::HIR::Path::Data::make_UfcsKnown({
                                box$(ty.clone()),
                                // - Clone argument tuple, as it's stolen into cache below
                                ::HIR::GenericPath(lang_FnOnce, ::HIR::PathParams(fcn_args_tup.clone())),
                                "Output",
                                {}
                                })),
                                {}
                                );
                        }
                    }
                    else if( const auto* e = ty.m_data.opt_Borrow() )
                    {
                        deref_count++;
                        ty_p = &this->context.get_type(*e->inner);
                        DEBUG("Deref " << ty << " -> " << *ty_p);
                        keep_looping = true;
                        continue;
                    }
                    else
                    {
                        if( !ty.m_data.is_Generic() )
                        {
                            bool found = this->context.m_resolve.find_trait_impls_crate(node.span(), lang_FnOnce, trait_pp, ty, [&](auto impl, auto cmp)->bool {
                                if (cmp == ::HIR::Compare::Fuzzy)
                                    TODO(node.span(), "Handle fuzzy match - " << impl);

                                auto tup = impl.get_trait_ty_param(0);
                                if (!tup.m_data.is_Tuple())
                                    ERROR(node.span(), E0000, "FnOnce expects a tuple argument, got " << tup);
                                fcn_args_tup = mv$(tup);
                                fcn_ret = impl.get_type("Output");
                                ASSERT_BUG(node.span(), fcn_ret != ::HIR::TypeRef(), "Impl didn't have a type for Output - " << impl);
                                return true;
                            });
                            if (found) {
                                // Fill cache and leave the TU_MATCH
                                node.m_arg_types = mv$(fcn_args_tup.m_data.as_Tuple());
                                node.m_arg_types.push_back(mv$(fcn_ret));
                                node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Unknown;
                                break; // leaves TU_MATCH
                            }
                        }
                        if( const auto* next_ty_p = this->context.m_resolve.autoderef(node.span(), ty, tmp_type) )
                        {
                            DEBUG("Deref (autoderef) " << ty << " -> " << *next_ty_p);
                            deref_count++;
                            ty_p = next_ty_p;
                            keep_looping = true;
                            continue;
                        }

                        // Didn't find anything. Error?
                        ERROR(node.span(), E0000, "Unable to find an implementation of Fn*" << trait_pp << " for " << this->context.m_ivars.fmt_type(ty));
                    }

                    node.m_arg_types = mv$(fcn_args_tup.m_data.as_Tuple());
                    node.m_arg_types.push_back(mv$(fcn_ret));
                }
            } while( keep_looping );

            if( deref_count > 0 )
            {
                ty_p = &ty_o;
                while(deref_count-- > 0)
                {
                    ty_p = this->context.m_resolve.autoderef(node.span(), *ty_p, tmp_type);
                    assert(ty_p);
                    node.m_value = this->context.create_autoderef( mv$(node.m_value), ty_p->clone() );
                }
            }

            assert( node.m_arg_types.size() == node.m_args.size() + 1 );
            for(unsigned int i = 0; i < node.m_args.size(); i ++)
            {
                this->context.equate_types(node.span(), node.m_arg_types[i], node.m_arg_ivars[i]);
            }
            this->context.equate_types(node.span(), node.m_res_type, node.m_arg_types.back());
            this->m_completed = true;
        }
        void visit(::HIR::ExprNode_CallMethod& node) override {
            const auto& sp = node.span();

            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            TRACE_FUNCTION_F("(CallMethod) {" << this->context.m_ivars.fmt_type(ty) << "}." << node.m_method << node.m_params
                << "(" << FMT_CB(os, for( const auto& arg_node : node.m_args ) os << this->context.m_ivars.fmt_type(arg_node->m_res_type) << ", ";) << ")"
                << " -> " << this->context.m_ivars.fmt_type(node.m_res_type)
                );

            // Make sure that no mentioned types are inferred until this method is known
            this->context.equate_types_from_shadow(node.span(), node.m_res_type);
            for( const auto& arg_node : node.m_args ) {
                this->context.equate_types_to_shadow(node.span(), arg_node->m_res_type);
            }

            // Using autoderef, locate this method on the type
            // TODO: Obtain a list of avaliable methods at that level?
            // - If running in a mode after stablise (before defaults), fall
            // back to trait if the inherent is still ambigious.
            ::std::vector<::std::pair<TraitResolution::AutoderefBorrow, ::HIR::Path>> possible_methods;
            unsigned int deref_count = this->context.m_resolve.autoderef_find_method(node.span(), node.m_traits, node.m_trait_param_ivars, ty, node.m_method.c_str(),  possible_methods);
        try_again:
            if( deref_count != ~0u )
            {
                DEBUG("possible_methods = " << possible_methods);
                if( possible_methods.empty() )
                {
                    //ERROR(sp, E0000, "Could not find method `" << method_name << "` on type `" << top_ty << "`");
                    ERROR(sp, E0000, "No applicable methods for {" << this->context.m_ivars.fmt_type(ty) << "}." << node.m_method);
                }
                if( possible_methods.size() > 1 )
                {
                    // TODO: What do do when there's multiple possibilities?
                    // - Should use available information to strike them down
                    // > Try and equate the return type and the arguments, if any fail then move on to the next possibility?
                    // > ONLY if those arguments/return are generic
                    //
                    // Possible causes of multiple entries
                    // - Multiple distinct traits with the same method
                    //   > If `self` is concretely known, this is an error (and shouldn't happen in well-formed code).
                    // - Multiple inherent methods on a type
                    //   > These would have to have different type parmeters
                    // - Multiple trait bounds (same trait, different type params)
                    //   > Guess at the type params, then discard if there's a conflict?
                    //   > De-duplicate same traits?
                    //
                    //
                    // So: To be able to prune the list, we need to check the type parameters for the trait/type/impl 

                    // De-duplcate traits in this list.
                    // - If the self type and the trait name are the same, replace with an entry using placeholder
                    //   ivars (node.m_trait_param_ivars)
                    for(auto it_1 = possible_methods.begin(); it_1 != possible_methods.end(); ++ it_1)
                    {
                        if( it_1->first != possible_methods.front().first )
                        {
                            it_1 = possible_methods.erase(it_1) - 1;
                        }
                    }
                    for(auto it_1 = possible_methods.begin(); it_1 != possible_methods.end(); ++ it_1)
                    {
                        if( !it_1->second.m_data.is_UfcsKnown() )
                            continue;
                        bool was_found = false;
                        auto& e1 = it_1->second.m_data.as_UfcsKnown();
                        for(auto it_2 = it_1 + 1; it_2 != possible_methods.end(); ++ it_2)
                        {
                            if( !it_2->second.m_data.is_UfcsKnown() )
                                continue;
                            if( it_2->second == it_1->second ) {
                                it_2 = possible_methods.erase(it_2) - 1;
                                continue ;
                            }
                            const auto& e2 = it_2->second.m_data.as_UfcsKnown();

                            // TODO: If the trait is the same, but the type differs, pick the first?
                            if( e1.trait == e2.trait ) {
                                DEBUG("Duplicate trait, different type - " << e1.trait << " for " << *e1.type << " or " << *e2.type << ", picking the first");
                                it_2 = possible_methods.erase(it_2) - 1;
                                continue ;
                            }
                            if( *e1.type != *e2.type )
                                continue;
                            if( e1.trait.m_path != e2.trait.m_path )
                                continue;
                            assert( !(e1.trait.m_params == e2.trait.m_params) );

                            DEBUG("Duplicate trait in possible_methods - " << it_1->second << " and " << it_2->second);
                            if( !was_found )
                            {
                                was_found = true;
                                const auto& ivars = node.m_trait_param_ivars;
                                unsigned int n_params = e1.trait.m_params.m_types.size();
                                assert(n_params <= ivars.size());
                                ::HIR::PathParams   trait_params;
                                trait_params.m_types.reserve( n_params );
                                for(unsigned int i = 0; i < n_params; i++) {
                                    trait_params.m_types.push_back( ::HIR::TypeRef::new_infer(ivars[i], ::HIR::InferClass::None) );
                                    //ASSERT_BUG(sp, m_ivars.get_type( trait_params.m_types.back() ).m_data.as_Infer().index == ivars[i], "A method selection ivar was bound");
                                }
                                // If one of these was already using the placeholder ivars, then maintain the one with the palceholders
                                if( e1.trait.m_params != trait_params )
                                {
                                    e1.trait.m_params = mv$(trait_params);
                                }

                                it_2 = possible_methods.erase(it_2) - 1;
                            }
                        }
                    }
                }
                assert( !possible_methods.empty() );
                if( possible_methods.size() != 1 && possible_methods.front().second.m_data.is_UfcsKnown() )
                {
                    DEBUG("- Multiple options, deferring");
                    // TODO: If the type is fully known, then this is an error.
                    return;
                }
                auto& ad_borrow = possible_methods.front().first;
                auto& fcn_path = possible_methods.front().second;
                DEBUG("- deref_count = " << deref_count << ", fcn_path = " << fcn_path);

                node.m_method_path = mv$(fcn_path);
                // NOTE: Steals the params from the node
                TU_MATCH(::HIR::Path::Data, (node.m_method_path.m_data), (e),
                (Generic,
                    ),
                (UfcsUnknown,
                    ),
                (UfcsKnown,
                    e.params = mv$(node.m_params);
                    //fix_param_count(sp, this->context, node.m_method_path, fcn.m_params, e.params);
                    ),
                (UfcsInherent,
                    e.params = mv$(node.m_params);
                    //fix_param_count(sp, this->context, node.m_method_path, fcn.m_params, e.params);
                    )
                )

                // TODO: If this is ambigious, and it's an inherent, and in fallback mode - fall down to the next trait method.
                if( !visit_call_populate_cache(this->context, node.span(), node.m_method_path, node.m_cache) ) {
                    DEBUG("- AMBIGUOUS - Trying again later");
                    // Move the params back
                    TU_MATCH(::HIR::Path::Data, (node.m_method_path.m_data), (e),
                    (Generic, ),
                    (UfcsUnknown, ),
                    (UfcsKnown,
                        node.m_params = mv$(e.params);
                        ),
                    (UfcsInherent,
                        node.m_params = mv$(e.params);
                        )
                    )
                    if( this->m_is_fallback && fcn_path.m_data.is_UfcsInherent() )
                    {
                        //possible_methods.erase(possible_methods.begin());
                        while( !possible_methods.empty() && possible_methods.front().second.m_data.is_UfcsInherent() )
                        {
                            possible_methods.erase(possible_methods.begin());
                        }
                        if( !possible_methods.empty() )
                        {
                            DEBUG("Infference stall, try again with " << possible_methods.front().second);
                            goto try_again;
                        }
                    }
                    return ;
                }
                DEBUG("> m_method_path = " << node.m_method_path);

                assert( node.m_cache.m_arg_types.size() >= 1);

                if( node.m_args.size()+1 != node.m_cache.m_arg_types.size() - 1 ) {
                    ERROR(node.span(), E0000, "Incorrect number of arguments to " << node.m_method_path
                        << " - exp " << node.m_cache.m_arg_types.size()-2 << " got " << node.m_args.size());
                }
                DEBUG("- fcn_path=" << node.m_method_path);

                // --- Check and equate self/arguments/return
                DEBUG("node.m_cache.m_arg_types = " << node.m_cache.m_arg_types);
                // NOTE: `Self` is equated after autoderef and autoref
                for(unsigned int i = 0; i < node.m_args.size(); i ++)
                {
                    // 1+ because it's a method call (#0 is Self)
                    DEBUG("> ARG " << i << " : " << node.m_cache.m_arg_types[1+i]);
                    this->context.equate_types_coerce(sp, node.m_cache.m_arg_types[1+i], node.m_args[i]);
                }
                DEBUG("> Ret : " << node.m_cache.m_arg_types.back());
                this->context.equate_types(sp, node.m_res_type,  node.m_cache.m_arg_types.back());

                // Add derefs
                if( deref_count > 0 )
                {
                    assert( deref_count < (1<<16) );    // Just some sanity.
                    DEBUG("- Inserting " << deref_count << " dereferences");
                    // Get dereferencing!
                    auto& node_ptr = node.m_value;
                    ::HIR::TypeRef  tmp_ty;
                    const ::HIR::TypeRef*   cur_ty = &node_ptr->m_res_type;
                    while( deref_count-- )
                    {
                        auto span = node_ptr->span();
                        cur_ty = this->context.m_resolve.autoderef(span, *cur_ty, tmp_ty);
                        assert(cur_ty);
                        auto ty = cur_ty->clone();

                        node.m_value = this->context.create_autoderef( mv$(node.m_value), mv$(ty) );
                    }
                }

                // Autoref
                if( ad_borrow != TraitResolution::AutoderefBorrow::None )
                {
                    ::HIR::BorrowType   bt = ::HIR::BorrowType::Shared;
                    switch(ad_borrow)
                    {
                    case TraitResolution::AutoderefBorrow::None:    throw "";
                    case TraitResolution::AutoderefBorrow::Shared:   bt = ::HIR::BorrowType::Shared; break;
                    case TraitResolution::AutoderefBorrow::Unique:   bt = ::HIR::BorrowType::Unique; break;
                    case TraitResolution::AutoderefBorrow::Owned :   bt = ::HIR::BorrowType::Owned ; break;
                    }

                    auto ty = ::HIR::TypeRef::new_borrow(bt, node.m_value->m_res_type.clone());
                    DEBUG("- Ref (cmd) " << &*node.m_value << " -> " << ty);
                    auto span = node.m_value->span();
                    node.m_value = NEWNODE(mv$(ty), span, _Borrow,  bt, mv$(node.m_value) );
                }
                else
                {
                }

                // Equate the type for `self` (to ensure that Self's type params infer correctly)
                this->context.equate_types(sp, node.m_cache.m_arg_types[0], node.m_value->m_res_type);

                this->m_completed = true;
            }
        }
        void visit(::HIR::ExprNode_Field& node) override {
            const auto& field_name = node.m_field;
            TRACE_FUNCTION_F("(Field) name=" << field_name << ", ty = " << this->context.m_ivars.fmt_type(node.m_value->m_res_type));

            //this->context.equate_types_from_shadow(node.span(), node.m_res_type);
            this->context.equate_types_shadow_strong(node.span(), node.m_res_type);

            ::HIR::TypeRef  out_type;

            // Using autoderef, locate this field
            unsigned int deref_count = 0;
            ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
            const auto* current_ty = &node.m_value->m_res_type;
            ::std::vector< ::HIR::TypeRef>  deref_res_types;

            // TODO: autoderef_find_field?
            do {
                const auto& ty = this->context.m_ivars.get_type(*current_ty);
                if( ty.m_data.is_Infer() ) {
                    DEBUG("Hit ivar, returning early");
                    return ;
                }
                if(ty.m_data.is_Path() && ty.m_data.as_Path().binding.is_Unbound()) {
                    DEBUG("Hit unbound path, returning early");
                    return ;
                }
                if( this->context.m_resolve.find_field(node.span(), ty, field_name.c_str(), out_type) ) {
                    this->context.equate_types(node.span(), node.m_res_type, out_type);
                    break;
                }

                deref_count += 1;
                current_ty = this->context.m_resolve.autoderef(node.span(), ty,  tmp_type);
                if( current_ty )
                    deref_res_types.push_back( current_ty->clone() );
            } while(current_ty);

            if( !current_ty )
            {
                ERROR(node.span(), E0000, "Couldn't find the field " << field_name << " in " << this->context.m_ivars.fmt_type(node.m_value->m_res_type));
            }

            assert( deref_count == deref_res_types.size() );
            for(unsigned int i = 0; i < deref_res_types.size(); i ++ )
            {
                auto ty = mv$(deref_res_types[i]);
                DEBUG("- Deref " << &*node.m_value << " -> " << ty);
                if( node.m_value->m_res_type.m_data.is_Array() ) {
                    BUG(node.span(), "Field access from array/slice?");
                }
                node.m_value = NEWNODE(mv$(ty), node.span(), _Deref,  mv$(node.m_value));
                context.m_ivars.get_type(node.m_value->m_res_type);
            }

            m_completed = true;
        }

        void visit(::HIR::ExprNode_Literal& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_PathValue& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Variable& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ConstParam& node) override {
            no_revisit(node);
        }

        void visit(::HIR::ExprNode_StructLiteral& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_UnionLiteral& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Tuple& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ArrayList& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ArraySized& node) override {
            no_revisit(node);
        }

        void visit(::HIR::ExprNode_Closure& node) override {
            no_revisit(node);
        }
    private:
        void no_revisit(::HIR::ExprNode& node) {
            BUG(node.span(), "Node revisit unexpected - " << typeid(node).name());
        }
    };  // class ExprVisitor_Revisit

    // -----------------------------------------------------------------------
    // Post-inferrence visitor
    //
    // Saves the inferred types into the HIR expression tree, and ensures that
    // all types were inferred.
    // -----------------------------------------------------------------------
    class ExprVisitor_Apply:
        public ::HIR::ExprVisitorDef
    {
        const Context& context;
        const HMTypeInferrence& ivars;
    public:
        ExprVisitor_Apply(const Context& context):
            context(context),
            ivars(context.m_ivars)
        {
        }
        void visit_node_ptr(::HIR::ExprPtr& node_ptr)
        {
            auto& node = *node_ptr;
            const char* node_ty = typeid(node).name();

            TRACE_FUNCTION_FR(&node << " " << &node << " " << node_ty << " : " << node.m_res_type, node_ty);
            this->check_type_resolved_top(node.span(), node.m_res_type);
            DEBUG(node_ty << " : = " << node.m_res_type);

            node_ptr->visit(*this);

            for( auto& ty : node_ptr.m_bindings )
                this->check_type_resolved_top(node.span(), ty);

            for( auto& ty : node_ptr.m_erased_types )
                this->check_type_resolved_top(node.span(), ty);
        }
        void visit_node_ptr(::HIR::ExprNodeP& node_ptr) override {
            auto& node = *node_ptr;
            const char* node_ty = typeid(node).name();
            TRACE_FUNCTION_FR(&node_ptr << " " << &node << " " << node_ty << " : " << node.m_res_type, &node << " " << node_ty);
            this->check_type_resolved_top(node.span(), node.m_res_type);
            DEBUG(node_ty << " : = " << node.m_res_type);
            ::HIR::ExprVisitorDef::visit_node_ptr(node_ptr);
        }

        void visit_pattern(const Span& sp, ::HIR::Pattern& pat) override {
            TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (e),
            (
                ),
            (Value,
                TU_IFLET( ::HIR::Pattern::Value, (e.val), Named, ve,
                    this->check_type_resolved_path(sp, ve.path);
                )
                ),
            (Range,
                TU_IFLET( ::HIR::Pattern::Value, e.start, Named, ve,
                    this->check_type_resolved_path(sp, ve.path);
                )
                TU_IFLET( ::HIR::Pattern::Value, e.end, Named, ve,
                    this->check_type_resolved_path(sp, ve.path);
                )
                ),
            (StructValue,
                this->check_type_resolved_genericpath(sp, e.path);
                ),
            (StructTuple,
                this->check_type_resolved_genericpath(sp, e.path);
                ),
            (Struct,
                this->check_type_resolved_genericpath(sp, e.path);
                ),
            (EnumValue,
                this->check_type_resolved_genericpath(sp, e.path);
                ),
            (EnumTuple,
                this->check_type_resolved_genericpath(sp, e.path);
                ),
            (EnumStruct,
                this->check_type_resolved_genericpath(sp, e.path);
                )
            )
            ::HIR::ExprVisitorDef::visit_pattern(sp, pat);
        }

        void visit(::HIR::ExprNode_Block& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            if( node.m_value_node )
            {
                check_types_equal(node.span(), node.m_res_type, node.m_value_node->m_res_type);
            }
            // If the last node diverges (yields `!`) then this block can yield `!` (or anything)
            else if( ! node.m_nodes.empty() && node.m_nodes.back()->m_res_type == ::HIR::TypeRef::new_diverge() )
            {
            }
            else
            {
                // Non-diverging (empty, or with a non-diverging last node) blocks must yield `()`
                check_types_equal(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
            }
        }

        void visit(::HIR::ExprNode_Let& node) override {
            this->check_type_resolved_top(node.span(), node.m_type);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_Closure& node) override {
            for(auto& arg : node.m_args)
                this->check_type_resolved_top(node.span(), arg.second);
            this->check_type_resolved_top(node.span(), node.m_return);
            ::HIR::ExprVisitorDef::visit(node);
        }

        void visit_callcache(const Span&sp, ::HIR::ExprCallCache& cache)
        {
            for(auto& ty : cache.m_arg_types)
                this->check_type_resolved_top(sp, ty);
        }
        void visit(::HIR::ExprNode_CallPath& node) override {
            this->visit_callcache(node.span(), node.m_cache);

            this->check_type_resolved_path(node.span(), node.m_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_CallMethod& node) override {
            this->visit_callcache(node.span(), node.m_cache);

            this->check_type_resolved_path(node.span(), node.m_method_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_CallValue& node) override {
            for(auto& ty : node.m_arg_types)
                this->check_type_resolved_top(node.span(), ty);
            ::HIR::ExprVisitorDef::visit(node);
        }

        void visit(::HIR::ExprNode_PathValue& node) override {
            this->check_type_resolved_path(node.span(), node.m_path);
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override {
            this->check_type_resolved_genericpath(node.span(), node.m_path);
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override {
            this->check_type_resolved_genericpath(node.span(), node.m_real_path);
            for(auto& ty : node.m_value_types) {
                if( ty != ::HIR::TypeRef() ) {
                    this->check_type_resolved_top(node.span(), ty);
                }
            }

            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_UnionLiteral& node) override {
            this->check_type_resolved_pp(node.span(), node.m_path.m_params, ::HIR::TypeRef());
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            this->check_type_resolved_pp(node.span(), node.m_path.m_params, ::HIR::TypeRef());
            for(auto& ty : node.m_arg_types) {
                if( ty != ::HIR::TypeRef() ) {
                    this->check_type_resolved_top(node.span(), ty);
                }
            }

            ::HIR::ExprVisitorDef::visit(node);
        }

        void visit(::HIR::ExprNode_Literal& node) override {
            TU_MATCH(::HIR::ExprNode_Literal::Data, (node.m_data), (e),
            (Integer,
                ASSERT_BUG(node.span(), node.m_res_type.m_data.is_Primitive(), "Integer _Literal didn't return primitive - " << node.m_res_type);
                e.m_type = node.m_res_type.m_data.as_Primitive();
                ),
            (Float,
                ASSERT_BUG(node.span(), node.m_res_type.m_data.is_Primitive(), "Float Literal didn't return primitive - " << node.m_res_type);
                e.m_type = node.m_res_type.m_data.as_Primitive();
                ),
            (Boolean,
                ),
            (ByteString,
                ),
            (String,
                )
            )
        }
    private:
        void check_type_resolved_top(const Span& sp, ::HIR::TypeRef& ty) const {
            check_type_resolved(sp, ty, ty);
            ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
        }
        void check_type_resolved_pp(const Span& sp, ::HIR::PathParams& pp, const ::HIR::TypeRef& top_type) const {
            for(auto& ty : pp.m_types)
                check_type_resolved(sp, ty, top_type);
        }
        void check_type_resolved_path(const Span& sp, ::HIR::Path& path) const {
            auto tmp = ::HIR::TypeRef::new_path(path.clone(), {});
            check_type_resolved_path(sp, path, tmp);
            TU_MATCH(::HIR::Path::Data, (path.m_data), (pe),
            (Generic,
                for(auto& ty : pe.m_params.m_types)
                    ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
                ),
            (UfcsInherent,
                *pe.type = this->context.m_resolve.expand_associated_types(sp, mv$(*pe.type));
                for(auto& ty : pe.params.m_types)
                    ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
                for(auto& ty : pe.impl_params.m_types)
                    ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
                ),
            (UfcsKnown,
                *pe.type = this->context.m_resolve.expand_associated_types(sp, mv$(*pe.type));
                for(auto& ty : pe.params.m_types)
                    ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
                for(auto& ty : pe.trait.m_params.m_types)
                    ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
                ),
            (UfcsUnknown,
                throw "";
                )
            )
        }
        void check_type_resolved_path(const Span& sp, ::HIR::Path& path, const ::HIR::TypeRef& top_type) const {
            TU_MATCH(::HIR::Path::Data, (path.m_data), (pe),
            (Generic,
                check_type_resolved_pp(sp, pe.m_params, top_type);
                ),
            (UfcsInherent,
                check_type_resolved(sp, *pe.type, top_type);
                check_type_resolved_pp(sp, pe.params, top_type);
                check_type_resolved_pp(sp, pe.impl_params, top_type);
                ),
            (UfcsKnown,
                check_type_resolved(sp, *pe.type, top_type);
                check_type_resolved_pp(sp, pe.trait.m_params, top_type);
                check_type_resolved_pp(sp, pe.params, top_type);
                ),
            (UfcsUnknown,
                ERROR(sp, E0000, "UfcsUnknown " << path << " left in " << top_type);
                )
            )
        }
        void check_type_resolved_genericpath(const Span& sp, ::HIR::GenericPath& path) const {
            auto tmp = ::HIR::TypeRef::new_path(path.clone(), {});
            check_type_resolved_pp(sp, path.m_params, tmp);
        }
        void check_type_resolved(const Span& sp, ::HIR::TypeRef& ty, const ::HIR::TypeRef& top_type) const {
            class InnerVisitor:
                public HIR::Visitor
            {
                const ExprVisitor_Apply& parent;
                const Span& sp;
                const ::HIR::TypeRef& top_type;

            public:
                InnerVisitor(const ExprVisitor_Apply& parent, const Span& sp, const ::HIR::TypeRef& top_type)
                    :parent(parent)
                    ,sp(sp)
                    ,top_type(top_type)
                {
                }

                void visit_path(::HIR::Path& path, HIR::Visitor::PathContext pc) override
                {
                    if( path.m_data.is_UfcsUnknown() )
                        ERROR(sp, E0000, "UfcsUnknown " << path << " left in " << top_type);
                    ::HIR::Visitor::visit_path(path, pc);
                }
                void visit_type(::HIR::TypeRef& ty) override
                {
                    if( ty.m_data.is_Infer() )
                    {
                        auto new_ty = parent.ivars.get_type(ty).clone();
                        // - Move over before checking, so that the source type mentions the correct ivar
                        ty = mv$(new_ty);
                        if( ty.m_data.is_Infer() ) {
                            ERROR(sp, E0000, "Failed to infer type " << ty << " in "  << top_type);
                        }
                    }
                    
                    ::HIR::Visitor::visit_type(ty);
                }
            };

            InnerVisitor v(*this, sp, top_type);
            v.visit_type(ty);
        }

        void check_types_equal(const Span& sp, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r) const
        {
            DEBUG(sp << " - " << l << " == " << r);
            if( r.m_data.is_Diverge() ) {
                // Diverge on the right is always valid
                // - NOT on the left, because `!` can become everything, but nothing can become `!`
            }
            else if( l != r ) {
                ERROR(sp, E0000, "Type mismatch - " << l << " != " << r);
            }
            else {
                // All good
            }
        }
    }; // class ExprVisitor_Apply

    class ExprVisitor_Print:
        public ::HIR::ExprVisitor
    {
        const Context& context;
        ::std::ostream& m_os;
    public:
        ExprVisitor_Print(const Context& context, ::std::ostream& os):
            context(context),
            m_os(os)
        {}

        void visit(::HIR::ExprNode_Block& node) override {
            m_os << "_Block {" << context.m_ivars.fmt_type(node.m_nodes.back()->m_res_type) << "}";
        }
        void visit(::HIR::ExprNode_Asm& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Return& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Let& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Loop& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_LoopControl& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Match& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_If& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Assign& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_BinOp& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_UniOp& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Borrow& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Cast& node) override {
            m_os << "_Cast {" << context.m_ivars.fmt_type(node.m_value->m_res_type) << "}";
        }
        void visit(::HIR::ExprNode_Unsize& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Index& node) override {
            m_os << "_Index {" << fmt_res_ty(*node.m_value) << "}[{" << fmt_res_ty(*node.m_index) << "}]";
        }
        void visit(::HIR::ExprNode_Deref& node) override {
            m_os << "_Deref {" << fmt_res_ty(*node.m_value) << "}";
        }
        void visit(::HIR::ExprNode_Emplace& node) override {
            m_os << "_Emplace(" << fmt_res_ty(*node.m_value) << " in " << fmt_res_ty(*node.m_place) << ")";
        }

        void visit(::HIR::ExprNode_TupleVariant& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_CallPath& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_CallValue& node) override {
            m_os << "_CallValue {" << fmt_res_ty(*node.m_value) << "}(";
            for(const auto& arg : node.m_args)
                m_os << "{" << fmt_res_ty(*arg) << "}, ";
            m_os << ")";
        }
        void visit(::HIR::ExprNode_CallMethod& node) override {
            m_os << "_CallMethod {" << fmt_res_ty(*node.m_value) << "}." << node.m_method << "(";
            for(const auto& arg : node.m_args)
                m_os << "{" << fmt_res_ty(*arg) << "}, ";
            m_os << ")";
        }
        void visit(::HIR::ExprNode_Field& node) override {
            m_os << "_Field {" << fmt_res_ty(*node.m_value) << "}." << node.m_field;
        }

        void visit(::HIR::ExprNode_Literal& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_PathValue& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Variable& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ConstParam& node) override {
            no_revisit(node);
        }

        void visit(::HIR::ExprNode_StructLiteral& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_UnionLiteral& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Tuple& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ArrayList& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ArraySized& node) override {
            no_revisit(node);
        }

        void visit(::HIR::ExprNode_Closure& node) override {
            no_revisit(node);
        }
    private:
        HMTypeInferrence::FmtType fmt_res_ty(const ::HIR::ExprNode& n) {
            return context.m_ivars.fmt_type(n.m_res_type);
        }
        void no_revisit(::HIR::ExprNode& n) {
            throw "";
        }
    }; // class ExprVisitor_Print
}


void Context::dump() const {
    DEBUG("--- Variables");
    for(unsigned int i = 0; i < m_bindings.size(); i ++)
    {
        DEBUG(i << " " << m_bindings[i].name << ": " << this->m_ivars.fmt_type(m_bindings[i].ty));
    }
    DEBUG("--- Ivars");
    m_ivars.dump();
    DEBUG("--- CS Context - " << link_coerce.size() << " Coercions, " << link_assoc.size() << " associated, " << to_visit.size() << " nodes, " << adv_revisits.size() << " callbacks");
    for(const auto& vp : link_coerce) {
        const auto& v = *vp;
        //DEBUG(v);
        DEBUG("R" << v.rule_idx << " " << this->m_ivars.fmt_type(v.left_ty) << " := " << v.right_node_ptr << " " << &**v.right_node_ptr << " (" << this->m_ivars.fmt_type((*v.right_node_ptr)->m_res_type) << ")");
    }
    for(const auto& v : link_assoc) {
        DEBUG(v);
    }
    for(const auto& v : to_visit) {
        DEBUG(&*v << " " << FMT_CB(os, { ExprVisitor_Print ev(*this, os); v->visit(ev); }) << " -> " << this->m_ivars.fmt_type(v->m_res_type));
    }
    for(const auto& v : adv_revisits) {
        DEBUG(FMT_CB(ss, v->fmt(ss);));
    }
    DEBUG("---");
}

void Context::equate_types(const Span& sp, const ::HIR::TypeRef& li, const ::HIR::TypeRef& ri) {

    if( li == ri || this->m_ivars.get_type(li) == this->m_ivars.get_type(ri) ) {
        DEBUG(li << " == " << ri);
        return ;
    }

    // Instantly apply equality
    TRACE_FUNCTION_F(li << " == " << ri);

    ASSERT_BUG(sp, !type_contains_impl_placeholder(ri), "Type contained an impl placeholder parameter - " << ri);
    ASSERT_BUG(sp, !type_contains_impl_placeholder(li), "Type contained an impl placeholder parameter - " << li);

    ::HIR::TypeRef  l_tmp;
    ::HIR::TypeRef  r_tmp;
    const auto& l_t = this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(li), l_tmp);
    const auto& r_t = this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(ri), r_tmp);

    if( l_t.m_data.is_Diverge() && !r_t.m_data.is_Infer() ) {
        return ;
    }
    if( r_t.m_data.is_Diverge() && !l_t.m_data.is_Infer() ) {
        return;
    }

    equate_types_inner(sp, l_t, r_t);
}

void Context::equate_types_inner(const Span& sp, const ::HIR::TypeRef& li, const ::HIR::TypeRef& ri) {

    if( li == ri || this->m_ivars.get_type(li) == this->m_ivars.get_type(ri) ) {
        return ;
    }

    // Check if the type contains a replacable associated type
    ::HIR::TypeRef  l_tmp;
    ::HIR::TypeRef  r_tmp;
    const auto& l_t = (li.m_data.is_Infer() ? this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(li), l_tmp) : li);
    const auto& r_t = (ri.m_data.is_Infer() ? this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(ri), r_tmp) : ri);
    if( l_t == r_t ) {
        return ;
    }

    // If either side is still a UfcsUnkonw after `expand_associated_types`, then emit an assoc bound instead of damaging ivars
    if(const auto* r_e = r_t.m_data.opt_Path())
    {
        if(const auto* rpe = r_e->path.m_data.opt_UfcsKnown())
        {
            if( r_e->binding.is_Unbound() ) {
                this->equate_types_assoc(sp, l_t,  rpe->trait.m_path, rpe->trait.m_params.clone().m_types, *rpe->type,  rpe->item.c_str());
                return ;
            }
        }
    }
    if(const auto* l_e = l_t.m_data.opt_Path())
    {
        if(const auto* lpe = l_e->path.m_data.opt_UfcsKnown())
        {
            if( l_e->binding.is_Unbound() ) {
                this->equate_types_assoc(sp, r_t,  lpe->trait.m_path, lpe->trait.m_params.clone().m_types, *lpe->type,  lpe->item.c_str());
                return ;
            }
        }
    }

    DEBUG("- l_t = " << l_t << ", r_t = " << r_t);
    if(const auto* r_e = r_t.m_data.opt_Infer())
    {
        if(const auto* l_e = l_t.m_data.opt_Infer())
        {
            // If both are infer, unify the two ivars (alias right to point to left)
            // TODO: Unify sized flags

            if( (r_e->index < m_ivars_sized.size() && m_ivars_sized.at(r_e->index))
              ||(l_e->index < m_ivars_sized.size() && m_ivars_sized.at(l_e->index))
                )
            {
                this->require_sized(sp, l_t);
                this->require_sized(sp, r_t);
            }

            this->m_ivars.ivar_unify(l_e->index, r_e->index);
        }
        else {
            // Righthand side is infer, alias it to the left
            if( r_e->index < m_ivars_sized.size() && m_ivars_sized.at(r_e->index) ) {
                this->require_sized(sp, l_t);
            }
            this->m_ivars.set_ivar_to(r_e->index, l_t.clone());
        }
    }
    else
    {
        if(const auto* l_e = l_t.m_data.opt_Infer())
        {
            // Lefthand side is infer, alias it to the right
            if( l_e->index < m_ivars_sized.size() && m_ivars_sized.at(l_e->index) ) {
                this->require_sized(sp, r_t);
            }
            this->m_ivars.set_ivar_to(l_e->index, r_t.clone());
        }
        else {
            // Helper function for Path and TraitObject
            auto equality_typeparams = [&](const ::HIR::PathParams& l, const ::HIR::PathParams& r) {
                    if( l.m_types.size() != r.m_types.size() ) {
                        ERROR(sp, E0000, "Type mismatch in type params `" << l << "` and `" << r << "`");
                    }
                    for(unsigned int i = 0; i < l.m_types.size(); i ++)
                    {
                        this->equate_types_inner(sp, l.m_types[i], r.m_types[i]);
                    }
                };

            // If either side is !, return early
            // TODO: Should ! end up in an ivar?
            #if 1
            if( l_t.m_data.is_Diverge() && r_t.m_data.is_Diverge() ) {
                return ;
            }
            /*else if( l_t.m_data.is_Diverge() ) {
                if(const auto* l_e = li.m_data.opt_Infer()) {
                    this->m_ivars.set_ivar_to(l_e->index, r_t.clone());
                }
                return ;
            }*/
            else if( r_t.m_data.is_Diverge() ) {
                if(const auto* r_e = ri.m_data.opt_Infer()) {
                    this->m_ivars.set_ivar_to(r_e->index, l_t.clone());
                }
                return ;
            }
            else {
            }
            #else
            if( l_t.m_data.is_Diverge() || r_t.m_data.is_Diverge() ) {
                return ;
            }
            #endif

            if( l_t.m_data.tag() != r_t.m_data.tag() ) {
                ERROR(sp, E0000, "Type mismatch between " << this->m_ivars.fmt_type(l_t) << " and " << this->m_ivars.fmt_type(r_t));
            }
            TU_MATCH_HDRA( (l_t.m_data, r_t.m_data), {)
            TU_ARMA(Infer, l_e, r_e) {
                throw "";
                }
            TU_ARMA(Diverge, l_e, r_e) {
                // ignore?
                }
            TU_ARMA(Primitive, l_e, r_e) {
                if( l_e != r_e ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                }
            TU_ARMA(Path, l_e, r_e) {
                if( l_e.path.m_data.tag() != r_e.path.m_data.tag() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                TU_MATCH(::HIR::Path::Data, (l_e.path.m_data, r_e.path.m_data), (lpe, rpe),
                (Generic,
                    if( lpe.m_path != rpe.m_path ) {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    }
                    equality_typeparams(lpe.m_params, rpe.m_params);
                    ),
                (UfcsInherent,
                    equality_typeparams(lpe.params, rpe.params);
                    if( lpe.item != rpe.item )
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    this->equate_types_inner(sp, *lpe.type, *rpe.type);
                    ),
                (UfcsKnown,
                    if( lpe.trait.m_path != rpe.trait.m_path || lpe.item != rpe.item )
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    equality_typeparams(lpe.trait.m_params, rpe.trait.m_params);
                    equality_typeparams(lpe.params, rpe.params);
                    this->equate_types_inner(sp, *lpe.type, *rpe.type);
                    ),
                (UfcsUnknown,
                    // TODO: If the type is fully known, locate a suitable trait item
                    equality_typeparams(lpe.params, rpe.params);
                    if( lpe.item != rpe.item )
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    this->equate_types_inner(sp, *lpe.type, *rpe.type);
                    )
                )
                }
            TU_ARMA(Generic, l_e, r_e) {
                if( l_e.binding != r_e.binding ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                }
            TU_ARMA(TraitObject, l_e, r_e) {
                if( l_e.m_trait.m_path.m_path != r_e.m_trait.m_path.m_path ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                equality_typeparams(l_e.m_trait.m_path.m_params, r_e.m_trait.m_path.m_params);
                for(auto it_l = l_e.m_trait.m_type_bounds.begin(), it_r = r_e.m_trait.m_type_bounds.begin(); it_l != l_e.m_trait.m_type_bounds.end(); it_l++, it_r++ ) {
                    if( it_l->first != it_r->first ) {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - associated bounds differ");
                    }
                    this->equate_types_inner(sp, it_l->second, it_r->second);
                }
                if( l_e.m_markers.size() != r_e.m_markers.size() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - trait counts differ");
                }
                // TODO: Is this list sorted in any way? (if it's not sorted, this could fail when source does Send+Any instead of Any+Send)
                for(unsigned int i = 0; i < l_e.m_markers.size(); i ++ )
                {
                    auto& l_p = l_e.m_markers[i];
                    auto& r_p = r_e.m_markers[i];
                    if( l_p.m_path != r_p.m_path ) {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    }
                    equality_typeparams(l_p.m_params, r_p.m_params);
                }
                // NOTE: Lifetime is ignored
                }
            TU_ARMA(ErasedType, l_e, r_e) {
                ASSERT_BUG(sp, l_e.m_origin != ::HIR::SimplePath(), "ErasedType " << l_t << " wasn't bound to its origin");
                ASSERT_BUG(sp, r_e.m_origin != ::HIR::SimplePath(), "ErasedType " << r_t << " wasn't bound to its origin");
                // TODO: Ivar equate origin
                if( l_e.m_origin != r_e.m_origin ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - different source");
                }
                }
            TU_ARMA(Array, l_e, r_e) {
                this->equate_types_inner(sp, *l_e.inner, *r_e.inner);
                if( l_e.size != r_e.size ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - sizes differ");
                }
                }
            TU_ARMA(Slice, l_e, r_e) {
                this->equate_types_inner(sp, *l_e.inner, *r_e.inner);
                }
            TU_ARMA(Tuple, l_e, r_e) {
                if( l_e.size() != r_e.size() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Tuples are of different length");
                }
                for(unsigned int i = 0; i < l_e.size(); i ++)
                {
                    this->equate_types_inner(sp, l_e[i], r_e[i]);
                }
                }
            TU_ARMA(Borrow, l_e, r_e) {
                if( l_e.type != r_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Borrow classes differ");
                }
                this->equate_types_inner(sp, *l_e.inner, *r_e.inner);
                }
            TU_ARMA(Pointer, l_e, r_e) {
                if( l_e.type != r_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Pointer mutability differs");
                }
                this->equate_types_inner(sp, *l_e.inner, *r_e.inner);
                }
            TU_ARMA(Function, l_e, r_e) {
                if( l_e.is_unsafe != r_e.is_unsafe
                    || l_e.m_abi != r_e.m_abi
                    || l_e.m_arg_types.size() != r_e.m_arg_types.size()
                    )
                {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                this->equate_types_inner(sp, *l_e.m_rettype, *r_e.m_rettype);
                for(unsigned int i = 0; i < l_e.m_arg_types.size(); i ++ ) {
                    this->equate_types_inner(sp, l_e.m_arg_types[i], r_e.m_arg_types[i]);
                }
                }
            TU_ARMA(Closure, l_e, r_e) {
                if( l_e.m_arg_types.size() != r_e.m_arg_types.size() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                this->equate_types_inner(sp, *l_e.m_rettype, *r_e.m_rettype);
                for( unsigned int i = 0; i < l_e.m_arg_types.size(); i ++ )
                {
                    this->equate_types_inner(sp, l_e.m_arg_types[i], r_e.m_arg_types[i]);
                }
                }
            }
        }
    }
}

void Context::add_binding_inner(const Span& sp, const ::HIR::PatternBinding& pb, ::HIR::TypeRef type)
{
    assert( pb.is_valid() );
    switch( pb.m_type )
    {
    case ::HIR::PatternBinding::Type::Move:
        this->add_var( sp, pb.m_slot, pb.m_name, mv$(type) );
        break;
    case ::HIR::PatternBinding::Type::Ref:
        this->add_var( sp, pb.m_slot, pb.m_name, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, mv$(type)) );
        break;
    case ::HIR::PatternBinding::Type::MutRef:
        this->add_var( sp, pb.m_slot, pb.m_name, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, mv$(type)) );
        break;
    }
}

// NOTE: Mutates the pattern to add ivars to contained paths
void Context::handle_pattern(const Span& sp, ::HIR::Pattern& pat, const ::HIR::TypeRef& type)
{
    TRACE_FUNCTION_F("pat = " << pat << ", type = " << type);

    // TODO: 1.29 includes "match ergonomics" which allows automatic insertion of borrow/deref when matching
    // - Handling this will make pattern matching slightly harder (all patterns needing revisist)
    // - BUT: New bindings will still be added as usualin this pass.
    // - Any use of `&` (or `ref`?) in the pattern disables match ergonomics for the entire pattern.
    //   - Does `box` also do this disable?
    //
    //
    // - Add a counter to each pattern indicting how many implicit borrows/derefs are applied.
    // - When this function is called, check if the pattern is eligable for pattern auto-ref/deref
    // - Detect if the pattern uses & or ref. If it does, then invoke the existing code
    // - Otherwise, register a revisit for the pattern


    struct H2 {
        static bool has_ref_or_borrow(const Span& sp, const ::HIR::Pattern& pat) {
            // TODO: Turns out that this isn't valid. See libsyntax 1.29
            // - ref `rustc-1.29.0-src/src/libsyntax/print/pprust.rs` 2911, `&Option` matched with `Some(ref foo)`
            //if( pat.m_binding.is_valid() && pat.m_binding.m_type != ::HIR::PatternBinding::Type::Move ) {
            //    return true;
            //}
            if( pat.m_data.is_Ref() ) {
                return true;
            }
            bool rv = false;
            TU_MATCHA( (pat.m_data), (e),
            (Any,
                ),
            (Value,
                ),
            (Range,
                ),
            (Box,
                rv |= H2::has_ref_or_borrow(sp, *e.sub);
                ),
            (Ref,
                rv |= H2::has_ref_or_borrow(sp, *e.sub);
                ),
            (Tuple,
                for(const auto& subpat : e.sub_patterns)
                    rv |= H2::has_ref_or_borrow(sp, subpat);
                ),
            (SplitTuple,
                for(auto& subpat : e.leading) {
                    rv |= H2::has_ref_or_borrow(sp, subpat);
                }
                for(auto& subpat : e.trailing) {
                    rv |= H2::has_ref_or_borrow(sp, subpat);
                }
                ),
            (Slice,
                for(auto& sub : e.sub_patterns)
                    rv |= H2::has_ref_or_borrow(sp, sub);
                ),
            (SplitSlice,
                for(auto& sub : e.leading)
                    rv |= H2::has_ref_or_borrow(sp, sub);
                for(auto& sub : e.trailing)
                    rv |= H2::has_ref_or_borrow(sp, sub);
                ),

            // - Enums/Structs
            (StructValue,
                ),
            (StructTuple,
                for(const auto& subpat : e.sub_patterns)
                    rv |= H2::has_ref_or_borrow(sp, subpat);
                ),
            (Struct,
                for( auto& field_pat : e.sub_patterns )
                    rv |= H2::has_ref_or_borrow(sp, field_pat.second);
                ),
            (EnumValue,
                ),
            (EnumTuple,
                for(const auto& subpat : e.sub_patterns)
                    rv |= H2::has_ref_or_borrow(sp, subpat);
                ),
            (EnumStruct,
                for( auto& field_pat : e.sub_patterns )
                    rv |= H2::has_ref_or_borrow(sp, field_pat.second);
                )
            )
            return rv;
        }
    };

    // 1. Determine if this pattern can apply auto-ref/deref
    if( pat.m_data.is_Any() ) {
        // `_` pattern, no destructure/match, so no auto-ref/deref
        // - TODO: Does this do auto-borrow too?
        if( pat.m_binding.is_valid() ) {
            this->add_binding_inner(sp, pat.m_binding, type.clone());
        }
        return ;
    }

    // NOTE: Even if the top-level is a binding, and even if the top-level type is fully known, match ergonomics
    // still applies.
    if( TARGETVER_1_29 ) { //&& ! H2::has_ref_or_borrow(sp, pat) ) {
        // There's not a `&` or `ref` in the pattern, and we're targeting 1.29
        // - Run the match ergonomics handler
        // TODO: Default binding mode can be overridden back to "move" with `mut`

        struct MatchErgonomicsRevisit:
            public Revisitor
        {
            Span    sp;
            ::HIR::TypeRef  m_outer_ty;
            ::HIR::Pattern& m_pattern;
            ::HIR::PatternBinding::Type m_outer_mode;

            mutable ::std::vector<::HIR::TypeRef>   m_temp_ivars;
            mutable ::HIR::TypeRef  m_possible_type;

            MatchErgonomicsRevisit(Span sp, ::HIR::TypeRef outer, ::HIR::Pattern& pat, ::HIR::PatternBinding::Type binding_mode=::HIR::PatternBinding::Type::Move):
                sp(mv$(sp)), m_outer_ty(mv$(outer)),
                m_pattern(pat),
                m_outer_mode(binding_mode)
            {}

            const Span& span() const override {
                return sp;
            }
            void fmt(::std::ostream& os) const override {
                os << "MatchErgonomicsRevisit { " << m_pattern << " : " << m_outer_ty << " }";
            }
            bool revisit(Context& context, bool is_fallback_mode) override {
                TRACE_FUNCTION_F("Match ergonomics - " << m_pattern << " : " << m_outer_ty << (is_fallback_mode ? " (fallback)": ""));
                m_outer_ty = context.m_resolve.expand_associated_types(sp, mv$(m_outer_ty));
                return this->revisit_inner_real(context, m_pattern, m_outer_ty, m_outer_mode, is_fallback_mode);
            }
            // TODO: Recurse into inner patterns, creating new revisitors?
            // - OR, could just recurse on it.
            // 
            // Recusring incurs costs on every iteration, but is less expensive the first time around
            // New revisitors are cheaper when inferrence takes multiple iterations, but takes longer first time.
            bool revisit_inner(Context& context, ::HIR::Pattern& pattern, const ::HIR::TypeRef& type, ::HIR::PatternBinding::Type binding_mode) const
            {
                if( !revisit_inner_real(context, pattern, type, binding_mode, false) )
                {
                    DEBUG("Add revisit for " << pattern << " : " << type << "(mode = " << (int)binding_mode << ")");
                    context.add_revisit_adv( box$(( MatchErgonomicsRevisit { sp, type.clone(), pattern, binding_mode } )) );
                }
                return true;
            }
            ::HIR::TypeRef get_possible_type_val(Context& context, ::HIR::Pattern::Value& pv) const
            {
                TU_MATCH_HDR( (pv), {)
                TU_ARM(pv, Integer, ve) {
                    if( ve.type == ::HIR::CoreType::Str ) {
                        return ::HIR::TypeRef::new_infer(context.m_ivars.new_ivar(), ::HIR::InferClass::Integer);
                    }
                    return ve.type;
                    }
                TU_ARM(pv, Float, ve) {
                    if( ve.type == ::HIR::CoreType::Str ) {
                        return ::HIR::TypeRef::new_infer(context.m_ivars.new_ivar(), ::HIR::InferClass::Float);
                    }
                    return ve.type;
                    }
                TU_ARM(pv, String, ve) {
                    return ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ::HIR::CoreType::Str);
                    }
                TU_ARM(pv, ByteString, ve) {
                    // TODO: ByteString patterns can match either &[u8] or &[u8; N]
                    //return ::HIR::TypeRef::new_borrow(
                    //        ::HIR::BorrowType::Shared,
                    //        ::HIR::TypeRef::new_slice(::HIR::CoreType::U8)
                    //        );
                    return ::HIR::TypeRef();
                    }
                TU_ARM(pv, Named, ve) {
                    // TODO: Look up the path and get the type
                    return ::HIR::TypeRef();
                    }
                }
                throw "";
            }
            const ::HIR::TypeRef& get_possible_type(Context& context, ::HIR::Pattern& pattern) const
            {
                if( m_possible_type == ::HIR::TypeRef() )
                {
                    ::HIR::TypeRef  possible_type;
                    // Get a potential type from the pattern, and set as a possibility.
                    // - Note, this is only if no derefs were applied
                    TU_MATCH_HDR( (pattern.m_data), { )
                    TU_ARM(pattern.m_data, Any, pe) {
                        // No type information.
                        }
                    TU_ARM(pattern.m_data, Value, pe) {
                        possible_type = get_possible_type_val(context, pe.val);
                        }
                    TU_ARM(pattern.m_data, Range, pe) {
                        possible_type = get_possible_type_val(context, pe.start);
                        if( possible_type == ::HIR::TypeRef() ) {
                            possible_type = get_possible_type_val(context, pe.end);
                        }
                        else {
                            // TODO: Check that the type from .end matches .start
                        }
                        }
                    TU_ARM(pattern.m_data, Box, pe) {
                        // TODO: Get type info (Box<_>) ?
                        // - Is this possible? Shouldn't a box pattern disable ergonomics?
                        }
                    TU_ARM(pattern.m_data, Ref, pe) {
                        BUG(sp, "Match ergonomics - & pattern");
                        }
                    TU_ARM(pattern.m_data, Tuple, e) {
                        // Get type info `(T, U, ...)`
                        if( m_temp_ivars.size() != e.sub_patterns.size() ) {
                            for(size_t i = 0; i < e.sub_patterns.size(); i ++)
                                m_temp_ivars.push_back( context.m_ivars.new_ivar_tr() );
                        }
                        decltype(m_temp_ivars)  tuple;
                        for(const auto& ty : m_temp_ivars)
                            tuple.push_back(ty.clone());
                        possible_type = ::HIR::TypeRef( ::std::move(tuple) );
                        }
                    TU_ARM(pattern.m_data, SplitTuple, pe) {
                        // Can't get type information, tuple size is unkown
                        }
                    TU_ARM(pattern.m_data, Slice, e) {
                        // Can be either a [T] or [T; n]. Can't provide a hint
                        }
                    TU_ARM(pattern.m_data, SplitSlice, pe) {
                        // Can be either a [T] or [T; n]. Can't provide a hint
                        }
                    TU_ARM(pattern.m_data, StructValue, e) {
                        context.add_ivars_params( e.path.m_params );
                        possible_type = ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypePathBinding(e.binding));
                        }
                    TU_ARM(pattern.m_data, StructTuple, e) {
                        context.add_ivars_params( e.path.m_params );
                        possible_type = ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypePathBinding(e.binding));
                        }
                    TU_ARM(pattern.m_data, Struct, e) {
                        context.add_ivars_params( e.path.m_params );
                        possible_type = ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypePathBinding(e.binding));
                        }
                    TU_ARM(pattern.m_data, EnumValue, e) {
                        context.add_ivars_params( e.path.m_params );
                        possible_type = ::HIR::TypeRef::new_path(get_parent_path(e.path), ::HIR::TypePathBinding(e.binding_ptr));
                        }
                    TU_ARM(pattern.m_data, EnumTuple, e) {
                        context.add_ivars_params( e.path.m_params );
                        possible_type = ::HIR::TypeRef::new_path(get_parent_path(e.path), ::HIR::TypePathBinding(e.binding_ptr));
                        }
                    TU_ARM(pattern.m_data, EnumStruct, e) {
                        context.add_ivars_params( e.path.m_params );
                        possible_type = ::HIR::TypeRef::new_path(get_parent_path(e.path), ::HIR::TypePathBinding(e.binding_ptr));
                        }
                    }
                    m_possible_type = ::std::move(possible_type);
                }
                return m_possible_type;
            }
            bool revisit_inner_real(Context& context, ::HIR::Pattern& pattern, const ::HIR::TypeRef& type, ::HIR::PatternBinding::Type binding_mode, bool is_fallback) const
            {
                TRACE_FUNCTION_F(pattern << " : " << type);

                // Binding applies to the raw input type (not after dereferencing)
                if( pattern.m_binding.is_valid() )
                {
                    // - Binding present, use the current binding mode
                    if( pattern.m_binding.m_type == ::HIR::PatternBinding::Type::Move )
                    {
                        pattern.m_binding.m_type = binding_mode;
                    }
                    ::HIR::TypeRef  tmp;
                    const ::HIR::TypeRef* binding_type = nullptr;
                    switch(pattern.m_binding.m_type)
                    {
                    case ::HIR::PatternBinding::Type::Move:
                        binding_type = &type;
                        break;
                    case ::HIR::PatternBinding::Type::MutRef:
                        // NOTE: Needs to deref and borrow to get just `&mut T` (where T isn't a &mut T)
                        binding_type = &(tmp = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, type.clone()));
                        break;
                    case ::HIR::PatternBinding::Type::Ref:
                        // NOTE: Needs to deref and borrow to get just `&mut T` (where T isn't a &mut T)
                        binding_type = &(tmp = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, type.clone()));
                        break;
                    default:
                        TODO(sp, "Assign variable type using mode " << (int)binding_mode << " and " << type);
                    }
                    assert(binding_type);
                    context.equate_types(sp, context.get_var(sp, pattern.m_binding.m_slot), *binding_type);
                }

                // For `_` patterns, there's nothing to match, so they just succeed with no derefs
                if( pattern.m_data.is_Any() )
                {
                    return true;
                }

                if( auto* pe = pattern.m_data.opt_Ref() )
                {
                    // Require a &-ptr (hard requirement), then visit sub-pattern
                    auto inner_ty = context.m_ivars.new_ivar_tr();
                    auto new_ty = ::HIR::TypeRef::new_borrow( pe->type, inner_ty.clone() );
                    context.equate_types(sp, type, new_ty);

                    return this->revisit_inner( context, *pe->sub, inner_ty, binding_mode );
                }

                // If the type is a borrow, then count derefs required for the borrow
                // - If the first non-borrow inner is an ivar, return false
                unsigned n_deref = 0;
                ::HIR::BorrowType   bt = ::HIR::BorrowType::Owned;
                const auto* ty_p = &context.get_type(type);
                while( ty_p->m_data.is_Borrow() ) {
                    DEBUG("bt " << bt << ", " << ty_p->m_data.as_Borrow().type);
                    bt = ::std::min(bt, ty_p->m_data.as_Borrow().type);
                    ty_p = &context.get_type( *ty_p->m_data.as_Borrow().inner );
                    n_deref ++;
                }
                DEBUG("- " << n_deref << " derefs of class " << bt << " to get " << *ty_p);
                if( ty_p->m_data.is_Infer() || TU_TEST1(ty_p->m_data, Path, .binding.is_Unbound()) )
                {
                    // Still pure infer, can't do anything
                    // - What if it's a literal?

                    // TODO: Don't do fallback if the ivar is marked as being hard blocked
                    if( const auto* te = ty_p->m_data.opt_Infer() )
                    {
                        if( te->index < context.possible_ivar_vals.size()
                            && context.possible_ivar_vals[te->index].force_disable
                            )
                        {
                            MatchErgonomicsRevisit::disable_possibilities_on_bindings(sp, context, pattern);
                            return false;
                        }
                    }

                    // If there's no dereferences done, then add a possible unsize type
                    const ::HIR::TypeRef& possible_type = get_possible_type(context, pattern);
                    if( possible_type != ::HIR::TypeRef() )
                    {
                        DEBUG("n_deref = " << n_deref << ", possible_type = " << possible_type);
                        const ::HIR::TypeRef* possible_type_p = &possible_type;
                        // Unwrap borrows as many times as we've already dereferenced
                        for(size_t i = 0; i < n_deref && possible_type_p; i ++) {
                            if( const auto* te = possible_type_p->m_data.opt_Borrow() ) {
                                possible_type_p = &*te->inner;
                            }
                            else {
                                possible_type_p = nullptr;
                            }
                        }
                        if( possible_type_p )
                        {
                            const auto& possible_type = *possible_type_p;
                            if( const auto* te = ty_p->m_data.opt_Infer() )
                            {
                                context.possible_equate_type_unsize_to(te->index, possible_type);
                            }
                            else if( is_fallback )
                            {
                                DEBUG("Fallback equate " << possible_type);
                                context.equate_types(sp, *ty_p, possible_type);
                            }
                            else
                            {
                            }

                            //if( is_fallback )
                            //{
                            //    DEBUG("Possible equate " << possible_type);
                            //    context.equate_types( sp, *ty_p, possible_type );
                            //}
                        }
                    }

                    // Visit all inner bindings and disable coercion fallbacks on them.
                    MatchErgonomicsRevisit::disable_possibilities_on_bindings(sp, context, pattern);
                    return false;
                }
                const auto& ty = *ty_p;

                // Here we have a known type and binding mode for this pattern
                // - Time to handle this pattern then recurse into sub-patterns

                // Store the deref count in the pattern.
                pattern.m_implicit_deref_count = n_deref;
                // Determine the new binding mode from the borrow type
                switch(bt)
                {
                case ::HIR::BorrowType::Owned:
                    // No change
                    break;
                case ::HIR::BorrowType::Unique:
                    switch(binding_mode)
                    {
                    case ::HIR::PatternBinding::Type::Move:
                    case ::HIR::PatternBinding::Type::MutRef:
                        binding_mode = ::HIR::PatternBinding::Type::MutRef;
                        break;
                    case ::HIR::PatternBinding::Type::Ref:
                        // No change
                        break;
                    }
                    break;
                case ::HIR::BorrowType::Shared:
                    binding_mode = ::HIR::PatternBinding::Type::Ref;
                    break;
                }

                bool rv = false;
                TU_MATCH_HDR( (pattern.m_data), { )
                TU_ARM(pattern.m_data, Any, pe) {
                    // no-op
                    rv = true;
                    }
                TU_ARM(pattern.m_data, Value, pe) {
                    // no-op?
                    if( pe.val.is_String() || pe.val.is_ByteString() ) {
                        ASSERT_BUG(sp, pattern.m_implicit_deref_count >= 1, "");
                        pattern.m_implicit_deref_count -= 1;
                    }
                    rv = true;
                    }
                TU_ARM(pattern.m_data, Range, pe) {
                    // no-op?
                    rv = true;
                    }
                TU_ARM(pattern.m_data, Box, pe) {
                    // Box<T>
                    if( TU_TEST2(ty.m_data, Path, .path.m_data, Generic, .m_path == context.m_lang_Box) )
                    {
                        const auto& path = ty.m_data.as_Path().path.m_data.as_Generic();
                        const auto& inner = path.m_params.m_types.at(0);
                        rv = this->revisit_inner(context, *pe.sub, inner, binding_mode);
                    }
                    else
                    {
                        TODO(sp, "Match ergonomics - box pattern - Non Box<T> type: " << ty);
                        //auto inner = this->m_ivars.new_ivar_tr();
                        //this->handle_pattern_direct_inner(sp, *e.sub, inner);
                        //::HIR::GenericPath  path { m_lang_Box, ::HIR::PathParams(mv$(inner)) };
                        //this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypePathBinding(&m_crate.get_struct_by_path(sp, m_lang_Box))) );
                    }
                    }
                TU_ARM(pattern.m_data, Ref, pe) {
                    BUG(sp, "Match ergonomics - & pattern");
                    }
                TU_ARM(pattern.m_data, Tuple, e) {
                    if( !ty.m_data.is_Tuple() ) {
                        ERROR(sp, E0000, "Matching a non-tuple with a tuple pattern - " << ty);
                    }
                    const auto& te = ty.m_data.as_Tuple();
                    if( e.sub_patterns.size() != te.size() ) {
                        ERROR(sp, E0000, "Tuple pattern with an incorrect number of fields, expected " << e.sub_patterns.size() << "-tuple, got " << ty);
                    }

                    rv = true;
                    for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                        rv &= this->revisit_inner(context, e.sub_patterns[i], te[i], binding_mode);
                    }
                TU_ARM(pattern.m_data, SplitTuple, pe) {
                    if( !ty.m_data.is_Tuple() ) {
                        ERROR(sp, E0000, "Matching a non-tuple with a tuple pattern - " << ty);
                    }
                    const auto& te = ty.m_data.as_Tuple();
                    if( pe.leading.size() + pe.trailing.size() > te.size() ) {
                        ERROR(sp, E0000, "Split-tuple pattern with an incorrect number of fields, expected at most " << (pe.leading.size() + pe.trailing.size()) << "-tuple, got " << te.size());
                    }
                    pe.total_size = te.size();
                    rv = true;
                    for(size_t i = 0; i < pe.leading.size(); i++)
                        rv &= this->revisit_inner(context, pe.leading[i], te[i], binding_mode);
                    for(size_t i = 0; i < pe.trailing.size(); i++)
                        rv &= this->revisit_inner(context, pe.trailing[i], te[te.size() - pe.trailing.size() + i], binding_mode);
                    }
                TU_ARM(pattern.m_data, Slice, e) {
                    const ::HIR::TypeRef*   slice_inner;
                    if(const auto* te = ty.m_data.opt_Slice()) {
                        slice_inner = &*te->inner;
                    }
                    else if(const auto* te = ty.m_data.opt_Array() ) {
                        slice_inner = &*te->inner;
                    }
                    else {
                        ERROR(sp, E0000, "Matching a non-array/slice with a slice pattern - " << ty);
                    }
                    rv = true;
                    for(auto& sub : e.sub_patterns)
                        rv |= this->revisit_inner(context, sub, *slice_inner, binding_mode);
                    }
                TU_ARM(pattern.m_data, SplitSlice, pe) {
                    const ::HIR::TypeRef*   slice_inner;
                    if(const auto* te = ty.m_data.opt_Slice()) {
                        slice_inner = &*te->inner;
                    }
                    else if(const auto* te = ty.m_data.opt_Array() ) {
                        slice_inner = &*te->inner;
                    }
                    else {
                        ERROR(sp, E0000, "Matching a non-array/slice with a slice pattern - " << ty);
                    }
                    rv = true;
                    for(auto& sub : pe.leading)
                        rv |= this->revisit_inner(context, sub, *slice_inner, binding_mode);
                    // TODO: Extra bind
                    for(auto& sub : pe.trailing)
                        rv |= this->revisit_inner(context, sub, *slice_inner, binding_mode);
                    }
                TU_ARM(pattern.m_data, StructValue, e) {
                    context.add_ivars_params( e.path.m_params );
                    context.equate_types( sp, ty, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypePathBinding(e.binding)) );
                    rv = true;
                    }
                TU_ARM(pattern.m_data, StructTuple, e) {
                    context.add_ivars_params( e.path.m_params );
                    context.equate_types( sp, ty, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypePathBinding(e.binding)) );

                    assert(e.binding);
                    const auto& str = *e.binding;

                    // - assert check from earlier pass
                    ASSERT_BUG(sp, str.m_data.is_Tuple(), "Struct-tuple pattern on non-Tuple struct");
                    const auto& sd = str.m_data.as_Tuple();
                    const auto& params = e.path.m_params;
                    ::HIR::TypeRef  tmp;
                    auto maybe_monomorph = [&](const ::HIR::TypeRef& field_type)->const ::HIR::TypeRef& {
                        return (monomorphise_type_needed(field_type)
                                ? (tmp = context.m_resolve.expand_associated_types(sp, monomorphise_type(sp, str.m_params, params,  field_type)))
                                : field_type
                                );
                        };

                    rv = true;
                    for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                    {
                        /*const*/ auto& sub_pat = e.sub_patterns[i];
                        const auto& field_type = sd[i].ent;
                        const auto& var_ty = maybe_monomorph(field_type);
                        rv &= this->revisit_inner(context, sub_pat, var_ty, binding_mode);
                    }
                    }
                TU_ARM(pattern.m_data, Struct, e) {
                    context.add_ivars_params( e.path.m_params );
                    context.equate_types( sp, ty, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypePathBinding(e.binding)) );
                    assert(e.binding);
                    const auto& str = *e.binding;

                    //if( ! e.is_wildcard() )
                    if( e.sub_patterns.empty() )
                    {
                        // TODO: Check the field count?
                        rv = true;
                    }
                    else
                    {
                        // - assert check from earlier pass
                        ASSERT_BUG(sp, str.m_data.is_Named(), "Struct pattern on non-Named struct");
                        const auto& sd = str.m_data.as_Named();
                        const auto& params = e.path.m_params;

                        ::HIR::TypeRef  tmp;
                        auto maybe_monomorph = [&](const ::HIR::TypeRef& field_type)->const ::HIR::TypeRef& {
                            return (monomorphise_type_needed(field_type)
                                    ? (tmp = context.m_resolve.expand_associated_types(sp, monomorphise_type(sp, str.m_params, params,  field_type)))
                                    : field_type
                                    );
                            };

                        rv = true;
                        for( auto& field_pat : e.sub_patterns )
                        {
                            unsigned int f_idx = ::std::find_if( sd.begin(), sd.end(), [&](const auto& x){ return x.first == field_pat.first; } ) - sd.begin();
                            if( f_idx == sd.size() ) {
                                ERROR(sp, E0000, "Struct " << e.path << " doesn't have a field " << field_pat.first);
                            }
                            const ::HIR::TypeRef& field_type = maybe_monomorph(sd[f_idx].second.ent);
                            rv &= this->revisit_inner(context, field_pat.second, field_type, binding_mode);
                        }
                    }
                    }
                TU_ARM(pattern.m_data, EnumValue, e) {
                    context.add_ivars_params( e.path.m_params );
                    context.equate_types( sp, ty, ::HIR::TypeRef::new_path(get_parent_path(e.path), ::HIR::TypePathBinding(e.binding_ptr)) );
                    rv = true;
                    }
                TU_ARM(pattern.m_data, EnumTuple, e) {
                    context.add_ivars_params( e.path.m_params );
                    context.equate_types( sp, ty, ::HIR::TypeRef::new_path(get_parent_path(e.path), ::HIR::TypePathBinding(e.binding_ptr)) );
                    assert(e.binding_ptr);
                    const auto& enm = *e.binding_ptr;
                    const auto& str = *enm.m_data.as_Data()[e.binding_idx].type.m_data.as_Path().binding.as_Struct();
                    const auto& tup_var = str.m_data.as_Tuple();

                    const auto& params = e.path.m_params;

                    ::HIR::TypeRef  tmp;
                    auto maybe_monomorph = [&](const ::HIR::TypeRef& field_type)->const ::HIR::TypeRef& {
                        return (monomorphise_type_needed(field_type)
                                ? (tmp = context.m_resolve.expand_associated_types(sp, monomorphise_type(sp, str.m_params, params,  field_type)))
                                : field_type
                                );
                        };

                    if( e.sub_patterns.size() != tup_var.size() ) {
                        ERROR(sp, E0000, "Enum pattern with an incorrect number of fields - " << e.path << " - expected " << tup_var.size() << ", got " << e.sub_patterns.size() );
                    }

                    rv = true;  // &= below ensures that all must be complete to return complete
                    for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                    {
                        const auto& var_ty = maybe_monomorph(tup_var[i].ent);
                        rv &= this->revisit_inner(context, e.sub_patterns[i], var_ty, binding_mode);
                    }
                    }
                TU_ARM(pattern.m_data, EnumStruct, e) {
                    context.add_ivars_params( e.path.m_params );
                    context.equate_types( sp, ty, ::HIR::TypeRef::new_path(get_parent_path(e.path), ::HIR::TypePathBinding(e.binding_ptr)) );
                    assert(e.binding_ptr);

                    const auto& enm = *e.binding_ptr;
                    const auto& str = *enm.m_data.as_Data()[e.binding_idx].type.m_data.as_Path().binding.as_Struct();
                    const auto& tup_var = str.m_data.as_Named();
                    const auto& params = e.path.m_params;

                    ::HIR::TypeRef  tmp;
                    auto maybe_monomorph = [&](const ::HIR::TypeRef& field_type)->const ::HIR::TypeRef& {
                        return (monomorphise_type_needed(field_type)
                                ? (tmp = context.m_resolve.expand_associated_types(sp, monomorphise_type(sp, str.m_params, params,  field_type)))
                                : field_type
                                );
                        };

                    rv = true;  // &= below ensures that all must be complete to return complete
                    for( auto& field_pat : e.sub_patterns )
                    {
                        unsigned int f_idx = ::std::find_if( tup_var.begin(), tup_var.end(), [&](const auto& x){ return x.first == field_pat.first; } ) - tup_var.begin();
                        if( f_idx == tup_var.size() ) {
                            ERROR(sp, E0000, "Enum variant " << e.path << " doesn't have a field " << field_pat.first);
                        }
                        const ::HIR::TypeRef& field_type = maybe_monomorph(tup_var[f_idx].second.ent);
                        rv &= this->revisit_inner(context, field_pat.second, field_type, binding_mode);
                    }
                    }
                }
                return rv;
            }

            static void disable_possibilities_on_bindings(const Span& sp, Context& context, const ::HIR::Pattern& pat)
            {
                if( pat.m_binding.is_valid() ) {
                    const auto& pb = pat.m_binding;
                    //context.equate_types_from_shadow(sp, context.get_var(sp, pb.m_slot));
                    context.equate_types_shadow_strong(sp, context.get_var(sp, pb.m_slot));
                }
                TU_MATCHA( (pat.m_data), (e),
                (Any,
                    ),
                (Value,
                    ),
                (Range,
                    ),
                (Box,
                    disable_possibilities_on_bindings(sp, context, *e.sub);
                    ),
                (Ref,
                    disable_possibilities_on_bindings(sp, context, *e.sub);
                    ),
                (Tuple,
                    for(auto& subpat : e.sub_patterns)
                        disable_possibilities_on_bindings(sp, context, subpat);
                    ),
                (SplitTuple,
                    for(auto& subpat : e.leading) {
                        disable_possibilities_on_bindings(sp, context, subpat);
                    }
                    for(auto& subpat : e.trailing) {
                        disable_possibilities_on_bindings(sp, context, subpat);
                    }
                    ),
                (Slice,
                    for(auto& sub : e.sub_patterns)
                        disable_possibilities_on_bindings(sp, context, sub);
                    ),
                (SplitSlice,
                    for(auto& sub : e.leading)
                        disable_possibilities_on_bindings(sp, context, sub);
                    for(auto& sub : e.trailing)
                        disable_possibilities_on_bindings(sp, context, sub);
                    ),

                // - Enums/Structs
                (StructValue,
                    ),
                (StructTuple,
                    for(auto& subpat : e.sub_patterns)
                        disable_possibilities_on_bindings(sp, context, subpat);
                    ),
                (Struct,
                    for(auto& field_pat : e.sub_patterns)
                        disable_possibilities_on_bindings(sp, context, field_pat.second);
                    ),
                (EnumValue,
                    ),
                (EnumTuple,
                    for(auto& subpat : e.sub_patterns)
                        disable_possibilities_on_bindings(sp, context, subpat);
                    ),
                (EnumStruct,
                    for(auto& field_pat : e.sub_patterns)
                        disable_possibilities_on_bindings(sp, context, field_pat.second);
                    )
                )
            }
            static void create_bindings(const Span& sp, Context& context, ::HIR::Pattern& pat)
            {
                if( pat.m_binding.is_valid() ) {
                    const auto& pb = pat.m_binding;
                    context.add_var( sp, pb.m_slot, pb.m_name, context.m_ivars.new_ivar_tr() );
                    // TODO: Ensure that there's no more bindings below this?
                    // - I'll leave the option open, MIR generation should handle cases where there's multiple borrows
                    //   or moves.
                }
                TU_MATCH_HDRA( (pat.m_data), { )
                TU_ARMA(Any, e) {
                    }
                TU_ARMA(Value, e) {
                    }
                TU_ARMA(Range, e) {
                    }
                TU_ARMA(Box, e) {
                    create_bindings(sp, context, *e.sub);
                    }
                TU_ARMA(Ref, e) {
                    create_bindings(sp, context, *e.sub);
                    }
                TU_ARMA(Tuple, e) {
                    for(auto& subpat : e.sub_patterns)
                        create_bindings(sp, context, subpat);
                    }
                TU_ARMA(SplitTuple, e) {
                    for(auto& subpat : e.leading) {
                        create_bindings(sp, context, subpat);
                    }
                    for(auto& subpat : e.trailing) {
                        create_bindings(sp, context, subpat);
                    }
                    }
                TU_ARMA(Slice, e) {
                    for(auto& sub : e.sub_patterns)
                        create_bindings(sp, context, sub);
                    }
                TU_ARMA(SplitSlice, e) {
                    for(auto& sub : e.leading)
                        create_bindings(sp, context, sub);
                    if( e.extra_bind.is_valid() ) {
                        const auto& pb = e.extra_bind;
                        context.add_var( sp, pb.m_slot, pb.m_name, context.m_ivars.new_ivar_tr() );
                    }
                    for(auto& sub : e.trailing)
                        create_bindings(sp, context, sub);
                    }

                // - Enums/Structs
                TU_ARMA(StructValue, e) {
                    }
                TU_ARMA(StructTuple, e) {
                    for(auto& subpat : e.sub_patterns)
                        create_bindings(sp, context, subpat);
                    }
                TU_ARMA(Struct, e) {
                    for(auto& field_pat : e.sub_patterns)
                        create_bindings(sp, context, field_pat.second);
                    }
                TU_ARMA(EnumValue, e) {
                    }
                TU_ARMA(EnumTuple, e) {
                    for(auto& subpat : e.sub_patterns)
                        create_bindings(sp, context, subpat);
                    }
                TU_ARMA(EnumStruct, e) {
                    for(auto& field_pat : e.sub_patterns)
                        create_bindings(sp, context, field_pat.second);
                    }
                }
            }
        };
        // - Create variables, assigning new ivars for all of them.
        MatchErgonomicsRevisit::create_bindings(sp, *this, pat);
        // - Add a revisit for the outer pattern (saving the current target type as well as the pattern)
        DEBUG("Handle match ergonomics - " << pat << " with " << type);
        this->add_revisit_adv( box$(( MatchErgonomicsRevisit { sp, type.clone(), pat } )) );
        return ;
    }

    // ---
    this->handle_pattern_direct_inner(sp, pat, type);
}

void Context::handle_pattern_direct_inner(const Span& sp, ::HIR::Pattern& pat, const ::HIR::TypeRef& type)
{
    TRACE_FUNCTION_F("pat = " << pat << ", type = " << type);

    if( pat.m_binding.is_valid() ) {
        this->add_binding_inner(sp, pat.m_binding, type.clone());

        // TODO: Bindings aren't allowed within another binding
    }

    struct H {
        static void handle_value(Context& context, const Span& sp, const ::HIR::TypeRef& type, const ::HIR::Pattern::Value& val) {
            TU_MATCH(::HIR::Pattern::Value, (val), (v),
            (Integer,
                DEBUG("Integer " << ::HIR::TypeRef(v.type));
                // TODO: Apply an ivar bound? (Require that this ivar be an integer?)
                if( v.type != ::HIR::CoreType::Str ) {
                    context.equate_types(sp, type, ::HIR::TypeRef(v.type));
                }
                ),
            (Float,
                DEBUG("Float " << ::HIR::TypeRef(v.type));
                // TODO: Apply an ivar bound? (Require that this ivar be a float?)
                if( v.type != ::HIR::CoreType::Str ) {
                    context.equate_types(sp, type, ::HIR::TypeRef(v.type));
                }
                ),
            (String,
                context.equate_types(sp, type, ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef(::HIR::CoreType::Str) ));
                ),
            (ByteString,
                // NOTE: Matches both &[u8] and &[u8; N], so doesn't provide type information
                // TODO: Check the type.
                ),
            (Named,
                // TODO: Get type of the value and equate it
                )
            )
        }
    };

    //
    TU_MATCH_HDRA( (pat.m_data), {)
    TU_ARMA(Any, e) {
        // Just leave it, the pattern says nothing
        }
    TU_ARMA(Value, e) {
        H::handle_value(*this, sp, type, e.val);
        }
    TU_ARMA(Range, e) {
        H::handle_value(*this, sp, type, e.start);
        H::handle_value(*this, sp, type, e.end);
        }
    TU_ARMA(Box, e) {
        if( m_lang_Box == ::HIR::SimplePath() )
            ERROR(sp, E0000, "Use of `box` pattern without the `owned_box` lang item");
        const auto& ty = this->get_type(type);
        // Two options:
        // 1. Enforce that the current type must be "owned_box"
        // 2. Make a new ivar for the inner and emit an associated type bound on Deref

        // Taking option 1 for now
        if(const auto* te = ty.m_data.opt_Path())
        {
            if( TU_TEST1(te->path.m_data, Generic, .m_path == m_lang_Box) ) {
                // Box<T>
                const auto& inner = te->path.m_data.as_Generic().m_params.m_types.at(0);
                this->handle_pattern_direct_inner(sp, *e.sub, inner);
                break ;
            }
        }

        auto inner = this->m_ivars.new_ivar_tr();
        this->handle_pattern_direct_inner(sp, *e.sub, inner);
        ::HIR::GenericPath  path { m_lang_Box, ::HIR::PathParams(mv$(inner)) };
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypePathBinding(&m_crate.get_struct_by_path(sp, m_lang_Box))) );
        }
    TU_ARMA(Ref, e) {
        const auto& ty = this->get_type(type);
        if(const auto* te = ty.m_data.opt_Borrow())
        {
            if( te->type != e.type ) {
                ERROR(sp, E0000, "Pattern-type mismatch, &-ptr mutability mismatch");
            }
            this->handle_pattern_direct_inner(sp, *e.sub, *te->inner);
        }
        else {
            auto inner = this->m_ivars.new_ivar_tr();
            this->handle_pattern_direct_inner(sp, *e.sub, inner);
            this->equate_types(sp, type, ::HIR::TypeRef::new_borrow( e.type, mv$(inner) ));
        }
        }
    TU_ARMA(Tuple, e) {
        const auto& ty = this->get_type(type);
        if(const auto* tep = ty.m_data.opt_Tuple())
        {
            const auto& te = *tep;
            if( e.sub_patterns.size() != te.size() ) {
                ERROR(sp, E0000, "Tuple pattern with an incorrect number of fields, expected " << e.sub_patterns.size() << "-tuple, got " << ty);
            }

            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                this->handle_pattern_direct_inner(sp, e.sub_patterns[i], te[i] );
        }
        else {

            ::std::vector< ::HIR::TypeRef>  sub_types;
            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ ) {
                sub_types.push_back( this->m_ivars.new_ivar_tr() );
                this->handle_pattern_direct_inner(sp, e.sub_patterns[i], sub_types[i] );
            }
            this->equate_types(sp, ty, ::HIR::TypeRef( mv$(sub_types) ));
        }
        }
    TU_ARMA(SplitTuple, e) {
        const auto& ty = this->get_type(type);
        if(const auto* tep = ty.m_data.opt_Tuple())
        {
            const auto& te = *tep;
            // - Should have been checked in AST resolve
            ASSERT_BUG(sp, e.leading.size() + e.trailing.size() <= te.size(), "Invalid field count for split tuple pattern");

            unsigned int tup_idx = 0;
            for(auto& subpat : e.leading) {
                this->handle_pattern_direct_inner(sp, subpat, te[tup_idx++]);
            }
            tup_idx = te.size() - e.trailing.size();
            for(auto& subpat : e.trailing) {
                this->handle_pattern_direct_inner(sp, subpat, te[tup_idx++]);
            }

            // TODO: Should this replace the pattern with a non-split?
            // - Changing the address of the pattern means that the below revisit could fail.
            e.total_size = te.size();
        }
        else {
            if( !ty.m_data.is_Infer() ) {
                ERROR(sp, E0000, "Tuple pattern on non-tuple");
            }

            ::std::vector<::HIR::TypeRef>   leading_tys;
            leading_tys.reserve(e.leading.size());
            for(auto& subpat : e.leading) {
                leading_tys.push_back( this->m_ivars.new_ivar_tr() );
                this->handle_pattern_direct_inner(sp, subpat, leading_tys.back());
            }
            ::std::vector<::HIR::TypeRef>   trailing_tys;
            for(auto& subpat : e.trailing) {
                trailing_tys.push_back( this->m_ivars.new_ivar_tr() );
                this->handle_pattern_direct_inner(sp, subpat, trailing_tys.back());
            }

            struct SplitTuplePatRevisit:
                public Revisitor
            {
                Span    sp;
                ::HIR::TypeRef  m_outer_ty;
                ::std::vector<::HIR::TypeRef>   m_leading_tys;
                ::std::vector<::HIR::TypeRef>   m_trailing_tys;
                unsigned int& m_pat_total_size;

                SplitTuplePatRevisit(Span sp, ::HIR::TypeRef outer, ::std::vector<::HIR::TypeRef> leading, ::std::vector<::HIR::TypeRef> trailing, unsigned int& pat_total_size):
                    sp(mv$(sp)), m_outer_ty(mv$(outer)),
                    m_leading_tys( mv$(leading) ), m_trailing_tys( mv$(trailing) ),
                    m_pat_total_size(pat_total_size)
                {}

                const Span& span() const override {
                    return sp;
                }
                void fmt(::std::ostream& os) const override {
                    os << "SplitTuplePatRevisit { " << m_outer_ty << " = (" << m_leading_tys << ", ..., " << m_trailing_tys << ") }";
                }
                bool revisit(Context& context, bool is_fallback) override {
                    const auto& ty = context.get_type(m_outer_ty);
                    if(ty.m_data.is_Infer()) {
                        return false;
                    }
                    else if(const auto* tep = ty.m_data.opt_Tuple()) {
                        const auto& te = *tep;
                        if( te.size() < m_leading_tys.size() + m_trailing_tys.size() )
                            ERROR(sp, E0000, "Tuple pattern too large for tuple");
                        for(unsigned int i = 0; i < m_leading_tys.size(); i ++)
                            context.equate_types(sp, te[i], m_leading_tys[i]);
                        unsigned int ofs = te.size() - m_trailing_tys.size();
                        for(unsigned int i = 0; i < m_trailing_tys.size(); i ++)
                            context.equate_types(sp, te[ofs+i], m_trailing_tys[i]);
                        m_pat_total_size = te.size();
                        return true;
                    }
                    else {
                        ERROR(sp, E0000, "Tuple pattern on non-tuple - " << ty);
                    }
                }
            };

            // Register a revisit and wait until the tuple is known - then bind through.
            this->add_revisit_adv( box$(( SplitTuplePatRevisit { sp, ty.clone(), mv$(leading_tys), mv$(trailing_tys), e.total_size } )) );
        }
        }
    TU_ARMA(Slice, e) {
        const auto& ty = this->get_type(type);
        TU_MATCH_HDRA( (ty.m_data), {)
        default:
            ERROR(sp, E0000, "Slice pattern on non-array/-slice - " << ty);
        TU_ARMA(Slice, te) {
            for(auto& sub : e.sub_patterns)
                this->handle_pattern_direct_inner(sp, sub, *te.inner );
            }
        TU_ARMA(Array, te) {
            for(auto& sub : e.sub_patterns)
                this->handle_pattern_direct_inner(sp, sub, *te.inner );
            }
        TU_ARMA(Infer, te) {
            auto inner = this->m_ivars.new_ivar_tr();
            for(auto& sub : e.sub_patterns)
                this->handle_pattern_direct_inner(sp, sub, inner);

            struct SlicePatRevisit:
                public Revisitor
            {
                Span    sp;
                ::HIR::TypeRef  inner;
                ::HIR::TypeRef  type;
                unsigned int size;

                SlicePatRevisit(Span sp, ::HIR::TypeRef inner, ::HIR::TypeRef type, unsigned int size):
                    sp(mv$(sp)), inner(mv$(inner)), type(mv$(type)), size(size)
                {}

                const Span& span() const override {
                    return sp;
                }
                void fmt(::std::ostream& os) const override { os << "SlicePatRevisit { " << inner << ", " << type << ", " << size; }
                bool revisit(Context& context, bool is_fallback) override {
                    const auto& ty = context.get_type(type);
                    TU_MATCH_HDRA( (ty.m_data), {)
                    default:
                        ERROR(sp, E0000, "Slice pattern on non-array/-slice - " << ty);
                    TU_ARMA(Infer, te) {
                        return false;
                        }
                    TU_ARMA(Slice, te) {
                        context.equate_types(sp, *te.inner, inner);
                        return true;
                        }
                    TU_ARMA(Array, te) {
                        if( te.size.as_Known() != size ) {
                            ERROR(sp, E0000, "Slice pattern on an array if differing size");
                        }
                        context.equate_types(sp, *te.inner, inner);
                        return true;
                        }
                    }
                }
            };
            this->add_revisit_adv( box$(( SlicePatRevisit { sp, mv$(inner), ty.clone(), static_cast<unsigned int>(e.sub_patterns.size()) } )) );
            }
        }
        }
    TU_ARMA(SplitSlice, e) {
        ::HIR::TypeRef  inner;
        unsigned int min_len = e.leading.size() + e.trailing.size();
        const auto& ty = this->get_type(type);
        TU_MATCH_HDRA( (ty.m_data), {)
        default:
            ERROR(sp, E0000, "SplitSlice pattern on non-array/-slice - " << ty);
        TU_ARMA(Slice, te) {
            // Slice - Fetch inner and set new variable also be a slice
            // - TODO: Better new variable handling.
            inner = te.inner->clone();
            if( e.extra_bind.is_valid() ) {
                this->add_binding_inner( sp, e.extra_bind, ty.clone() );
            }
            }
        TU_ARMA(Array, te) {
            inner = te.inner->clone();
            if( te.size.as_Known() < min_len ) {
                ERROR(sp, E0000, "Slice pattern on an array smaller than the pattern");
            }
            unsigned extra_len = te.size.as_Known() - min_len;

            if( e.extra_bind.is_valid() ) {
                this->add_binding_inner( sp, e.extra_bind, ::HIR::TypeRef::new_array(inner.clone(), extra_len) );
            }
            }
        TU_ARMA(Infer, te) {
            inner = this->m_ivars.new_ivar_tr();
            ::HIR::TypeRef  var_ty;
            if( e.extra_bind.is_valid() ) {
                var_ty = this->m_ivars.new_ivar_tr();
                this->add_binding_inner( sp, e.extra_bind, var_ty.clone() );
            }

            struct SplitSlicePatRevisit:
                public Revisitor
            {
                Span    sp;
                // Inner type
                ::HIR::TypeRef  inner;
                // Outer ivar (should be either Slice or Array)
                ::HIR::TypeRef  type;
                // Binding type (if not default value)
                ::HIR::TypeRef  var_ty;
                unsigned int min_size;

                SplitSlicePatRevisit(Span sp, ::HIR::TypeRef inner, ::HIR::TypeRef type, ::HIR::TypeRef var_ty, unsigned int size):
                    sp(mv$(sp)), inner(mv$(inner)), type(mv$(type)), var_ty(mv$(var_ty)), min_size(size)
                {}

                const Span& span() const override {
                    return sp;
                }
                void fmt(::std::ostream& os) const override { os << "SplitSlice inner=" << inner << ", outer=" << type << ", binding="<<var_ty<<", " << min_size; }
                bool revisit(Context& context, bool is_fallback) override {
                    const auto& ty = context.get_type(this->type);
                    TU_MATCH_HDRA( (ty.m_data), {)
                    default:
                        ERROR(sp, E0000, "Slice pattern on non-array/-slice - " << ty);
                    TU_ARMA(Infer, te) {
                        return false;
                        }
                    TU_ARMA(Slice, te) {
                        // Slice - Equate inners
                        context.equate_types(this->sp, this->inner, *te.inner);
                        if( this->var_ty != ::HIR::TypeRef() ) {
                            context.equate_types(this->sp, this->var_ty, ty);
                        }
                        }
                    TU_ARMA(Array, te) {
                        // Array - Equate inners and check size
                        context.equate_types(this->sp, this->inner, *te.inner);
                        if( te.size.as_Known() < this->min_size ) {
                            ERROR(sp, E0000, "Slice pattern on an array smaller than the pattern");
                        }
                        unsigned extra_len = te.size.as_Known() - this->min_size;

                        if( this->var_ty != ::HIR::TypeRef() ) {
                            context.equate_types(this->sp, this->var_ty, ::HIR::TypeRef::new_array(this->inner.clone(), extra_len) );
                        }
                        }
                    }
                    return true;
                }
            };
            // Callback
            this->add_revisit_adv( box$(( SplitSlicePatRevisit { sp, inner.clone(), ty.clone(), mv$(var_ty), min_len } )) );
            }
        }

        for(auto& sub : e.leading)
            this->handle_pattern_direct_inner( sp, sub, inner );
        for(auto& sub : e.trailing)
            this->handle_pattern_direct_inner( sp, sub, inner );
        }

    // - Enums/Structs
    TU_ARMA(StructValue, e) {
        this->add_ivars_params( e.path.m_params );
        const auto& str = *e.binding;
        assert( str.m_data.is_Unit() );
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypePathBinding(e.binding)) );
        }
    TU_ARMA(StructTuple, e) {
        this->add_ivars_params( e.path.m_params );
        const auto& str = *e.binding;
        // - assert check from earlier pass
        assert( str.m_data.is_Tuple() );
        const auto& sd = str.m_data.as_Tuple();

        const auto& params = e.path.m_params;
        assert(e.binding);
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypePathBinding(e.binding)) );

        for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
        {
            /*const*/ auto& sub_pat = e.sub_patterns[i];
            const auto& field_type = sd[i].ent;
            if( monomorphise_type_needed(field_type) ) {
                auto var_ty = monomorphise_type(sp, str.m_params, params,  field_type);
                this->handle_pattern_direct_inner(sp, sub_pat, var_ty);
            }
            else {
                this->handle_pattern_direct_inner(sp, sub_pat, field_type);
            }
        }
        }
    TU_ARMA(Struct, e) {
        this->add_ivars_params( e.path.m_params );
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypePathBinding(e.binding)) );

        if( e.is_wildcard() )
            return ;

        assert(e.binding);
        const auto& str = *e.binding;

        // - assert check from earlier pass
        ASSERT_BUG(sp, str.m_data.is_Named(), "Struct pattern on non-Named struct");
        const auto& sd = str.m_data.as_Named();
        const auto& params = e.path.m_params;

        for( auto& field_pat : e.sub_patterns )
        {
            unsigned int f_idx = ::std::find_if( sd.begin(), sd.end(), [&](const auto& x){ return x.first == field_pat.first; } ) - sd.begin();
            if( f_idx == sd.size() ) {
                ERROR(sp, E0000, "Struct " << e.path << " doesn't have a field " << field_pat.first);
            }
            const ::HIR::TypeRef& field_type = sd[f_idx].second.ent;
            if( monomorphise_type_needed(field_type) ) {
                auto field_type_mono = monomorphise_type(sp, str.m_params, params,  field_type);
                this->handle_pattern_direct_inner(sp, field_pat.second, field_type_mono);
            }
            else {
                this->handle_pattern_direct_inner(sp, field_pat.second, field_type);
            }
        }
        }
    TU_ARMA(EnumValue, e) {
        this->add_ivars_params( e.path.m_params );
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(get_parent_path(e.path), ::HIR::TypePathBinding(e.binding_ptr)) );

        assert(e.binding_ptr);
        const auto& enm = *e.binding_ptr;
        if( enm.m_data.is_Data() )
        {
            //const auto& var = enm.m_data.as_Data()[e.binding_idx];
            //assert(var.is_Value() || var.is_Unit());
        }
        }
    TU_ARMA(EnumTuple, e) {
        this->add_ivars_params( e.path.m_params );
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(get_parent_path(e.path), ::HIR::TypePathBinding(e.binding_ptr)) );
        assert(e.binding_ptr);
        const auto& enm = *e.binding_ptr;
        const auto& str = *enm.m_data.as_Data()[e.binding_idx].type.m_data.as_Path().binding.as_Struct();
        const auto& tup_var = str.m_data.as_Tuple();

        const auto& params = e.path.m_params;

        ASSERT_BUG(sp, e.sub_patterns.size() == tup_var.size(),
            "Enum pattern with an incorrect number of fields - " << e.path << " - expected " << tup_var.size() << ", got " << e.sub_patterns.size()
            );

        for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
        {
            if( monomorphise_type_needed(tup_var[i].ent) ) {
                auto var_ty = monomorphise_type(sp, enm.m_params, params,  tup_var[i].ent);
                this->handle_pattern_direct_inner(sp, e.sub_patterns[i], var_ty);
            }
            else {
                this->handle_pattern_direct_inner(sp, e.sub_patterns[i], tup_var[i].ent);
            }
        }
        }
    TU_ARMA(EnumStruct, e) {
        this->add_ivars_params( e.path.m_params );
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(get_parent_path(e.path), ::HIR::TypePathBinding(e.binding_ptr)) );

        if( e.sub_patterns.empty() )
            return ;

        assert(e.binding_ptr);
        const auto& enm = *e.binding_ptr;
        const auto& str = *enm.m_data.as_Data()[e.binding_idx].type.m_data.as_Path().binding.as_Struct();
        const auto& tup_var = str.m_data.as_Named();
        const auto& params = e.path.m_params;

        for( auto& field_pat : e.sub_patterns )
        {
            unsigned int f_idx = ::std::find_if( tup_var.begin(), tup_var.end(), [&](const auto& x){ return x.first == field_pat.first; } ) - tup_var.begin();
            if( f_idx == tup_var.size() ) {
                ERROR(sp, E0000, "Enum variant " << e.path << " doesn't have a field " << field_pat.first);
            }
            const ::HIR::TypeRef& field_type = tup_var[f_idx].second.ent;
            if( monomorphise_type_needed(field_type) ) {
                auto field_type_mono = monomorphise_type(sp, enm.m_params, params,  field_type);
                this->handle_pattern_direct_inner(sp, field_pat.second, field_type_mono);
            }
            else {
                this->handle_pattern_direct_inner(sp, field_pat.second, field_type);
            }
        }
        }
    }
}
void Context::equate_types_coerce(const Span& sp, const ::HIR::TypeRef& l, ::HIR::ExprNodeP& node_ptr)
{
    this->m_ivars.get_type(l);
    // - Just record the equality
    this->link_coerce.push_back(std::make_unique<Coercion>(Coercion {
        this->next_rule_idx ++,
        l.clone(), &node_ptr
        }));
    DEBUG("++ " << *this->link_coerce.back());
    this->m_ivars.mark_change();
}
void Context::equate_types_shadow(const Span& sp, const ::HIR::TypeRef& l, bool is_to)
{
    TU_MATCH_HDRA( (this->get_type(l).m_data), {)
    default:
        // TODO: Shadow sub-types too
    TU_ARMA(Path, e) {
        TU_MATCH_DEF( ::HIR::Path::Data, (e.path.m_data), (pe),
        (
            ),
        (Generic,
            for(const auto& sty : pe.m_params.m_types)
                this->equate_types_shadow(sp, sty, is_to);
            )
        )
        }
    TU_ARMA(Tuple, e) {
        for(const auto& sty : e)
            this->equate_types_shadow(sp, sty, is_to);
        }
    TU_ARMA(Borrow, e) {
        this->equate_types_shadow(sp, *e.inner, is_to);
        }
    TU_ARMA(Array, e) {
        this->equate_types_shadow(sp, *e.inner, is_to);
        }
    TU_ARMA(Slice, e) {
        this->equate_types_shadow(sp, *e.inner, is_to);
        }
    TU_ARMA(Closure, e) {
        for(const auto& aty : e.m_arg_types)
            this->equate_types_shadow(sp, aty, is_to);
        this->equate_types_shadow(sp, *e.m_rettype, is_to);
        }
    TU_ARMA(Infer, e) {
        this->possible_equate_type_disable(e.index, is_to);
        }
    }
}
void Context::equate_types_shadow_strong(const Span& sp, const ::HIR::TypeRef& ty)
{
    TU_MATCH_HDRA( (this->get_type(ty).m_data), {)
    default:
        // TODO: Shadow sub-types too
    TU_ARMA(Path, e) {
        TU_MATCH_DEF( ::HIR::Path::Data, (e.path.m_data), (pe),
        (
            ),
        (Generic,
            for(const auto& sty : pe.m_params.m_types)
                this->equate_types_shadow_strong(sp, sty);
            )
        )
        }
    TU_ARMA(Tuple, e) {
        for(const auto& sty : e)
            this->equate_types_shadow_strong(sp, sty);
        }
    TU_ARMA(Borrow, e) {
        this->equate_types_shadow_strong(sp, *e.inner);
        }
    TU_ARMA(Array, e) {
        this->equate_types_shadow_strong(sp, *e.inner);
        }
    TU_ARMA(Slice, e) {
        this->equate_types_shadow_strong(sp, *e.inner);
        }
    TU_ARMA(Closure, e) {
        for(const auto& aty : e.m_arg_types)
            this->equate_types_shadow_strong(sp, aty);
        this->equate_types_shadow_strong(sp, *e.m_rettype);
        }
    TU_ARMA(Infer, e) {
        this->possible_equate_type_disable_strong(sp, e.index);
        }
    }
}
void Context::equate_types_assoc(const Span& sp, const ::HIR::TypeRef& l,  const ::HIR::SimplePath& trait, ::HIR::PathParams pp, const ::HIR::TypeRef& impl_ty, const char *name, bool is_op)
{
    for(const auto& a : this->link_assoc)
    {
        if( a.left_ty != l )
            continue ;
        if( a.trait != trait )
            continue ;
        if( a.params != pp )
            continue ;
        if( a.impl_ty != impl_ty )
            continue ;
        if( a.name != name )
            continue ;
        if( a.is_operator != is_op )
            continue ;

        DEBUG("(DUPLICATE " << a << ")");
        return ;
    }
    this->link_assoc.push_back(Associated {
        this->next_rule_idx ++,
        sp,
        l.clone(),

        trait.clone(),
        mv$(pp),
        impl_ty.clone(),
        name,
        is_op
        });
    DEBUG("++ " << this->link_assoc.back());
    this->m_ivars.mark_change();
}
void Context::add_revisit(::HIR::ExprNode& node) {
    this->to_visit.push_back( &node );
}
void Context::add_revisit_adv(::std::unique_ptr<Revisitor> ent_ptr) {
    this->adv_revisits.push_back( mv$(ent_ptr) );
}
void Context::require_sized(const Span& sp, const ::HIR::TypeRef& ty_)
{
    const auto& ty = m_ivars.get_type(ty_);
    TRACE_FUNCTION_F(ty_ << " -> " << ty);
    if( m_resolve.type_is_sized(sp, ty) == ::HIR::Compare::Unequal )
    {
        ERROR(sp, E0000, "Unsized type not valid here - " << ty);
    }
    if( const auto* e = ty.m_data.opt_Infer() )
    {
        switch(e->ty_class)
        {
        case ::HIR::InferClass::Integer:
        case ::HIR::InferClass::Float:
            // Has to be.
            break;
        default:
            // TODO: Flag for future checking
            ASSERT_BUG(sp, e->index != ~0u, "Unbound ivar " << ty);
            if(e->index >= m_ivars_sized.size())
                m_ivars_sized.resize(e->index+1);
            m_ivars_sized.at(e->index) = true;
            break;
        }
    }
    else if( const auto* e = ty.m_data.opt_Path() )
    {
        const ::HIR::GenericParams* params_def = nullptr;
        TU_MATCHA( (e->binding), (pb),
        (Unbound,
            // TODO: Add a trait check rule
            params_def = nullptr;
            ),
        (Opaque,
            // Already checked by type_is_sized
            params_def = nullptr;
            ),
        (ExternType,
            static ::HIR::GenericParams empty_params;
            params_def = &empty_params;
            ),
        (Enum,
            params_def = &pb->m_params;
            ),
        (Union,
            params_def = &pb->m_params;
            ),
        (Struct,
            params_def = &pb->m_params;

            if( pb->m_struct_markings.dst_type == ::HIR::StructMarkings::DstType::Possible )
            {
                // Check sized-ness of the unsized param
                this->require_sized( sp, e->path.m_data.as_Generic().m_params.m_types.at(pb->m_struct_markings.unsized_param) );
            }
            )
        )

        if( params_def )
        {
            const auto& gp_tys = e->path.m_data.as_Generic().m_params.m_types;
            for(size_t i = 0; i < gp_tys.size(); i ++)
            {
                if(params_def->m_types.at(i).m_is_sized)
                {
                    this->require_sized( sp, gp_tys[i] );
                }
            }
        }
    }
    else if( const auto* e = ty.m_data.opt_Tuple() )
    {
        // All entries in a tuple must be Sized
        for(const auto& ity : *e)
        {
            this->require_sized( sp, ity );
        }
    }
    else if( const auto* e = ty.m_data.opt_Array() )
    {
        // Inner type of an array must be sized
        this->require_sized(sp, *e->inner);
    }
}

void Context::possible_equate_type(unsigned int ivar_index, const ::HIR::TypeRef& t, bool is_to, bool is_borrow) {
    DEBUG(ivar_index << " " << (is_borrow ? "unsize":"coerce") << " " << (is_to?"to":"from") << " " << t << " " << this->m_ivars.get_type(t));
    {
        ::HIR::TypeRef  ty_l;
        ty_l.m_data.as_Infer().index = ivar_index;
        const auto& real_ty = m_ivars.get_type(ty_l);
        if( real_ty != ty_l )
        {
            DEBUG("IVar " << ivar_index << " is actually " << real_ty);
            return ;
        }
    }

    if( ivar_index >= possible_ivar_vals.size() ) {
        possible_ivar_vals.resize( ivar_index + 1 );
    }
    auto& ent = possible_ivar_vals[ivar_index];
    auto& list = (is_borrow
        ? (is_to ? ent.types_unsize_to : ent.types_unsize_from)
        : (is_to ? ent.types_coerce_to : ent.types_coerce_from)
        );
    list.push_back( t.clone() );
}
void Context::possible_equate_type_bound(const Span& sp, unsigned int ivar_index, const ::HIR::TypeRef& t) {
    {
        ::HIR::TypeRef  ty_l;
        ty_l.m_data.as_Infer().index = ivar_index;
        const auto& real_ty = m_ivars.get_type(ty_l);
        if( real_ty != ty_l )
        {
            DEBUG("IVar " << ivar_index << " is actually " << real_ty);
            return ;
        }

        ASSERT_BUG(sp, !type_contains_impl_placeholder(t), "Type contained an impl placeholder parameter - " << t);
    }

    if( ivar_index >= possible_ivar_vals.size() ) {
        possible_ivar_vals.resize( ivar_index + 1 );
    }
    auto& ent = possible_ivar_vals[ivar_index];
    for(const auto& e : ent.bounded)
    {
        if( e == t )
        {
            if( t.m_data.is_Infer() )
                DEBUG(ivar_index << " duplicate bounded " << t << " " << this->m_ivars.get_type(t));
            else
                DEBUG(ivar_index << " duplicate bounded " << t);
            return ;
        }
    }
    ent.bounded.push_back( t.clone() );
    if( t.m_data.is_Infer() )
        DEBUG(ivar_index << " bounded as " << t << " " << this->m_ivars.get_type(t));
    else
        DEBUG(ivar_index << " bounded as " << t);
}
void Context::possible_equate_type_disable(unsigned int ivar_index, bool is_to) {
    DEBUG(ivar_index << " ?= ?? (" << (is_to ? "to" : "from") << ")");
    {
        ::HIR::TypeRef  ty_l;
        ty_l.m_data.as_Infer().index = ivar_index;
        assert( m_ivars.get_type(ty_l).m_data.is_Infer() );
    }

    if( ivar_index >= possible_ivar_vals.size() ) {
        possible_ivar_vals.resize( ivar_index + 1 );
    }
    auto& ent = possible_ivar_vals[ivar_index];
    if( is_to ) {
        ent.force_no_to = true;
    }
    else {
        ent.force_no_from = true;
    }
}
void Context::possible_equate_type_disable_strong(const Span& sp, unsigned int ivar_index)
{
    DEBUG(ivar_index << " = ??");
    {
        ::HIR::TypeRef  ty_l;
        ty_l.m_data.as_Infer().index = ivar_index;
        ASSERT_BUG(sp, m_ivars.get_type(ty_l).m_data.is_Infer(), "possible_equate_type_disable_strong on known ivar");
    }

    if( ivar_index >= possible_ivar_vals.size() ) {
        possible_ivar_vals.resize( ivar_index + 1 );
    }
    auto& ent = possible_ivar_vals[ivar_index];
    ent.force_disable = true;
}

void Context::add_var(const Span& sp, unsigned int index, const RcString& name, ::HIR::TypeRef type) {
    DEBUG("(" << index << " " << name << " : " << type << ")");
    assert(index != ~0u);
    if( m_bindings.size() <= index )
        m_bindings.resize(index+1);
    if( m_bindings[index].name == "" ) {
        m_bindings[index] = Binding { name, mv$(type) };
        this->require_sized(sp, m_bindings[index].ty);
    }
    else {
        ASSERT_BUG(sp, m_bindings[index].name == name, "");
        this->equate_types(sp, m_bindings[index].ty, type);
    }
}

const ::HIR::TypeRef& Context::get_var(const Span& sp, unsigned int idx) const {
    if( idx < this->m_bindings.size() ) {
        return this->m_bindings[idx].ty;
    }
    else {
        BUG(sp, "get_var - Binding index out of range - " << idx << " >=" << this->m_bindings.size());
    }
}

::HIR::ExprNodeP Context::create_autoderef(::HIR::ExprNodeP val_node, ::HIR::TypeRef ty_dst) const
{
    const auto& span = val_node->span();
    const auto& ty_src = val_node->m_res_type;
    // Special case for going Array->Slice, insert _Unsize instead of _Deref
    if( get_type(ty_src).m_data.is_Array() )
    {
        ASSERT_BUG(span, ty_dst.m_data.is_Slice(), "Array should only ever autoderef to Slice");

        // HACK: Emit an invalid _Unsize op that is fixed once usage type is known.
        auto ty_dst_c = ty_dst.clone();
        val_node = NEWNODE( mv$(ty_dst), span, _Unsize,  mv$(val_node), mv$(ty_dst_c) );
        DEBUG("- Unsize " << &*val_node << " -> " << val_node->m_res_type);
    }
    else {
        val_node = NEWNODE( mv$(ty_dst), span, _Deref,  mv$(val_node) );
        DEBUG("- Deref " << &*val_node << " -> " << val_node->m_res_type);
    }

    return val_node;
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
                if( typ.m_default.m_data.is_Infer() ) {
                    ERROR(sp, E0000, "Omitted type parameter with no default in " << path);
                }
                else if( monomorphise_type_needed(typ.m_default) ) {
                    auto cb = [&](const auto& ty)->const ::HIR::TypeRef& {
                        const auto& ge = ty.m_data.as_Generic();
                        if( ge.binding == 0xFFFF ) {
                            ASSERT_BUG(sp, self_ty != ::HIR::TypeRef(), "Self not allowed in this context");
                            return self_ty;
                        }
                        else {
                            TODO(sp, "Monomorphise default param - " << typ.m_default << " - " << ty);
                        }
                        };
                    auto ty = monomorphise_type_with(sp, typ.m_default, cb);
                    params.m_types.push_back( mv$(ty) );
                }
                else {
                    params.m_types.push_back( typ.m_default.clone() );
                }
            }
            else
            {
                params.m_types.push_back( context.m_ivars.new_ivar_tr() );
                // TODO: It's possible that the default could be added using `context.possible_equate_type_def` to give inferrence a fallback
            }
        }
    }
}
void fix_param_count(const Span& sp, Context& context, const ::HIR::TypeRef& self_ty, bool use_defaults, const ::HIR::Path& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params) {
    fix_param_count_(sp, context, self_ty, use_defaults, path, param_defs, params);
}
void fix_param_count(const Span& sp, Context& context, const ::HIR::TypeRef& self_ty, bool use_defaults, const ::HIR::GenericPath& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params) {
    fix_param_count_(sp, context, self_ty, use_defaults, path, param_defs, params);
}

namespace {
    void add_coerce_borrow(Context& context, ::HIR::ExprNodeP& orig_node_ptr, const ::HIR::TypeRef& des_borrow_inner, ::std::function<void(::HIR::ExprNodeP& n)> cb)
    {
        const auto& src_type = context.m_ivars.get_type(orig_node_ptr->m_res_type);
        auto borrow_type = src_type.m_data.as_Borrow().type;

        // Since this function operates on destructured &-ptrs, the dereferences have to be added behind a borrow
        ::HIR::ExprNodeP*   node_ptr_ptr = &orig_node_ptr;

        #if 1
        // If the coercion is of a block, apply the mutation to the inner node
        while( auto* p = dynamic_cast< ::HIR::ExprNode_Block*>(&**node_ptr_ptr) )
        {
            DEBUG("- Moving into block");
            assert( p->m_value_node );
            // Block result and the inner node's result must be the same type
            ASSERT_BUG( p->span(), context.m_ivars.types_equal(p->m_res_type, p->m_value_node->m_res_type),
                "Block and result mismatch - " << context.m_ivars.fmt_type(p->m_res_type) << " != " << context.m_ivars.fmt_type(p->m_value_node->m_res_type));
            // - Override the the result type to the desired result
            p->m_res_type = ::HIR::TypeRef::new_borrow(borrow_type, des_borrow_inner.clone());
            node_ptr_ptr = &p->m_value_node;
        }
        #endif
        auto& node_ptr = *node_ptr_ptr;

        // - If the pointed node is a borrow operation, add the dereferences within its value
        if( auto* p = dynamic_cast< ::HIR::ExprNode_Borrow*>(&*node_ptr) )
        {
            // Set the result of the borrow operation to the output type
            node_ptr->m_res_type = ::HIR::TypeRef::new_borrow(borrow_type, des_borrow_inner.clone());

            node_ptr_ptr = &p->m_value;
        }
        // - Otherwise, create a new borrow operation behind which the dereferences happen
        else
        {
            DEBUG("- Coercion node isn't a borrow, adding one");
            auto span = node_ptr->span();
            const auto& src_inner_ty = *src_type.m_data.as_Borrow().inner;

            auto inner_ty_ref = ::HIR::TypeRef::new_borrow(borrow_type, des_borrow_inner.clone());

            // 1. Dereference (resulting in the dereferenced input type)
            node_ptr = NEWNODE(src_inner_ty.clone(), span, _Deref,  mv$(node_ptr));
            // 2. Borrow (resulting in the referenced output type)
            node_ptr = NEWNODE(mv$(inner_ty_ref), span, _Borrow,  borrow_type, mv$(node_ptr));

            // - Set node pointer reference to point into the new borrow op
            node_ptr_ptr = &dynamic_cast< ::HIR::ExprNode_Borrow&>(*node_ptr).m_value;
        }

        cb(*node_ptr_ptr);

        context.m_ivars.mark_change();
    }

    enum CoerceResult {
        Unknown,    // Coercion still unknown.
        Equality,   // Types should be equated
        Custom, // An op was emitted, and rule is complete
        Unsize, // Emits an _Unsize op
    };

    // TODO: Add a (two?) callback(s) that handle type equalities (and possible equalities) so this function doesn't have to mutate the context
    CoerceResult check_unsize_tys(Context& context_mut_r, const Span& sp, const ::HIR::TypeRef& dst_raw, const ::HIR::TypeRef& src_raw, ::HIR::ExprNodeP* node_ptr_ptr=nullptr, bool allow_mutate=true)
    {
        Context* context_mut = (allow_mutate ? &context_mut_r : nullptr);
        const Context& context = context_mut_r;

        const auto& dst = context.m_ivars.get_type(dst_raw);
        const auto& src = context.m_ivars.get_type(src_raw);
        TRACE_FUNCTION_F("dst=" << dst << ", src=" << src);

        // If the types are already equal, no operation is required
        if( context.m_ivars.types_equal(dst, src) ) {
            DEBUG("Equal");
            return CoerceResult::Equality;
        }

        // Impossibilities
        if( src.m_data.is_Slice() )
        {
            // [T] can't unsize to anything
            DEBUG("Slice can't unsize");
            return CoerceResult::Equality;
        }

        // Can't Unsize to a known-Sized type.
        // BUT! Can do a Deref coercion to a Sized type.
        #if 0
        if( dst.m_data.is_Infer() && dst.m_data.as_Infer().index < context.m_ivars_sized.size() && context.m_ivars_sized.at( dst.m_data.as_Infer().index ) )
        {
            DEBUG("Can't unsize to known-Sized type");
            return CoerceResult::Equality;
        }
        #endif

        // Handle ivars specially
        if(dst.m_data.is_Infer() && src.m_data.is_Infer())
        {
            // If both are literals, equate
            if( dst.m_data.as_Infer().is_lit() && src.m_data.as_Infer().is_lit() )
            {
                DEBUG("Literal ivars");
                return CoerceResult::Equality;
            }
            if( context_mut )
            {
                context_mut->possible_equate_type_unsize_to(src.m_data.as_Infer().index, dst);
                context_mut->possible_equate_type_unsize_from(dst.m_data.as_Infer().index, src);
            }
            DEBUG("Both ivars");
            return CoerceResult::Unknown;
        }
        else if(const auto* dep = dst.m_data.opt_Infer())
        {
            // Literal from a primtive has to be equal
            if( dep->is_lit() && src.m_data.is_Primitive() )
            {
                DEBUG("Literal with primitive");
                return CoerceResult::Equality;
            }
            if( context_mut )
            {
                context_mut->possible_equate_type_unsize_from(dep->index, src);
            }
            DEBUG("Dst ivar");
            return CoerceResult::Unknown;
        }
        else if(const auto* sep = src.m_data.opt_Infer())
        {
            if(sep->is_lit())
            {
                if( !dst.m_data.is_TraitObject())
                {
                    // Literal to anything other than a trait object must be an equality
                    DEBUG("Literal with primitive");
                    return CoerceResult::Equality;
                }
                else
                {
                    // Fall through
                }
            }
            else
            {
                if( context_mut )
                {
                    context_mut->possible_equate_type_unsize_to(sep->index, dst);
                }
                DEBUG("Src is ivar (" << src << "), return Unknown");
                return CoerceResult::Unknown;
            }
        }
        else
        {
            // Neither side is an ivar, keep going.
        }

        // If either side is an unbound path, then return Unknown
        if( TU_TEST1(src.m_data, Path, .binding.is_Unbound()) )
        {
            DEBUG("Source unbound path");
            return CoerceResult::Unknown;
        }
        if( TU_TEST1(dst.m_data, Path, .binding.is_Unbound()) )
        {
            DEBUG("Destination unbound path");
            return CoerceResult::Unknown;
        }

        // Array unsize (quicker than going into deref search)
        if(dst.m_data.is_Slice() && src.m_data.is_Array())
        {
            if( context_mut )
            {
                context_mut->equate_types(sp, *dst.m_data.as_Slice().inner, *src.m_data.as_Array().inner);
            }
            if(node_ptr_ptr)
            {
                // TODO: Insert deref (instead of leading to a _Unsize op)
            }
            else
            {
                // Just return Unsize
            }
            DEBUG("Array => Slice");
            return CoerceResult::Unsize;
        }

        // Shortcut: Types that can't deref coerce (and can't coerce here because the target isn't a TraitObject)
        if( ! dst.m_data.is_TraitObject() )
        {
            if( src.m_data.is_Generic() )
            {
            }
            else if( src.m_data.is_Path() )
            {
            }
            else if( src.m_data.is_Borrow() )
            {
            }
            else
            {
                DEBUG("Target isn't a trait object, and sources can't Deref");
                return CoerceResult::Equality;
            }
        }

        // Deref coercions
        // - If right can be dereferenced to left
        if(node_ptr_ptr || !allow_mutate)
        {
            DEBUG("-- Deref coercions");
            ::HIR::TypeRef  tmp_ty;
            const ::HIR::TypeRef*   out_ty_p = &src;
            unsigned int count = 0;
            ::std::vector< ::HIR::TypeRef>  types;
            while( (out_ty_p = context.m_resolve.autoderef(sp, *out_ty_p, tmp_ty)) )
            {
                const auto& out_ty = context.m_ivars.get_type(*out_ty_p);
                count += 1;

                if( const auto* sep = out_ty.m_data.opt_Infer() )
                {
                    if( !sep->is_lit() )
                    {
                        // Hit a _, so can't keep going
                        if( context_mut )
                        {
                            // Could also be any deref chain of the destination type
                            ::HIR::TypeRef  tmp_ty2;
                            const ::HIR::TypeRef* d_ty_p = &dst;
                            context_mut->possible_equate_type_unsize_to(sep->index, dst);
                            for(unsigned int i = 0; i < count && (d_ty_p = context.m_resolve.autoderef(sp, *d_ty_p, tmp_ty2)); i ++)
                            {
                                context_mut->possible_equate_type_unsize_to(sep->index, *d_ty_p);
                            }
                        }
                        DEBUG("Src derefs to ivar (" << src << "), return Unknown");
                        return CoerceResult::Unknown;
                    }
                    // Literal infer, keep going (but remember how many times we dereferenced?)
                }

                if( TU_TEST1(out_ty.m_data, Path, .binding.is_Unbound()) )
                {
                    DEBUG("Src derefed to unbound type (" << out_ty << "), return Unknown");
                    return CoerceResult::Unknown;
                }

                types.push_back( out_ty.clone() );

                // Types aren't equal
                if( context.m_ivars.types_equal(dst, out_ty) == false )
                {
                    // Check if they can be considered equivalent.
                    // - E.g. a fuzzy match, or both are slices/arrays
                    if( dst.m_data.tag() != out_ty.m_data.tag() )
                    {
                        DEBUG("Different types");
                        continue ;
                    }

                    if( dst.m_data.is_Slice() )
                    {
                        if(context_mut)
                        {
                            context_mut->equate_types(sp, dst, out_ty);
                        }
                    }
                    else
                    {
                        if( dst .compare_with_placeholders(sp, out_ty, context.m_ivars.callback_resolve_infer()) == ::HIR::Compare::Unequal ) {
                            DEBUG("Same tag, but not fuzzy match");
                            continue ;
                        }
                        DEBUG("Same tag and fuzzy match - assuming " << dst << " == " << out_ty);
                        if( context_mut )
                        {
                            context_mut->equate_types(sp, dst, out_ty);
                        }
                    }
                }

                if( context_mut && node_ptr_ptr )
                {
                    auto& node_ptr = *node_ptr_ptr;
                    add_coerce_borrow(*context_mut, node_ptr, types.back(), [&](auto& node_ptr)->void {
                        // node_ptr = node that yeilds ty_src
                        assert( count == types.size() );
                        for(unsigned int i = 0; i < types.size(); i ++ )
                        {
                            auto span = node_ptr->span();
                            // TODO: Replace with a call to context.create_autoderef to handle cases where the below assertion would fire.
                            ASSERT_BUG(span, !node_ptr->m_res_type.m_data.is_Array(), "Array->Slice shouldn't be in deref coercions");
                            auto ty = mv$(types[i]);
                            node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Deref( mv$(span), mv$(node_ptr) ));
                            DEBUG("- Deref " << &*node_ptr << " -> " << ty);
                            node_ptr->m_res_type = mv$(ty);
                            context.m_ivars.get_type(node_ptr->m_res_type);
                        }
                        });
                }

                return CoerceResult::Custom;
            }
            // Either ran out of deref, or hit a _
            DEBUG("No applicable deref coercions");
        }

        // Trait objects
        if(const auto* dep = dst.m_data.opt_TraitObject())
        {
            if(const auto* sep = src.m_data.opt_TraitObject())
            {
                DEBUG("TraitObject => TraitObject");
                // Ensure that the trait list in the destination is a strict subset of the source

                // TODO: Equate these two trait paths
                if( dep->m_trait.m_path.m_path != sep->m_trait.m_path.m_path )
                {
                    // Trait mismatch!
                    return CoerceResult::Equality;
                }
                const auto& tys_d = dep->m_trait.m_path.m_params.m_types;
                const auto& tys_s = sep->m_trait.m_path.m_params.m_types;
                if( context_mut )
                {
                    for(size_t i = 0; i < tys_d.size(); i ++)
                    {
                        context_mut->equate_types(sp, tys_d[i], tys_s.at(i));
                    }
                }

                // 2. Destination markers must be a strict subset
                for(const auto& mt : dep->m_markers)
                {
                    // TODO: Fuzzy match
                    bool found = false;
                    for(const auto& omt : sep->m_markers) {
                        if( omt == mt ) {
                            found = true;
                            break;
                        }
                    }
                    if( !found ) {
                        // Return early.
                        return CoerceResult::Equality;
                    }
                }

                return CoerceResult::Unsize;
            }
            else
            {
                const auto& trait = dep->m_trait.m_path;

                // Check for trait impl
                if( trait.m_path != ::HIR::SimplePath() )
                {
                    // Just call equate_types_assoc to add the required bounds.
                    if( context_mut )
                    {
                        for(const auto& tyb : dep->m_trait.m_type_bounds)
                        {
                            context_mut->equate_types_assoc(sp, tyb.second,  trait.m_path, trait.m_params.clone(), src, tyb.first.c_str(), false);
                        }
                        if( dep->m_trait.m_type_bounds.empty() )
                        {
                            context_mut->add_trait_bound(sp, src,  trait.m_path, trait.m_params.clone());
                        }
                    }
                    else
                    {
                        // TODO: Should this check?
                    }
                }

                if( context_mut )
                {
                    for(const auto& marker : dep->m_markers)
                    {
                        context_mut->add_trait_bound(sp, src,  marker.m_path, marker.m_params.clone());
                    }
                }
                else
                {
                    // TODO: Should this check?
                }

                // Add _Unsize operator
                return CoerceResult::Unsize;
            }
        }

        // Find an Unsize impl?
        struct H {
            static bool type_is_bounded(const ::HIR::TypeRef& ty)
            {
                if( ty.m_data.is_Generic() ) {
                    return true;
                }
                else if( TU_TEST1(ty.m_data, Path, .binding.is_Opaque()) ) {
                    return true;
                }
                else {
                    return false;
                }
            }
        };
        if( H::type_is_bounded(src) )
        {
            const auto& lang_Unsize = context.m_crate.get_lang_item_path(sp, "unsize");
            DEBUG("Search for `Unsize<" << dst << ">` impl for `" << src << "`");

            ImplRef best_impl;
            unsigned int count = 0;

            ::HIR::PathParams   pp { dst.clone() };
            bool found = context.m_resolve.find_trait_impls(sp, lang_Unsize, pp, src, [&best_impl,&count,&context](auto impl, auto cmp){
                    DEBUG("[check_unsize_tys] Found impl " << impl << (cmp == ::HIR::Compare::Fuzzy ? " (fuzzy)" : ""));
                    if( !impl.overlaps_with(context.m_crate, best_impl) )
                    {
                        // No overlap, count it as a new possibility
                        if( count == 0 )
                            best_impl = mv$(impl);
                        count ++;
                    }
                    else if( impl.more_specific_than(best_impl) )
                    {
                        best_impl = mv$(impl);
                    }
                    else
                    {
                        // Less specific
                    }
                    // TODO: Record the best impl (if fuzzy) and equate params
                    return cmp != ::HIR::Compare::Fuzzy;
                    });
            if( found )
            {
                return CoerceResult::Unsize;
            }
            else if( count == 1 )
            {
                auto pp = best_impl.get_trait_params();
                DEBUG("Fuzzy, best was Unsize" << pp);
                if( context_mut )
                {
                    context_mut->equate_types(sp, dst, pp.m_types.at(0));
                }
                return CoerceResult::Unsize;
            }
            else
            {
                // TODO: Fuzzy?
                //context.equate_types(sp, *e.inner, *s_e.inner);
                DEBUG("Multiple impls for bounded unsize");
            }
        }

        // Path types
        if( src.m_data.tag() == dst.m_data.tag() )
        {
            TU_MATCH_HDRA( (src.m_data, dst.m_data), {)
            default:
                return CoerceResult::Equality;
            TU_ARMA(Path, se, de) {
                if( se.binding.tag() == de.binding.tag() )
                {
                    TU_MATCHA( (se.binding, de.binding), (sbe, dbe),
                    (Unbound,
                        // Don't care
                        ),
                    (Opaque,
                        // Handled above in bounded
                        ),
                    (ExternType,
                        // Must be equal
                        if( sbe == dbe )
                        {
                            return CoerceResult::Equality;
                        }
                        ),
                    (Enum,
                        // Must be equal
                        if( sbe == dbe )
                        {
                            return CoerceResult::Equality;
                        }
                        ),
                    (Union,
                        if( sbe == dbe )
                        {
                            // Must be equal
                            return CoerceResult::Equality;
                        }
                        ),
                    (Struct,
                        if( sbe == dbe )
                        {
                            const auto& sm = sbe->m_struct_markings;
                            if( sm.dst_type == ::HIR::StructMarkings::DstType::Possible )
                            {
                                const auto& isrc = se.path.m_data.as_Generic().m_params.m_types.at(sm.unsized_param);
                                const auto& idst = de.path.m_data.as_Generic().m_params.m_types.at(sm.unsized_param);
                                return check_unsize_tys(context_mut_r, sp, idst, isrc, nullptr, allow_mutate);
                            }
                            else
                            {
                                // Must be equal
                                return CoerceResult::Equality;
                            }
                        }
                        )
                    )
                }
                }
            }
        }

        // If the destination is an Unbound path, return Unknown
        if( TU_TEST1(dst.m_data, Path, .binding.is_Unbound()) )
        {
            DEBUG("Unbound destination");
            return CoerceResult::Unknown;
        }


        DEBUG("Reached end of check_unsize_tys, return Equality");
        // TODO: Determine if this unsizing could ever happen.
        return CoerceResult::Equality;
    }

    /// Checks if two types can be a valid coercion
    //
    // General rules:
    // - CoerceUnsized generics/associated types can only involve generics/associated types
    // - CoerceUnsized structs only go between themselves (and either recurse or unsize a parameter)
    // - No other path can implement CoerceUnsized
    // - Pointers do unsizing (and maybe casting)
    // - All other types equate
    CoerceResult check_coerce_tys(Context& context, const Span& sp, const ::HIR::TypeRef& dst, const ::HIR::TypeRef& src, ::HIR::ExprNodeP* node_ptr_ptr=nullptr)
    {
        TRACE_FUNCTION_F(dst << " := " << src);
        // If the types are equal, then return equality
        if( context.m_ivars.types_equal(dst, src) ) {
            return CoerceResult::Equality;
        }
        // If either side is a literal, then can't Coerce
        if( TU_TEST1(dst.m_data, Infer, .is_lit()) ) {
            return CoerceResult::Equality;
        }
        if( TU_TEST1(src.m_data, Infer, .is_lit()) ) {
            return CoerceResult::Equality;
        }

        // TODO: If the destination is bounded to be Sized, equate and return.
        // If both sides are `_`, then can't know about coerce yet
        if( dst.m_data.is_Infer() && src.m_data.is_Infer() ) {
            // Add possibilities both ways
            context.possible_equate_type_coerce_to(src.m_data.as_Infer().index, dst);
            context.possible_equate_type_coerce_from(dst.m_data.as_Infer().index, src);
            return CoerceResult::Unknown;
        }

        struct H {
            static bool type_is_bounded(const ::HIR::TypeRef& ty)
            {
                if( ty.m_data.is_Generic() ) {
                    return true;
                }
                else if( TU_TEST1(ty.m_data, Path, .binding.is_Opaque()) ) {
                    return true;
                }
                else {
                    return false;
                }
            }
            static ::HIR::TypeRef make_pruned(Context& context, const ::HIR::TypeRef& ty)
            {
                const auto& binding = ty.m_data.as_Path().binding;
                const auto& sm = binding.as_Struct()->m_struct_markings;
                ::HIR::GenericPath gp = ty.m_data.as_Path().path.m_data.as_Generic().clone();
                assert(sm.coerce_param != ~0u);
                gp.m_params.m_types.at(sm.coerce_param) = context.m_ivars.new_ivar_tr();
                return ::HIR::TypeRef::new_path(mv$(gp), binding.as_Struct());
            }
        };
        // A CoerceUnsized generic/aty/erased on one side
        // - If other side is an ivar, do a possible equality and return Unknown
        // - If impl is found, emit _Unsize
        // - Else, equate and return
        // TODO: Should ErasedType be counted here? probably not.
        if( H::type_is_bounded(src) || H::type_is_bounded(dst) )
        {
            const auto& lang_CoerceUnsized = context.m_crate.get_lang_item_path(sp, "coerce_unsized");
            // `CoerceUnsized<U> for T` means `T -> U`

            ::HIR::PathParams   pp { dst.clone() };

            // PROBLEM: This can false-negative leading to the types being falsely equated.

            bool fuzzy_match = false;
            ImplRef  best_impl;
            bool found = context.m_resolve.find_trait_impls(sp, lang_CoerceUnsized, pp, src, [&](auto impl, auto cmp)->bool {
                DEBUG("[check_coerce] cmp=" << cmp << ", impl=" << impl);
                // TODO: Allow fuzzy match if it's the only matching possibility?
                // - Recorded for now to know if there could be a matching impl later
                if( cmp == ::HIR::Compare::Fuzzy ) {
                    fuzzy_match = true;
                    if( impl.more_specific_than(best_impl) ) {
                        best_impl = mv$(impl);
                    }
                    else {
                        TODO(sp, "Equal specificity impls");
                    }
                }
                return cmp == ::HIR::Compare::Equal;
                });
            // - Concretely found - emit the _Unsize op and remove this rule
            if( found )
            {
                return CoerceResult::Unsize;
            }
            if( fuzzy_match )
            {
                DEBUG("- best_impl = " << best_impl);
                return CoerceResult::Unknown;
            }
            DEBUG("- No CoerceUnsized impl found");
        }

        // CoerceUnsized struct paths
        // - If one side is an ivar, create a type-pruned version of the other
        // - Recurse/unsize inner value
        if( src.m_data.is_Infer() && TU_TEST2(dst.m_data, Path, .binding, Struct, ->m_struct_markings.coerce_unsized != ::HIR::StructMarkings::Coerce::None) )
        {
#if 0
            auto new_src = H::make_pruned(context, dst);
            context.equate_types(sp, src, new_src);
#else
            context.possible_equate_type_coerce_to(src.m_data.as_Infer().index, dst);
#endif
            // TODO: Avoid needless loop return
            return CoerceResult::Unknown;
        }
        if( dst.m_data.is_Infer() && TU_TEST2(src.m_data, Path, .binding, Struct, ->m_struct_markings.coerce_unsized != ::HIR::StructMarkings::Coerce::None) )
        {
#if 0
            auto new_dst = H::make_pruned(context, src);
            context.equate_types(sp, dst, new_dst);
#else
            context.possible_equate_type_coerce_from(dst.m_data.as_Infer().index, src);
#endif
            // TODO: Avoid needless loop return
            return CoerceResult::Unknown;
        }
        if( TU_TEST1(dst.m_data, Path, .binding.is_Struct()) && TU_TEST1(src.m_data, Path, .binding.is_Struct()) )
        {
            const auto& spbe = src.m_data.as_Path().binding.as_Struct();
            const auto& dpbe = dst.m_data.as_Path().binding.as_Struct();
            if( spbe != dpbe )
            {
                // TODO: Error here? (equality in caller will cause an error)
                return CoerceResult::Equality;
            }
            const auto& sm = spbe->m_struct_markings;
            // Has to be equal?
            if( sm.coerce_unsized == ::HIR::StructMarkings::Coerce::None )
                return CoerceResult::Equality;
            const auto& idst = dst.m_data.as_Path().path.m_data.as_Generic().m_params.m_types.at(sm.coerce_param);
            const auto& isrc = src.m_data.as_Path().path.m_data.as_Generic().m_params.m_types.at(sm.coerce_param);
            switch( sm.coerce_unsized )
            {
            case ::HIR::StructMarkings::Coerce::None:
                throw "";
            case ::HIR::StructMarkings::Coerce::Passthrough:
                DEBUG("Passthough CoerceUnsized");
                // TODO: Force emitting `_Unsize` instead of anything else
                return check_coerce_tys(context, sp, idst, isrc, nullptr);
            case ::HIR::StructMarkings::Coerce::Pointer:
                DEBUG("Pointer CoerceUnsized");
                return check_unsize_tys(context, sp, idst, isrc, nullptr);
            }
        }

        // If either side is an unbound UFCS, can't know yet
        if( TU_TEST1(dst.m_data, Path, .binding.is_Unbound()) || TU_TEST1(src.m_data, Path, .binding.is_Unbound()) )
        {
            return CoerceResult::Unknown;
        }

        // Any other type, check for pointer
        // - If not a pointer, return Equality
        if(const auto* sep = src.m_data.opt_Infer())
        {
            const auto& se = *sep;
            ASSERT_BUG(sp, ! dst.m_data.is_Infer(), "Already handled?");

            // If the other side isn't a pointer, equate
            if( dst.m_data.is_Pointer() || dst.m_data.is_Borrow() )
            {
                context.possible_equate_type_coerce_to(se.index, dst);
                return CoerceResult::Unknown;
            }
            else
            {
                return CoerceResult::Equality;
            }
        }
        else if(const auto* sep = src.m_data.opt_Pointer())
        {
            const auto& se = *sep;
            if( const auto* dep = dst.m_data.opt_Infer() )
            {
                context.possible_equate_type_coerce_from(dep->index, src);
                return CoerceResult::Unknown;
            }
            if( ! dst.m_data.is_Pointer() )
            {
                // TODO: Error here? (leave to caller)
                return CoerceResult::Equality;
            }
            // Pointers coerce to similar pointers of higher restriction
            if( se.type == ::HIR::BorrowType::Shared )
            {
                // *const is the bottom of the tree, it doesn't coerce to anything
                return CoerceResult::Equality;
            }
            const auto* dep = &dst.m_data.as_Pointer();
        
            // If using `*mut T` where `*const T` is expected - add cast
            if( dep->type == ::HIR::BorrowType::Shared && se.type == ::HIR::BorrowType::Unique )
            {
                context.equate_types(sp, *dep->inner, *se.inner);

                if( node_ptr_ptr )
                {
                    auto& node_ptr = *node_ptr_ptr;
                    // Add cast down
                    auto span = node_ptr->span();
                    //node_ptr->m_res_type = src.clone();
                    node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Cast( mv$(span), mv$(node_ptr), dst.clone() ));
                    node_ptr->m_res_type = dst.clone();

                    return CoerceResult::Custom;
                }
                else
                {
                    return CoerceResult::Unsize;
                }
            }

            if( dep->type != se.type ) {
                ERROR(sp, E0000, "Type mismatch between " << dst << " and " << src << " - Pointer mutability differs");
            }
            return CoerceResult::Equality;
        }
        else if(const auto* sep = src.m_data.opt_Borrow())
        {
            const auto& se = *sep;
            if( const auto* dep = dst.m_data.opt_Infer() )
            {
                context.possible_equate_type_coerce_from(dep->index, src);
                return CoerceResult::Unknown;
            }
            else if( const auto* dep = dst.m_data.opt_Pointer() )
            {
                // Add cast to the pointer (if valid strength reduction)
                // Call unsizing code on casted value
                
                // Borrows can coerce to pointers while reducing in strength
                // - Shared < Unique. If the destination is not weaker or equal to the source, it's an error
                if( !(dep->type <= se.type) ) {
                    ERROR(sp, E0000, "Type mismatch between " << dst << " and " << src << " - Mutability not compatible");
                }

                // Add downcast
                if( node_ptr_ptr )
                {
                    auto& node_ptr = *node_ptr_ptr;

                    switch( check_unsize_tys(context, sp, *dep->inner, *se.inner, node_ptr_ptr) )
                    {
                    case CoerceResult::Unknown:
                        return CoerceResult::Unknown;
                    case CoerceResult::Custom:
                        return CoerceResult::Custom;
                    case CoerceResult::Equality:
                        DEBUG("- NEWNODE _Cast " << &*node_ptr << " -> " << dst);
                        {
                            auto span = node_ptr->span();
                            node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Cast( mv$(span), mv$(node_ptr), dst.clone() ));
                            node_ptr->m_res_type = dst.clone();
                        }

                        context.equate_types(sp, *dep->inner, *se.inner);
                        return CoerceResult::Custom;
                    case CoerceResult::Unsize:
                        auto dst_b = ::HIR::TypeRef::new_borrow(se.type, dep->inner->clone());
                        DEBUG("- NEWNODE _Unsize " << &*node_ptr << " -> " << dst_b);
                        {
                            auto span = node_ptr->span();
                            node_ptr = NEWNODE( dst_b.clone(), span, _Unsize,  mv$(node_ptr), dst_b.clone() );
                        }

                        DEBUG("- NEWNODE _Cast " << &*node_ptr << " -> " << dst);
                        {
                            auto span = node_ptr->span();
                            node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Cast( mv$(span), mv$(node_ptr), dst.clone() ));
                            node_ptr->m_res_type = dst.clone();
                        }
                        return CoerceResult::Custom;
                    }
                    throw "";
                }
                else
                {
                    TODO(sp, "Inner coercion of borrow to pointer");
                    return CoerceResult::Custom;
                }
            }
            else if( const auto* dep = dst.m_data.opt_Borrow() )
            {
                // Check strength reduction
                if( dep->type < se.type )
                {
                    if( node_ptr_ptr )
                    {
                        // > Goes from `src` -> `*src` -> `&`dep->type` `*src`
                        const auto inner_ty = se.inner->clone();
                        auto dst_bt = dep->type;
                        auto new_type = ::HIR::TypeRef::new_borrow(dst_bt, inner_ty.clone());

                        // If the coercion is of a block, do the reborrow on the last node of the block
                        // - Cleans up the dumped MIR and prevents needing a reborrow elsewhere.
                        // - TODO: Alter the block's result types
                        ::HIR::ExprNodeP* npp = node_ptr_ptr;
                        while( auto* p = dynamic_cast< ::HIR::ExprNode_Block*>(&**npp) )
                        {
                            DEBUG("- Propagate to the last node of a _Block");
                            ASSERT_BUG( p->span(), context.m_ivars.types_equal(p->m_res_type, p->m_value_node->m_res_type),
                                "Block and result mismatch - " << context.m_ivars.fmt_type(p->m_res_type) << " != " << context.m_ivars.fmt_type(p->m_value_node->m_res_type));
                            ASSERT_BUG( p->span(), context.m_ivars.types_equal(p->m_res_type, src),
                                "Block and result mismatch - " << context.m_ivars.fmt_type(p->m_res_type) << " != " << context.m_ivars.fmt_type(src)
                                );
                            p->m_res_type = dst.clone();
                            npp = &p->m_value_node;
                        }
                        ::HIR::ExprNodeP& node_ptr = *npp;

                        // Add cast down
                        auto span = node_ptr->span();
                        // *<inner>
                        DEBUG("- Deref -> " << inner_ty);
                        node_ptr = NEWNODE( inner_ty.clone(), span, _Deref,  mv$(node_ptr) );
                        context.m_ivars.get_type(node_ptr->m_res_type);
                        // &*<inner>
                        DEBUG("- Borrow -> " << new_type);
                        node_ptr = NEWNODE( mv$(new_type) , span, _Borrow,  dst_bt, mv$(node_ptr) );
                        context.m_ivars.get_type(node_ptr->m_res_type);

                        context.m_ivars.mark_change();

                        // Continue on with coercion (now that node_ptr is updated)
                        switch( check_unsize_tys(context, sp, *dep->inner, *se.inner, &node_ptr) )
                        {
                        case CoerceResult::Unknown:
                            // Add new coercion at the new inner point
                            if( &node_ptr != node_ptr_ptr )
                            {
                                DEBUG("Unknown check_unsize_tys after autoderef - " << dst << " := " << node_ptr->m_res_type);
                                context.equate_types_coerce(sp, dst, node_ptr);
                                return CoerceResult::Custom;
                            }
                            else
                            {
                                return CoerceResult::Unknown;
                            }
                        case CoerceResult::Custom:
                            return CoerceResult::Custom;
                        case CoerceResult::Equality:
                            context.equate_types(sp, *dep->inner, *se.inner);
                            return CoerceResult::Custom;
                        case CoerceResult::Unsize:
                            DEBUG("- NEWNODE _Unsize " << &node_ptr << " " << &*node_ptr << " -> " << dst);
                            auto span = node_ptr->span();
                            node_ptr = NEWNODE( dst.clone(), span, _Unsize,  mv$(node_ptr), dst.clone() );
                            return CoerceResult::Custom;
                        }
                        throw "";
                    }
                    else
                    {
                        //TODO(sp, "Borrow strength reduction with no node pointer - " << src << " -> " << dst);
                        DEBUG("Borrow strength reduction with no node pointer - " << src << " -> " << dst);
                        return CoerceResult::Unsize;
                    }
                }
                else if( dep->type == se.type ) {
                    // Valid.
                }
                else {
                    ERROR(sp, E0000, "Type mismatch between " << dst << " and " << src << " - Borrow classes differ");
                }
                ASSERT_BUG(sp, dep->type == se.type, "Borrow strength mismatch");

                // Call unsizing code
                return check_unsize_tys(context, sp, *dep->inner, *se.inner, node_ptr_ptr);
            }
            else
            {
                // TODO: Error here?
                return CoerceResult::Equality;
            }
        }
        else if( src.m_data.is_Closure() )
        {
            const auto& se = src.m_data.as_Closure();
            if( dst.m_data.is_Function() )
            {
                const auto& de = dst.m_data.as_Function();
                auto& node_ptr = *node_ptr_ptr;
                auto span = node_ptr->span();
                if( de.m_abi != ABI_RUST ) {
                    ERROR(span, E0000, "Cannot use closure for extern function pointer");
                }
                if( de.m_arg_types.size() != se.m_arg_types.size() ) {
                    ERROR(span, E0000, "Mismatched argument count coercing closure to fn(...)");
                }
                for(size_t i = 0; i < de.m_arg_types.size(); i++)
                {
                    context.equate_types(sp, de.m_arg_types[i], se.m_arg_types[i]);
                }
                context.equate_types(sp, *de.m_rettype, *se.m_rettype);
                node_ptr = NEWNODE( dst.clone(), span, _Cast,  mv$(node_ptr), dst.clone() );
                return CoerceResult::Custom;
            }
            else if( const auto* dep = dst.m_data.opt_Infer() )
            {
                // Prevent inferrence of argument/return types
                for(const auto& at : se.m_arg_types)
                    context.equate_types_to_shadow(sp, at);
                context.equate_types_to_shadow(sp, *se.m_rettype);
                // Add as a possiblity
                context.possible_equate_type_coerce_from(dep->index, src);
                return CoerceResult::Unknown;
            }
            else
            {
                return CoerceResult::Equality;
            }
        }
        else if( const auto* se = src.m_data.opt_Function() )
        {
            if( const auto* de = dst.m_data.opt_Function() )
            {
                auto& node_ptr = *node_ptr_ptr;
                auto span = node_ptr->span();
                DEBUG("Function pointer coercion");
                // ABI must match
                if( se->m_abi != de->m_abi )
                    return CoerceResult::Equality;
                // const can be removed
                //if( se->is_const != de->is_const && de->is_const ) // Error going TO a const function pointer
                //    return CoerceResult::Equality;
                // unsafe can be added
                if( se->is_unsafe != de->is_unsafe && se->is_unsafe ) // Error going FROM an unsafe function pointer
                    return CoerceResult::Equality;
                // argument/return types must match
                if( de->m_arg_types.size() != se->m_arg_types.size() )
                    return CoerceResult::Equality;
                for(size_t i = 0; i < de->m_arg_types.size(); i++)
                {
                    context.equate_types(sp, de->m_arg_types[i], se->m_arg_types[i]);
                }
                context.equate_types(sp, *de->m_rettype, *se->m_rettype);
                node_ptr = NEWNODE( dst.clone(), span, _Cast,  mv$(node_ptr), dst.clone() );
                return CoerceResult::Custom;
            }
            else
            {
                return CoerceResult::Equality;
            }
        }
        else
        {
            // TODO: ! should be handled above or in caller?
            return CoerceResult::Equality;
        }
    }
    bool check_coerce(Context& context, const Context::Coercion& v)
    {
        ::HIR::ExprNodeP& node_ptr = *v.right_node_ptr;
        const auto& sp = node_ptr->span();
        const auto& ty_dst = context.m_ivars.get_type(v.left_ty);
        const auto& ty_src = context.m_ivars.get_type(node_ptr->m_res_type);
        TRACE_FUNCTION_F(v << " - " << context.m_ivars.fmt_type(ty_dst) << " := " << context.m_ivars.fmt_type(ty_src));

        // NOTE: Coercions can happen on comparisons, which means that checking for Sized isn't valid (because you can compare unsized types)

        switch( check_coerce_tys(context, sp, ty_dst, ty_src, &node_ptr) )
        {
        case CoerceResult::Unknown:
            DEBUG("Unknown - keep");
            return false;
        case CoerceResult::Custom:
            DEBUG("Custom - Completed");
            return true;
        case CoerceResult::Equality:
            DEBUG("Trigger equality - Completed");
            context.equate_types(sp, ty_dst, ty_src);
            return true;
        case CoerceResult::Unsize:
            DEBUG("Add _Unsize " << &*node_ptr << " -> " << ty_dst);
            auto span = node_ptr->span();
            node_ptr = NEWNODE( ty_dst.clone(), span, _Unsize,  mv$(node_ptr), ty_dst.clone() );
            return true;
        }
        throw "";
    }

    bool check_associated(Context& context, const Context::Associated& v)
    {
        const auto& sp = v.span;
        TRACE_FUNCTION_F(v);

        ::HIR::TypeRef  output_type;

        struct H {
            static bool type_is_num(const ::HIR::TypeRef& t) {
                TU_MATCH_DEF(::HIR::TypeData, (t.m_data), (e),
                ( return false; ),
                (Primitive,
                    switch(e)
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                        return false;
                    default:
                        return true;
                    }
                    ),
                (Infer,
                    return e.ty_class != ::HIR::InferClass::None;
                    )
                )
            }
        };

        // MAGIC! Have special handling for operator overloads
        if( v.is_operator ) {
            if( v.params.m_types.size() == 0 )
            {
                // Uni ops = If the value is a primitive, the output is the same type
                const auto& ty = context.get_type(v.impl_ty);
                const auto& res = context.get_type(v.left_ty);
                if( H::type_is_num(ty) ) {
                    DEBUG("- Magic inferrence link for uniops on numerics");
                    context.equate_types(sp, res, ty);
                }
            }
            else if( v.params.m_types.size() == 1 )
            {
                // Binary operations - If both types are primitives, the output is the lefthand side
                const auto& left = context.get_type(v.impl_ty); // yes, impl = LHS of binop
                const auto& right = context.get_type(v.params.m_types.at(0));
                const auto& res = context.get_type(v.left_ty);
                if( H::type_is_num(left) && H::type_is_num(right) ) {
                    DEBUG("- Magic inferrence link for binops on numerics");
                    context.equate_types(sp, res, left);
                }
                context.equate_types_to_shadow(sp, /*right*/v.params.m_types.at(0)); // RHS, can't use `right` because it might be freed by the above equate.
            }
            else
            {
                BUG(sp, "Associated type rule with `is_operator` set but an incorrect parameter count");
            }
        }

        // If the output type is present, prevent it from being guessed
        // - This generates an exact equation.
        if( v.left_ty != ::HIR::TypeRef() )
        {
            context.equate_types_shadow_strong(sp, v.left_ty);
        }

        // HACK? Soft-prevent inferrence of the param types
        for(const auto& t : v.params.m_types)
        {
            context.equate_types_to_shadow(sp, t);
        }

        // If the impl type is an unbounded ivar, and there's no trait args - don't bother searching
        if( const auto* e = context.m_ivars.get_type(v.impl_ty).m_data.opt_Infer() )
        {
            // TODO: ?
            if( !e->is_lit() && v.params.m_types.empty() )
            {
                return false;
            }
        }

        // Locate applicable trait impl
        unsigned int count = 0;
        DEBUG("Searching for impl " << v.trait << v.params << " for " << context.m_ivars.fmt_type(v.impl_ty));
        struct Possibility {
            ::HIR::TypeRef  impl_ty;
            ::HIR::PathParams   params;
            ImplRef  impl_ref;
        };
        ::std::vector<Possibility>  possible_impls;
        bool found = context.m_resolve.find_trait_impls(sp, v.trait, v.params,  v.impl_ty,
            [&](auto impl, auto cmp) {
                DEBUG("[check_associated] Found cmp=" << cmp << " " << impl);
                if( v.name != "" ) {
                    auto out_ty_o = impl.get_type(v.name.c_str());
                    if( out_ty_o == ::HIR::TypeRef() )
                    {
                        out_ty_o = ::HIR::TypeRef::new_path(::HIR::Path( v.impl_ty.clone(), ::HIR::GenericPath(v.trait, v.params.clone()), v.name, ::HIR::PathParams() ), {});
                    }
                    out_ty_o = context.m_resolve.expand_associated_types(sp, mv$(out_ty_o));

                    // TODO: if this is an unbound UfcsUnknown, treat as a fuzzy match.
                    // - Shouldn't compare_with_placeholders do that?
                    const auto& out_ty = out_ty_o;

                    // - If we're looking for an associated type, allow it to eliminate impossible impls
                    //  > This makes `let v: usize = !0;` work without special cases
                    auto cmp2 = v.left_ty.compare_with_placeholders(sp, out_ty, context.m_ivars.callback_resolve_infer());
                    if( cmp2 == ::HIR::Compare::Unequal ) {
                        DEBUG("[check_associated] - (fail) known result can't match (" << context.m_ivars.fmt_type(v.left_ty) << " and " << context.m_ivars.fmt_type(out_ty) << ")");
                        return false;
                    }
                    // if solid or fuzzy, leave as-is
                    output_type = mv$( out_ty_o );
                    DEBUG("[check_associated] cmp = " << cmp << " (2)");
                }
                if( cmp == ::HIR::Compare::Equal ) {
                    // NOTE: Sometimes equal can be returned when it's not 100% equal (TODO)
                    // - Equate the types
                    auto itp = impl.get_trait_params();
                    assert( v.params.m_types.size() == itp.m_types.size() );
                    for(unsigned int i = 0; i < v.params.m_types.size(); i ++)
                    {
                        context.equate_types(sp, v.params.m_types[i], itp.m_types[i]);
                    }
                    return true;
                }
                else {
                    count += 1;
                    DEBUG("[check_associated] - (possible) " << impl);

                    if( possible_impls.empty() ) {
                        DEBUG("[check_associated] First - " << impl);
                        possible_impls.push_back({ impl.get_impl_type(), impl.get_trait_params(), mv$(impl) });
                    }
                    // If there is an existing impl, determine if this is part of the same specialisation tree
                    // - If more specific, replace. If less, ignore.
                    // NOTE: `overlaps_with` (should be) reflective
                    else
                    {
                        bool was_used = false;
                        for(auto& possible_impl : possible_impls)
                        {
                            const auto& best_impl = possible_impl.impl_ref;
                            // TODO: Handle duplicates (from overlapping bounds)
                            if( impl.overlaps_with(context.m_crate, best_impl) )
                            {
                                DEBUG("[check_associated] - Overlaps with existing - " << best_impl);
                                // if not more specific than the existing best, ignore.
                                if( ! impl.more_specific_than(best_impl) )
                                {
                                    DEBUG("[check_associated] - Less specific than existing");
                                    // NOTE: This picks the _least_ specific impl
                                    possible_impl.impl_ty = impl.get_impl_type();
                                    possible_impl.params = impl.get_trait_params();
                                    possible_impl.impl_ref = mv$(impl);
                                    count -= 1;
                                }
                                // If the existing best is not more specific than the new one, use the new one
                                else if( ! best_impl.more_specific_than(impl) )
                                {
                                    DEBUG("[check_associated] - More specific than existing - " << best_impl);
                                    count -= 1;
                                }
                                else
                                {
                                    // Supposedly, `more_specific_than` should be reflexive...
                                    DEBUG("[check_associated] > Neither is more specific. Error?");
                                }
                                was_used = true;
                                break;
                            }
                            else
                            {
                                // Disjoint impls.
                                DEBUG("[check_associated] Disjoint with " << best_impl);
                            }

                            // Edge case: Might be just outright identical
                            if( v.name == "" && possible_impl.impl_ty == impl.get_impl_type() && possible_impl.params == impl.get_trait_params() )
                            {
                                DEBUG("[check_associated] HACK: Same type and params, and don't care about ATYs");
                                was_used = true;
                                count -= 1;
                                break;
                            }
                        }
                        if( !was_used )
                        {
                            DEBUG("[check_associated] Add new possible: " << impl);
                            possible_impls.push_back({ impl.get_impl_type(), impl.get_trait_params(), mv$(impl) });
                        }
                    }

                    return false;
                }
            });
        if( found ) {
            // Fully-known impl
            DEBUG("Fully-known impl located");
            if( v.name != "" ) {
                // Stop this from just pushing the same rule again.
                if( output_type.m_data.is_Path() && output_type.m_data.as_Path().path.m_data.is_UfcsKnown() )
                {
                    const auto& te = output_type.m_data.as_Path();
                    const auto& pe = te.path.m_data.as_UfcsKnown();
                    // If the target type is unbound, and is this rule exactly, don't return success
                    if( te.binding.is_Unbound() && *pe.type == v.impl_ty && pe.item == v.name && pe.trait.m_path == v.trait && pe.trait.m_params == v.params)
                    {
                        DEBUG("Would re-create the same rule, returning unconsumed");
                        return false;
                    }
                }
                context.equate_types(sp, v.left_ty, output_type);
            }
            // TODO: Any equating of type params?
            return true;
        }
        else if( count == 0 ) {
            // No applicable impl
            // - TODO: This should really only fire when there isn't an impl. But it currently fires when _
            if( v.name == "" )
                DEBUG("No impl of " << v.trait << context.m_ivars.fmt(v.params) << " for " << context.m_ivars.fmt_type(v.impl_ty));
            else
                DEBUG("No impl of " << v.trait << context.m_ivars.fmt(v.params) << " for " << context.m_ivars.fmt_type(v.impl_ty)
                    << " with " << v.name << " = " << context.m_ivars.fmt_type(v.left_ty));

            const auto& ty = context.get_type(v.impl_ty);
            bool is_known = !ty.m_data.is_Infer() && !(ty.m_data.is_Path() && ty.m_data.as_Path().binding.is_Unbound());
            //bool is_known = !context.m_ivars.type_contains_ivars(v.impl_ty);
            //for(const auto& t : v.params.m_types)
            //    is_known &= !context.m_ivars.type_contains_ivars(t);
            if( !is_known )
            {
                // There's still an ivar (or an unbound UFCS), keep trying
                return false;
            }
            else if( v.trait == context.m_crate.get_lang_item_path(sp, "unsize") )
            {
                // TODO: Detect if this was a compiler-generated bound, or was actually in the code.

                ASSERT_BUG(sp, v.params.m_types.size() == 1, "Incorrect number of parameters for Unsize");
                const auto& src_ty = context.get_type(v.impl_ty);
                const auto& dst_ty = context.get_type(v.params.m_types[0]);

                context.equate_types(sp, dst_ty, src_ty);
                return true;
            }
            else
            {
                if( v.name == "" )
                    ERROR(sp, E0000, "Failed to find an impl of " << v.trait << context.m_ivars.fmt(v.params) << " for " << context.m_ivars.fmt_type(v.impl_ty));
                else
                    ERROR(sp, E0000, "Failed to find an impl of " << v.trait << context.m_ivars.fmt(v.params) << " for " << context.m_ivars.fmt_type(v.impl_ty)
                        << " with " << v.name << " = " << context.m_ivars.fmt_type(v.left_ty));
            }
        }
        else if( count == 1 ) {
            auto& possible_impl_ty = possible_impls.at(0).impl_ty;
            auto& possible_params = possible_impls.at(0).params;
            auto& best_impl = possible_impls.at(0).impl_ref;
            DEBUG("Only one impl " << v.trait << context.m_ivars.fmt(possible_params) << " for " << context.m_ivars.fmt_type(possible_impl_ty)
                << " - out=" << output_type);
            // - If there are any magic params in the impl, don't use it yet.
            //  > Ideally, there should be a match_test_generics to resolve the magic impls.
            DEBUG("> best_impl=" << best_impl);
            if( best_impl.has_magic_params() ) {
                // TODO: Pick this impl, and evaluate it (expanding the magic params out)
                DEBUG("> Magic params present, wait");
                return false;
            }
            const auto& impl_ty = context.m_ivars.get_type(v.impl_ty);
            if( TU_TEST1(impl_ty.m_data, Path, .binding.is_Unbound()) )
            {
                DEBUG("Unbound UfcsKnown, waiting");
                return false;
            }
            if( TU_TEST1(impl_ty.m_data, Infer, .is_lit() == false) )
            {
                DEBUG("Unbounded ivar, waiting - TODO: Add possibility " << impl_ty << " == " << possible_impl_ty);
                //context.possible_equate_type_bound(sp, impl_ty.m_data.as_Infer().index, possible_impl_ty);
                return false;
            }
            // Only one possible impl
            if( v.name != "" )
            {
                // If the output type is just < v.impl_ty as v.trait >::v.name, return false
                if( TU_TEST1(output_type.m_data, Path, .path.m_data.is_UfcsKnown()) )
                {
                    const auto& pe = output_type.m_data.as_Path().path.m_data.as_UfcsKnown();
                    if( *pe.type == v.impl_ty && pe.trait.m_path == v.trait && pe.trait.m_params == v.params && pe.item == v.name )
                    {
                        DEBUG("- Attempted recursion, stopping it");
                        return false;
                    }
                }
                context.equate_types(sp, v.left_ty, output_type);
            }
            assert( possible_impl_ty != ::HIR::TypeRef() );
            context.equate_types(sp, v.impl_ty, possible_impl_ty);
            for( unsigned int i = 0; i < possible_params.m_types.size(); i ++ ) {
                context.equate_types(sp, v.params.m_types[i], possible_params.m_types[i]);
            }
            // - Obtain the bounds required for this impl and add those as trait bounds to check/equate
            if( const auto* ep = best_impl.m_data.opt_TraitImpl() )
            {
                const auto& e = *ep;
                assert(e.impl);
                for(const auto& bound : e.impl->m_params.m_bounds )
                {
                    TU_MATCH_DEF(::HIR::GenericBound, (bound), (be),
                    (
                        ),
                    (TraitBound,
                        DEBUG("New bound (pre-mono) " << bound);
                        auto b_ty_mono = monomorphise_type_with(sp, be.type, best_impl.get_cb_monomorph_traitimpl(sp));
                        auto b_tp_mono = monomorphise_traitpath_with(sp, be.trait, best_impl.get_cb_monomorph_traitimpl(sp), true);
                        DEBUG("- " << b_ty_mono << " : " << b_tp_mono);
                        if( b_tp_mono.m_type_bounds.size() > 0 )
                        {
                            for(const auto& aty_bound : b_tp_mono.m_type_bounds)
                            {
                                context.equate_types_assoc(sp, aty_bound.second,  b_tp_mono.m_path.m_path, b_tp_mono.m_path.m_params.clone(), b_ty_mono, aty_bound.first.c_str(), false);
                            }
                        }
                        else
                        {
                            context.add_trait_bound(sp, b_ty_mono,  b_tp_mono.m_path.m_path, mv$(b_tp_mono.m_path.m_params));
                        }
                        )
                    )
                }
            }
            return true;
        }
        else {
            // Multiple possible impls, don't know yet
            DEBUG("Multiple impls");
            for(const auto& pi : possible_impls)
            {
                DEBUG(pi.params << " for " << pi.impl_ty);
                for(size_t i = 0; i < pi.params.m_types.size(); i++)
                {
                    const auto& t = context.get_type(v.params.m_types[i]);
                    if( const auto* e = t.m_data.opt_Infer() ) {
                        const auto& pi_t = pi.params.m_types[i];
                        if( !type_contains_impl_placeholder(pi_t) )
                        {
                            context.possible_equate_type_bound(sp, e->index, pi_t);
                        }
                        else
                        {
                            DEBUG("Not adding placeholder-containing type as a bound - " << pi_t);
                        }
                    }
                }
            }
            return false;
        }
    }

    bool check_ivar_poss__fails_bounds(const Span& sp, Context& context, const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& new_ty)
    {
        for(const auto& bound : context.link_assoc)
        {
            bool used_ty = false;
            auto cb = [&](const ::HIR::TypeRef& ty, ::HIR::TypeRef& out_ty){ if( ty == ty_l ) { out_ty = new_ty.clone(); used_ty = true; return true; } else { return false; }};
            auto t = clone_ty_with(sp, bound.impl_ty, cb);
            auto p = clone_path_params_with(sp, bound.params, cb);
            if(!used_ty)
                continue;
            // - Run EAT on t and p
            t = context.m_resolve.expand_associated_types( sp, mv$(t) );
            // TODO: EAT on `p`
            DEBUG("Check " << t << " : " << bound.trait << p);
            DEBUG("- From " << bound.impl_ty << " : " << bound.trait << bound.params);

            // Search for any trait impl that could match this,
            bool bound_failed = true;
            context.m_resolve.find_trait_impls(sp, bound.trait, p, t, [&](const auto impl, auto cmp){
                // If this bound specifies an associated type, then check that that type could match
                if( bound.name != "" )
                {
                    auto aty = impl.get_type(bound.name.c_str());
                    // The associated type is not present, what does that mean?
                    if( aty == ::HIR::TypeRef() ) {
                        DEBUG("[check_ivar_poss__fails_bounds] No ATY for " << bound.name << " in impl");
                        // A possible match was found, so don't delete just yet
                        bound_failed = false;
                        // - Return false to keep searching
                        return false;
                    }
                    else if( aty.compare_with_placeholders(sp, bound.left_ty, context.m_ivars.callback_resolve_infer()) == HIR::Compare::Unequal ) {
                        DEBUG("[check_ivar_poss__fails_bounds] ATY " << context.m_ivars.fmt_type(aty) << " != left " << context.m_ivars.fmt_type(bound.left_ty));
                        bound_failed = true;
                        // - Bail instantly
                        return true;
                    }
                    else {
                    }
                }
                bound_failed = false;
                return true;
                });
            if( bound_failed && ! t.m_data.is_Infer() ) {
                // If none was found, remove from the possibility list
                DEBUG("Remove possibility " << new_ty << " because it failed a bound");
                return true;
            }

            // TODO: Check for the resultant associated type
        }

        // Handle methods
        for(const auto* node_ptr_dyn : context.to_visit)
        {
            if( const auto* node_ptr = dynamic_cast<const ::HIR::ExprNode_CallMethod*>(node_ptr_dyn) )
            {
                const auto& node = *node_ptr;
                const auto& ty_tpl = context.get_type(node.m_value->m_res_type);

                bool used_ty = false;
                auto t = clone_ty_with(sp, ty_tpl, [&](const auto& ty, auto& out_ty){ if( ty == ty_l ) { out_ty = new_ty.clone(); used_ty = true; return true; } else { return false; }});
                if(!used_ty)
                    continue;

                DEBUG("Check <" << t << ">::" << node.m_method);
                ::std::vector<::std::pair<TraitResolution::AutoderefBorrow, ::HIR::Path>> possible_methods;
                unsigned int deref_count = context.m_resolve.autoderef_find_method(node.span(), node.m_traits, node.m_trait_param_ivars, t, node.m_method.c_str(),  possible_methods);
                DEBUG("> deref_count = " << deref_count << ", " << possible_methods);
                if( !t.m_data.is_Infer() && possible_methods.empty() )
                {
                    // No method found, which would be an error
                    return true;
                }
            }
            else
            {
            }
        }

        return false;
    }

    enum class IvarPossFallbackType {
        None,   // No fallback, only make safe decisions
        Assume, // Picks an option, even if there's non source/destination types
        IgnoreWeakDisable,  // Ignores the weaker disable flags
        FinalOption,
    };
    ::std::ostream& operator<<(::std::ostream& os, IvarPossFallbackType t) {
        switch(t)
        {
        case IvarPossFallbackType::None:    os << "";   break;
        case IvarPossFallbackType::Assume:  os << " weak";   break;
        case IvarPossFallbackType::IgnoreWeakDisable:  os << " unblock";   break;
        case IvarPossFallbackType::FinalOption:  os << " final";   break;
        }
        return os;
    }
    /// Check IVar possibilities, from both coercion/unsizing (which have well-encoded rules) and from trait impls
    bool check_ivar_poss(Context& context, unsigned int i, Context::IVarPossible& ivar_ent, IvarPossFallbackType fallback_ty=IvarPossFallbackType::None)
    {
        static Span _span;
        const auto& sp = _span;
        const bool honour_disable = (fallback_ty != IvarPossFallbackType::IgnoreWeakDisable);

        if( ivar_ent.force_disable )
        {
            DEBUG(i << ": forced unknown");
            return false;
        }
        if( ivar_ent.force_no_to || ivar_ent.force_no_from )
        {
            switch(fallback_ty)
            {
            case IvarPossFallbackType::IgnoreWeakDisable:
            case IvarPossFallbackType::FinalOption:
                break;
            default:
                DEBUG(i << ": coercion blocked");
                return false;
            }
        }

        ::HIR::TypeRef  ty_l_ivar;
        ty_l_ivar.m_data.as_Infer().index = i;
        const auto& ty_l = context.m_ivars.get_type(ty_l_ivar);

        if( ty_l != ty_l_ivar ) {
            if( ivar_ent.has_rules() )
            {
                DEBUG("- IVar " << i << " had possibilities, but was known to be " << ty_l);
                // Completely clear by reinitialising
                ivar_ent = Context::IVarPossible();
            }
            else
            {
                //DEBUG(i << ": known " << ty_l);
            }
            return false;
        }

        // Don't attempt to guess literals
        // - What about if they're bounded?
        if( ty_l.m_data.as_Infer().is_lit() )
        {
            DEBUG(i << ": Literal " << ty_l);
            return false;
        }
        if( ! ivar_ent.has_rules() )
        {
            if( ty_l.m_data.as_Infer().ty_class == ::HIR::InferClass::Diverge )
            {
                DEBUG("Set IVar " << i << " = ! (no rules, and is diverge-class ivar)");
                context.m_ivars.get_type(ty_l_ivar) = ::HIR::TypeRef::new_diverge();
                context.m_ivars.mark_change();
                return true;
            }
            // No rules, don't do anything (and don't print)
            DEBUG(i << ": No rules");
            return false;
        }

        TRACE_FUNCTION_F(i << fallback_ty << " - " << ty_l);


        bool has_no_coerce_posiblities;

        // Fill a single list with all possibilities, and pick the most suitable type.
        // - This list needs to include flags to say if the type can be dereferenced.
        {
            // TODO: Move this to its own function.
            struct PossibleType {
                bool    is_pointer; // I.e. it's from a coerce
                bool    can_deref;  // I.e. from an unsize or coerce, AND it's a "from"
                const ::HIR::TypeRef* ty;

                bool operator<(const PossibleType& o) const {
                    if( *ty != *o.ty )
                        return *ty < *o.ty;
                    if( is_pointer != o.is_pointer )
                        return is_pointer < o.is_pointer;
                    if( can_deref < o.can_deref )
                        return can_deref < o.can_deref;
                    return false;
                }
                ::std::ostream& fmt(::std::ostream& os) const {
                    return os << (is_pointer ? "C" : "-") << (can_deref ? "D" : "-") << " " << *ty;
                }

                bool is_source() const { return this->can_deref; }
                bool is_dest() const { return !this->can_deref; }
                static bool is_source_s(const PossibleType& self) { return self.is_source(); }
                static bool is_dest_s(const PossibleType& self) { return self.is_dest(); }
            };

            bool allow_unsized = !(i < context.m_ivars_sized.size() ? context.m_ivars_sized.at(i) : false);

            ::std::vector<PossibleType> possible_tys;
            static ::HIR::TypeRef   null_placeholder;
            bool add_placeholders = (fallback_ty < IvarPossFallbackType::IgnoreWeakDisable);
            if( add_placeholders && ivar_ent.force_no_from )  // TODO: This can't happen, there's an early return above.
            {
                possible_tys.push_back(PossibleType { false, true, &null_placeholder });
            }
            for(const auto& new_ty : ivar_ent.types_coerce_from )
            {
                possible_tys.push_back(PossibleType { true , true, &new_ty });
            }
            for(const auto& new_ty : ivar_ent.types_unsize_from )
            {
                possible_tys.push_back(PossibleType { false, true, &new_ty });
            }
            if( add_placeholders && ivar_ent.force_no_to )    // TODO: This can't happen, there's an early return above.
            {
                possible_tys.push_back(PossibleType { false, false, &null_placeholder });
            }
            for(const auto& new_ty : ivar_ent.types_coerce_to )
            {
                possible_tys.push_back(PossibleType { true , false, &new_ty });
            }
            for(const auto& new_ty : ivar_ent.types_unsize_to )
            {
                possible_tys.push_back(PossibleType { false, false, &new_ty });
            }
            DEBUG("possible_tys = " << possible_tys);

            // If exactly the same type is both a source and destination, equate.
            // - This is always correct, even if one of the types is an ivar (you can't go A -> B -> A with a coercion)
            {
                for(const auto& ent : possible_tys)
                {
                    if( !ent.can_deref )
                        continue ;
                    for(const auto& ent2 : possible_tys)
                    {
                        if( &ent == &ent2 ) {
                            continue;
                        }
                        if( ent2.can_deref ) {
                            continue ;
                        }
                        if( *ent.ty != ::HIR::TypeRef() && *ent.ty == *ent2.ty ) {
                            DEBUG("- Source/Destination type");
                            context.equate_types(sp, ty_l, *ent.ty);
                            return true;
                        }
                        // TODO: Compare such that &[_; 1] == &[u8; 1]?
                    }
                }
            }

#if 1
            if( ::std::count_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_source_s) == 1 && !ivar_ent.force_no_from )
            {
                // Single source, pick it?
                const auto& ent = *::std::find_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_source_s);
                // - Only if there's no ivars
                if( !context.m_ivars.type_contains_ivars(*ent.ty) )
                {
                    if( !check_ivar_poss__fails_bounds(sp, context, ty_l, *ent.ty) )
                    {
                        DEBUG("Single concrete source, " << *ent.ty);
                        context.equate_types(sp, ty_l, *ent.ty);
                        return true;
                    }
                }
            }
#endif


            if( ty_l.m_data.as_Infer().ty_class == ::HIR::InferClass::Diverge )
            {
                // There's a coercion (not an unsizing) AND there's no sources
                // - This ensures that the ! is directly as a value, and not as a generic param or behind a pointer
                if( ::std::any_of(possible_tys.begin(), possible_tys.end(), [](const PossibleType& ent){ return ent.is_pointer; })
                 && ::std::none_of(possible_tys.begin(), possible_tys.end(), [](const PossibleType& ent){ return ent.can_deref; })
                 )
                {
                    if( !ivar_ent.force_no_to && ::std::count_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_dest_s) == 1 )
                    {
                        auto ent = *::std::find_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_dest_s);
                        DEBUG("One destination (diverge, no source), setting to " << *ent.ty);
                        context.equate_types(sp, ty_l, *ent.ty);
                        return true;
                    }

                    // There are no source possibilities, this has to be a `!`
                    DEBUG("- Diverge with no source types, force setting to !");
                    DEBUG("Set IVar " << i << " = !");
                    context.m_ivars.get_type(ty_l_ivar) = ::HIR::TypeRef::new_diverge();
                    context.m_ivars.mark_change();
                    return true;
                }
            }

            // If there's no disable flags set, and there's only one source, pick it.
            // - Slight hack to speed up flow-down inference
            if( possible_tys.size() == 1 && possible_tys[0].can_deref && !ivar_ent.force_no_from ) {
                DEBUG("One possibility (before ivar removal), setting to " << *possible_tys[0].ty);
                context.equate_types(sp, ty_l, *possible_tys[0].ty);
                return true;
            }
            //if( possible_tys.size() == 1 && !possible_tys[0].can_deref && !ivar_ent.force_no_to ) {
            //    DEBUG("One possibility (before ivar removal), setting to " << *possible_tys[0].ty);
            //    context.equate_types(sp, ty_l, *possible_tys[0].ty);
            //    return true;
            //}

            // TODO: This shouldn't just return, instead the above null placeholders should be tested
            if( ivar_ent.force_no_to || ivar_ent.force_no_from )
            {
                switch(fallback_ty)
                {
                case IvarPossFallbackType::IgnoreWeakDisable:
                case IvarPossFallbackType::FinalOption:
                    break;
                default:
                    DEBUG(i << ": coercion blocked");
                    return false;
                }
            }

            // TODO: Single destination, and all sources are coerce-able
            // - Pick the single destination
            #if 0
            if( !ivar_ent.force_no_to && ::std::count_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_dest_s) == 1 )
            {
                auto ent = *::std::find_if(possible_tys.begin(), possible_tys.end(), PossibleType::is_dest_s);
                DEBUG("One destination, setting to " << *ent.ty);
                context.equate_types(sp, ty_l, *ent.ty);
                return true;
            }
            #endif

            // Filter out ivars
            // - TODO: Should this also remove &_ types? (maybe not, as they give information about borrow classes)
            size_t n_ivars;
            size_t n_src_ivars;
            size_t n_dst_ivars;
            {
                n_src_ivars = 0;
                n_dst_ivars = 0;
                auto new_end = ::std::remove_if(possible_tys.begin(), possible_tys.end(), [&](const PossibleType& ent) {
                        // TODO: Should this remove Unbound associated types too?
                        if( ent.ty->m_data.is_Infer() )
                        {
                            if( ent.can_deref )
                            {
                                n_src_ivars += 1;
                            }
                            else
                            {
                                n_dst_ivars += 1;
                            }
                            return true;
                        }
                        else
                        {
                            return false;
                        }
                        });
                n_ivars = possible_tys.end() - new_end;
                possible_tys.erase(new_end, possible_tys.end());
            }
            DEBUG(n_ivars << " ivars (" << n_src_ivars << " src, " << n_dst_ivars << " dst)");
            (void)n_ivars;

            // === If there's no source ivars, find the least permissive source ===
            // - If this source can't be unsized (e.g. in `&_, &str`, `&str` is the least permissive, and can't be
            // coerced without a `*const _` in the list), then equate to that
            // 1. Find the most accepting pointer type (if there is at least one coercion source)
            // 2. Look for an option that uses that pointer type, and contains an unsized type (that isn't a trait
            //    object with markers)
            // 3. Assign to that known most-permissive option
            // TODO: Do the oposite for the destination types (least permissive pointer, pick any Sized type)
            if( n_src_ivars == 0 || fallback_ty == IvarPossFallbackType::Assume )
            {
                static const ::HIR::TypeData::Tag tag_ordering[] = {
                    //::HIR::TypeData::TAG_Generic,
                    ::HIR::TypeData::TAG_Path, // Strictly speaking, Path == Generic?
                    ::HIR::TypeData::TAG_Borrow,
                    ::HIR::TypeData::TAG_Pointer,
                    };
                struct H {
                    static Ordering ord_accepting_ptr(const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r)
                    {
                        // Ordering in increasing acceptiveness:
                        // - Paths, Borrows, Raw Pointers
                        if( ty_l.m_data.tag() != ty_r.m_data.tag() )
                        {
                            const auto* tag_ordering_end = tag_ordering+sizeof(tag_ordering)/sizeof(tag_ordering[0]);
                            auto it_l = ::std::find(tag_ordering, tag_ordering_end, ty_l.m_data.tag());
                            auto it_r = ::std::find(tag_ordering, tag_ordering_end, ty_r.m_data.tag());
                            if( it_l == tag_ordering_end || it_r == tag_ordering_end ) {
                                // Huh?
                                return OrdEqual;
                            }
                            if( it_l < it_r )
                                return OrdLess;
                            else if( it_l > it_r )
                                return OrdGreater;
                            else
                                throw "Impossible";
                        }

                        switch(ty_l.m_data.tag())
                        {
                        case ::HIR::TypeData::TAG_Borrow:
                            // Reverse order - Shared is numerically lower than Unique, but is MORE accepting
                            return ::ord( static_cast<int>(ty_r.m_data.as_Borrow().type), static_cast<int>(ty_l.m_data.as_Borrow().type) );
                        case ::HIR::TypeData::TAG_Pointer:
                            // Reverse order - Shared is numerically lower than Unique, but is MORE accepting
                            return ::ord( static_cast<int>(ty_r.m_data.as_Pointer().type), static_cast<int>(ty_l.m_data.as_Pointer().type) );
                        case ::HIR::TypeData::TAG_Path:
                            return OrdEqual;
                        case ::HIR::TypeData::TAG_Generic:
                            return OrdEqual;
                        default:
                            // Technically a bug/error
                            return OrdEqual;
                        }
                    }

                    static const ::HIR::TypeRef* match_and_extract_ptr_ty(const ::HIR::TypeRef& ptr_tpl, const ::HIR::TypeRef& ty)
                    {
                        if( ty.m_data.tag() != ptr_tpl.m_data.tag() )
                            return nullptr;
                        TU_MATCH_HDRA( (ty.m_data), { )
                        TU_ARMA(Borrow, te) {
                            if( te.type == ptr_tpl.m_data.as_Borrow().type ) {
                                return &*te.inner;
                            }
                            }
                        TU_ARMA(Pointer, te) {
                            if( te.type == ptr_tpl.m_data.as_Pointer().type ) {
                                return &*te.inner;
                            }
                            }
                        TU_ARMA(Path, te) {
                            if( te.binding == ptr_tpl.m_data.as_Path().binding ) {
                                // TODO: Get inner
                            }
                            } break;
                        default:
                            break;
                        }
                        return nullptr;
                    }
                };
                const ::HIR::TypeRef* ptr_ty = nullptr;
                if( ::std::any_of(possible_tys.begin(), possible_tys.end(), [&](const auto& ent){ return ent.can_deref && ent.is_pointer; }) )
                {
                    for(const auto& ent : possible_tys)
                    {
                        if( !ent.can_deref )
                            continue;

                        if( ptr_ty == nullptr )
                        {
                            ptr_ty = ent.ty;
                        }
                        else if( H::ord_accepting_ptr(*ent.ty, *ptr_ty) == OrdGreater )
                        {
                            ptr_ty = ent.ty;
                        }
                        else
                        {
                        }
                    }
                }

                for(const auto& ent : possible_tys)
                {
                    // Sources only
                    if( ! ent.can_deref )
                        continue ;
                    // Must match `ptr_ty`'s outer pointer
                    const ::HIR::TypeRef* inner_ty = (ptr_ty ? H::match_and_extract_ptr_ty(*ptr_ty, *ent.ty) : ent.ty);
                    if( !inner_ty )
                        continue;

                    bool is_max_accepting = false;
                    if( inner_ty->m_data.is_Slice() ) {
                        is_max_accepting = true;
                    }
                    else if( TU_TEST1(inner_ty->m_data, Primitive, == ::HIR::CoreType::Str) ) {
                        is_max_accepting = true;
                    }
                    else {
                    }
                    if( is_max_accepting )
                    {
                        DEBUG("Most accepting pointer class, and most permissive inner type - " << *ent.ty);
                        context.equate_types(sp, ty_l, *ent.ty);
                        return true;
                    }
                }
            }

            struct TypeRestrictiveOrdering {
                static Ordering get_ordering_infer(const Span& sp, const ::HIR::TypeRef& r)
                {
                    // For infer, only concrete types are more restrictive
                    TU_MATCH_HDRA( (r.m_data), { )
                    default:
                        return OrdLess;
                    TU_ARMA(Path, te) {
                        if( te.binding.is_Opaque() )
                            return OrdLess;
                        if( te.binding.is_Unbound() )
                            return OrdEqual;
                        // TODO: Check if the type is concrete? (Check an unsizing param if present)
                        return OrdLess;
                        }
                    TU_ARMA(Borrow, _)
                        return OrdEqual;
                    TU_ARMA(Infer, _)
                        return OrdEqual;
                    TU_ARMA(Pointer, _)
                        return OrdEqual;
                    }
                    throw "";
                }
                // Ordering of `l` relative to `r`, OrdLess means that the LHS is less restrictive
                static Ordering get_ordering_ty(const Span& sp, const Context& context, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r)
                {
                    if( l == r ) {
                        return OrdEqual;
                    }
                    if( l.m_data.is_Infer() ) {
                        return get_ordering_infer(sp, r);
                    }
                    if( r.m_data.is_Infer() ) {
                        switch( get_ordering_infer(sp, l) )
                        {
                        case OrdLess:   return OrdGreater;
                        case OrdEqual:  return OrdEqual;
                        case OrdGreater:return OrdLess;
                        }
                    }
                    if( l.m_data.is_Path() ) {
                        const auto& te_l = l.m_data.as_Path();
                        // Path types can be unsize targets, and can also act like infers
                        // - If it's a Unbound treat as Infer
                        // - If Opaque, then search for a CoerceUnsized/Unsize bound?
                        // - If Struct, look for ^ tag
                        // - Else, more/equal specific
                        TU_MATCH_HDRA( (r.m_data), { )
                        default:
                            // An ivar is less restrictive?
                            if( te_l.binding.is_Unbound() )
                                return OrdLess;
                            TODO(sp, l << " with " << r << " - LHS is Path, RHS is ?");
                        TU_ARMA(Path, te_r) {
                            if( te_l.binding.is_Unbound() && te_r.binding.is_Unbound() )
                            {
                                return OrdEqual;
                            }
                            if( te_l.binding.is_Unbound() )
                                return OrdLess;
                            if( te_r.binding.is_Unbound() )
                            {
                                return OrdGreater;
                            }
                            else if( te_r.binding.is_Opaque() )
                            {
                                TODO(sp, l << " with " << r << " - LHS is Path, RHS is opaque type");
                            }
                            else if( TU_TEST1(te_r.binding, Struct, ->m_struct_markings.can_unsize) )
                            {
                                TODO(sp, l << " with " << r << " - LHS is Path, RHS is unsize-capable struct");
                            }
                            else
                            {
                                return OrdEqual;
                            }
                            }
                        }
                    }
                    if( r.m_data.is_Path() ) {
                        // Path types can be unsize targets, and can also act like infers
                        switch( get_ordering_ty(sp, context, r, l) )
                        {
                        case OrdLess:   return OrdGreater;
                        case OrdEqual:  return OrdEqual;
                        case OrdGreater:return OrdLess;
                        }
                    }

                    // Slice < Array
                    if( l.m_data.tag() == r.m_data.tag() ) {
                        return OrdEqual;
                    }
                    else {
                        if( l.m_data.is_Slice() && r.m_data.is_Array() ) {
                            return OrdGreater;
                        }
                        if( l.m_data.is_Array() && r.m_data.is_Slice() ) {
                            return OrdLess;
                        }
                        TODO(sp, "Compare " << l << " and " << r);
                    }
                }

                // Returns the restrictiveness ordering of `l` relative to `r`
                // - &T is more restrictive than *const T
                // - &mut T is more restrictive than &T
                // Restrictive means that left can't be coerced from right
                static Ordering get_ordering_ptr(const Span& sp, const Context& context, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r)
                {
                    Ordering cmp;
                    TRACE_FUNCTION_FR(l << " , " << r, cmp);
                    // Get ordering of this type to the current destination
                    // - If lesser/greater then ignore/update
                    // - If equal then what? (Instant error? Leave as-is and let the asignment happen? Disable the asignment?)
                    static const ::HIR::TypeData::Tag tag_ordering[] = {
                        ::HIR::TypeData::TAG_Pointer,
                        ::HIR::TypeData::TAG_Borrow,
                        ::HIR::TypeData::TAG_Path, // Strictly speaking, Path == Generic
                        ::HIR::TypeData::TAG_Generic,
                        };
                    static const ::HIR::TypeData::Tag* tag_ordering_end = &tag_ordering[ sizeof(tag_ordering) / sizeof(tag_ordering[0] )];
                    if( l.m_data.tag() != r.m_data.tag() )
                    {
                        auto p_l = ::std::find(tag_ordering, tag_ordering_end, l.m_data.tag());
                        auto p_r = ::std::find(tag_ordering, tag_ordering_end, r.m_data.tag());
                        if( p_l == tag_ordering_end ) {
                            TODO(sp, "Type " << l << " not in ordering list");
                        }
                        if( p_r == tag_ordering_end ) {
                            TODO(sp, "Type " << r << " not in ordering list");
                        }
                        cmp = ord( static_cast<int>(p_l-p_r), 0 );
                    }
                    else
                    {
                        TU_MATCH_HDRA( (l.m_data), { )
                        default:
                            BUG(sp, "Unexpected type class " << l << " in get_ordering_ty");
                            break;
                        TU_ARMA(Generic, _te_l) {
                            cmp = OrdEqual;
                            }
                        TU_ARMA(Path, te_l) {
                            //const auto& te = dest_type->m_data.as_Path();
                            // TODO: Prevent this rule from applying?
                            return OrdEqual;
                            }
                        TU_ARMA(Borrow, te_l) {
                            const auto& te_r = r.m_data.as_Borrow();
                            cmp = ord( (int)te_l.type, (int)te_r.type );   // Unique>Shared in the listing, and Unique is more restrictive than Shared
                            if( cmp == OrdEqual )
                            {
                                cmp = get_ordering_ty(sp, context, context.m_ivars.get_type(*te_l.inner), context.m_ivars.get_type(*te_r.inner));
                            }
                            }
                        TU_ARMA(Pointer, te_l) {
                            const auto& te_r = r.m_data.as_Pointer();
                            cmp = ord( (int)te_r.type, (int)te_l.type );   // Note, reversed ordering because we want Unique>Shared
                            if( cmp == OrdEqual )
                            {
                                cmp = get_ordering_ty(sp, context, context.m_ivars.get_type(*te_l.inner), context.m_ivars.get_type(*te_r.inner));
                            }
                            }
                        }
                    }
                    return cmp;
                }
            };

            // If there's multiple source types (which means that this ivar has to be a coercion from one of them)
            // Look for the least permissive of the available destination types and assign to that
            #if 1
            // NOTE: This only works for coercions (not usizings), so is restricted to all options being pointers
            if( ::std::all_of(possible_tys.begin(), possible_tys.end(), [](const auto& ent){ return ent.is_pointer; })
                //||  ::std::none_of(possible_tys.begin(), possible_tys.end(), [](const auto& ent){ return ent.is_pointer; })
                )
            {
                // 1. Count distinct (and non-ivar) source types
                // - This also ignores &_ types
                size_t num_distinct = 0;
                for(const auto& ent : possible_tys)
                {
                    if( !ent.can_deref )
                        continue ;
                    // Ignore infer borrows
                    if( TU_TEST1(ent.ty->m_data, Borrow, .inner->m_data.is_Infer()) )
                        continue;
                    bool is_duplicate = false;
                    for(const auto& ent2 : possible_tys)
                    {
                        if( &ent2 == &ent )
                            break;
                        if( !ent.can_deref )
                            continue ;
                        if( *ent.ty == *ent2.ty ) {
                            is_duplicate = true;
                            break;
                        }
                        // TODO: Compare such that &[_; 1] == &[u8; 1]?
                    }
                    if( !is_duplicate )
                    {
                        num_distinct += 1;
                    }
                }
                DEBUG("- " << num_distinct << " distinct possibilities");
                // 2. Find the most restrictive destination type
                // - Borrows are more restrictive than pointers
                // - Borrows of Sized types are more restrictive than any other
                // - Decreasing borrow type ordering: Owned, Unique, Shared
                const ::HIR::TypeRef*   dest_type = nullptr;
                for(const auto& ent : possible_tys)
                {
                    if( ent.can_deref )
                        continue ;
                    // Ignore &_ types?
                    // - No, need to handle them below
                    if( !dest_type ) {
                        dest_type = ent.ty;
                        continue ;
                    }

                    auto cmp = TypeRestrictiveOrdering::get_ordering_ptr(sp, context, *ent.ty, *dest_type);
                    switch(cmp)
                    {
                    case OrdLess:
                        // This entry is less restrictive, so DO update `dest_type`
                        dest_type = ent.ty;
                        break;
                    case OrdEqual:
                        break;
                    case OrdGreater:
                        // This entry is more restrictive, so don't update `dest_type`
                        break;
                    }
                }
                // TODO: Unsized types? Don't pick an unsized if coercions are present?
                // TODO: If in a fallback mode, then don't require >1 (just require dest_type)
                if( (num_distinct > 1 || fallback_ty == IvarPossFallbackType::Assume) && dest_type )
                {
                    DEBUG("- Most-restrictive destination " << *dest_type);
                    context.equate_types(sp, ty_l, *dest_type);
                    return true;
                }
            }
            #endif

            // TODO: Remove any types that are covered by another type
            // - E.g. &[T] and &[U] can be considered equal, because [T] can't unsize again
            // - Comparison function: Returns one of Incomparible,Less,Same,More - Representing the amount of type information present.
            {
                struct InfoOrdering
                {
                    enum Ordering {
                        Incompatible,   // The types are incompatible
                        Less,   // The LHS type provides less information (e.g. has more ivars)
                        Same,   // Same number of ivars
                        More,   // The RHS provides more information (less ivars)
                    };
                    static bool is_infer(const ::HIR::TypeRef& ty) {
                        if( ty.m_data.is_Infer() )
                            return true;
                        if( TU_TEST1(ty.m_data, Path, .binding.is_Unbound()) )
                            return true;
                        return false;
                    }
                    static Ordering compare(const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r) {
                        if( is_infer(ty_l) ) {
                            if( is_infer(ty_r) )
                                return Same;
                            return Less;
                        }
                        else {
                            if( is_infer(ty_r) )
                                return More;
                        }
                        if( ty_l.m_data.tag() != ty_r.m_data.tag() ) {
                            return Incompatible;
                        }
                        TU_MATCH_HDRA( (ty_l.m_data, ty_r.m_data), {)
                        default:
                            return Incompatible;
                        TU_ARMA(Tuple, le, re) {
                            if( le.size() != re.size() )
                                return Incompatible;
                            int score = 0;
                            for(size_t i = 0; i < le.size(); i ++)
                            {
                                switch(compare(le[i], re[i]))
                                {
                                case Incompatible:
                                    return Incompatible;
                                case Less:  score --;   break;
                                case Same:              break;
                                case More:  score ++;   break;
                                }
                            }
                            if( score < 0 )
                                return Less;
                            else if( score > 0 )
                                return More;
                            else
                                return Same;
                            }
                        }
                    }
                    static Ordering compare_top(const Context& context, const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r, bool should_deref) {
                        if( context.m_ivars.types_equal(ty_l, ty_r) )
                            return Same;
                        if( is_infer(ty_l) )
                            return Incompatible;
                        if( is_infer(ty_r) )
                            return Incompatible;
                        if( ty_l.m_data.tag() != ty_r.m_data.tag() ) {
                            return Incompatible;
                        }
                        if( should_deref ) {
                            if( const auto* le = ty_l.m_data.opt_Borrow() ) {
                                const auto& re = ty_r.m_data.as_Borrow();
                                if( le->type != re.type )
                                    return Incompatible;
                                return compare_top(context, context.m_ivars.get_type(*le->inner), context.m_ivars.get_type(*re.inner), false);
                            }
                            else if( const auto* le = ty_l.m_data.opt_Pointer() ) {
                                const auto& re = ty_r.m_data.as_Pointer();
                                if( le->type != re.type )
                                    return Incompatible;
                                return compare_top(context, context.m_ivars.get_type(*le->inner), context.m_ivars.get_type(*re.inner), false);
                            }
                            else if( TU_TEST2(ty_l.m_data, Path, .binding, Struct, ->m_struct_markings.coerce_unsized != ::HIR::StructMarkings::Coerce::None) )
                            {
                                const auto& le = ty_l.m_data.as_Path();
                                const auto& re = ty_l.m_data.as_Path();
                                if( le.binding != re.binding )
                                    return Incompatible;
                                auto param_idx = le.binding.as_Struct()->m_struct_markings.coerce_param;
                                assert(param_idx != ~0u);
                                return compare_top(context,
                                        context.m_ivars.get_type(le.path.m_data.as_Generic().m_params.m_types.at(param_idx)),
                                        context.m_ivars.get_type(re.path.m_data.as_Generic().m_params.m_types.at(param_idx)),
                                        false
                                        );
                            }
                            else
                            {
                                BUG(Span(), "Can't deref " << ty_l << " / " << ty_r);
                            }
                        }
                        if( ty_l.m_data.is_Slice() )
                        {
                            const auto& le = ty_l.m_data.as_Slice();
                            const auto& re = ty_r.m_data.as_Slice();

                            switch(compare(context.m_ivars.get_type(*le.inner), context.m_ivars.get_type(*re.inner)))
                            {
                            case Less:  return Less;
                            case More:  return More;
                            case Same:
                            case Incompatible:
                                return Same;
                            }
                            throw "";
                        }
                        return Incompatible;
                    }
                    static const ::HIR::TypeRef& get_pointer_inner(const Context& context, const ::HIR::TypeRef& t_raw) {
                        const auto& t = context.m_ivars.get_type(t_raw);
                        if( const auto* te = t.m_data.opt_Borrow() ) {
                            return context.m_ivars.get_type(*te->inner);
                        }
                        else if( const auto* te = t.m_data.opt_Pointer() ) {
                            return context.m_ivars.get_type(*te->inner);
                        }
                        else if( TU_TEST2(t.m_data, Path, .binding, Struct, ->m_struct_markings.coerce_unsized != ::HIR::StructMarkings::Coerce::None) )
                        {
                            const auto& te = t.m_data.as_Path();
                            auto param_idx = te.binding.as_Struct()->m_struct_markings.coerce_param;
                            assert(param_idx != ~0u);
                            const auto& path = te.path.m_data.as_Generic();
                            return context.m_ivars.get_type(path.m_params.m_types.at(param_idx));
                        }
                        else {
                            throw "";
                            //return t;
                        }
                    }
                };

                // De-duplicate destinations and sources separately
                for(auto it = possible_tys.begin(); it != possible_tys.end(); ++it)
                {
                    if( !it->ty )
                        continue;
                    for(auto it2 = it + 1; it2 != possible_tys.end(); ++it2)
                    {
                        if( !it2->ty )
                            continue;
                        if(it->can_deref != it2->can_deref) {
                            continue;
                        }
                        if(it->is_pointer != it2->is_pointer) {
                            continue;
                        }

                        switch(InfoOrdering::compare_top(context, *it->ty, *it2->ty, /*should_deref=*/it->is_pointer))
                        {
                        case InfoOrdering::Incompatible:
                            break;
                        case InfoOrdering::Less:
                        case InfoOrdering::Same:
                            DEBUG("Remove " << *it << ", keep " << *it2);
                            it->ty = it2->ty;
                            it->is_pointer = it2->is_pointer;
                            it2->ty = nullptr;
                            break;
                        case InfoOrdering::More:
                            DEBUG("Keep " << *it << ", remove " << *it2);
                            it2->ty = nullptr;
                            break;
                        }
                    }
                }
                auto new_end = ::std::remove_if(possible_tys.begin(), possible_tys.end(), [](const auto& e){ return e.ty == nullptr; });
                DEBUG("Removing " << (possible_tys.end() - new_end) << " redundant possibilities");
                possible_tys.erase(new_end, possible_tys.end());
            }

            // TODO: If in fallback mode, pick the most permissive option
            // - E.g. If the options are &mut T and *const T, use the *const T
            if( fallback_ty == IvarPossFallbackType::Assume )
            {
                // All are coercions (not unsizings)
                if( ::std::all_of(possible_tys.begin(), possible_tys.end(), [](const auto& ent){ return ent.is_pointer; }) && n_ivars == 0 )
                {
                    // Find the least restrictive destination, and most restrictive source
                    const ::HIR::TypeRef*   dest_type = nullptr;
                    bool any_ivar_present = false;
                    for(const auto& ent : possible_tys)
                    {
                        if( visit_ty_with(*ent.ty, [](const ::HIR::TypeRef& t){ return t.m_data.is_Infer(); }) ) {
                            any_ivar_present = true;
                        }
                        if( !dest_type ) {
                            dest_type = ent.ty;
                            continue ;
                        }

                        auto cmp = TypeRestrictiveOrdering::get_ordering_ptr(sp, context, *ent.ty, *dest_type);
                        switch(cmp)
                        {
                        case OrdLess:
                            // This entry is less restrictive, so DO update `dest_type`
                            dest_type = ent.ty;
                            break;
                        case OrdEqual:
                            break;
                        case OrdGreater:
                            // This entry is more restrictive, so don't update `dest_type`
                            break;
                        }
                    }

                    if( dest_type && n_ivars == 0 && any_ivar_present == false )
                    {
                        DEBUG("Suitable option " << *dest_type << " from " << possible_tys);
                        context.equate_types(sp, ty_l, *dest_type);
                        return true;
                    }
                }
            }

            DEBUG("possible_tys = " << possible_tys);
            DEBUG("- Bounded [" << ivar_ent.bounded << "]");
#if 1
            if( !possible_tys.empty() )
            {
                for(const auto& new_ty : ivar_ent.bounded)
                {
                    bool failed_a_bound = false;
                    // Check if this bounded type _cannot_ work with any of the existing bounds
                    // - Don't add to the possiblity list if so
                    for(const auto& opt : possible_tys)
                    {
                        CoerceResult    res;
                        if( opt.can_deref ) {
                            DEBUG(" > " << new_ty << " =? " << *opt.ty);
                            res = check_unsize_tys(context, sp, new_ty, *opt.ty, nullptr, false);
                        }
                        else {
                            // Destination type, this option must deref to it
                            DEBUG(" > " << *opt.ty << " =? " << new_ty);
                            res = check_unsize_tys(context, sp, *opt.ty, new_ty, nullptr, false);
                        }
                        DEBUG(" = " << res);
                        if( res == CoerceResult::Equality ) {
                            failed_a_bound = true;
                            break;
                        }
                        else if( res == CoerceResult::Unknown ) {
                            // Should this also be treated as a fail?
                        }
                    }
                    if( !failed_a_bound )
                    {
                        // TODO: Don't add ivars?
                        if( new_ty == ty_l )
                        {
                        }
                        else if( new_ty.m_data.is_Infer() )
                        {
                            n_ivars += 1;
                        }
                        else
                        {
                            possible_tys.push_back(PossibleType { false, false, &new_ty });
                        }
                    }
                }
            }
#endif
            DEBUG("possible_tys = " << possible_tys);
            // Filter out useless options and impossiblities
            for(auto it = possible_tys.begin(); it != possible_tys.end(); )
            {
                bool remove_option = false;
                if( *it->ty == ty_l )
                {
                    remove_option = true;
                }
                else if( !allow_unsized &&  context.m_resolve.type_is_sized(sp, *it->ty) == ::HIR::Compare::Unequal )
                {
                    remove_option = true;
                }
                else
                {
                    // Keep
                }

                // TODO: Ivars have been removed, this sort of check should be moved elsewhere.
                if( !remove_option && ty_l.m_data.as_Infer().ty_class == ::HIR::InferClass::Integer )
                {
                    if( const auto* te = it->ty->m_data.opt_Primitive() ) {
                        (void)te;
                    }
                    else if( const auto* te = it->ty->m_data.opt_Path() ) {
                        // If not Unbound, remove option
                        (void)te;
                    }
                    else if( const auto* te = it->ty->m_data.opt_Infer() ) {
                        (void)te;
                    }
                    else {
                        remove_option = true;
                    }
                }

                it = (remove_option ? possible_tys.erase(it) : it + 1);
            }
            DEBUG("possible_tys = " << possible_tys);
            for(auto it = possible_tys.begin(); it != possible_tys.end(); )
            {
                bool remove_option = false;
                for(const auto& other_opt : possible_tys)
                {
                    if( &other_opt == &*it )
                        continue ;
                    if( *other_opt.ty == *it->ty )
                    {
                        // Potential duplicate
                        // - If the flag set is the same, then it is a duplicate
                        if( other_opt.can_deref == it->can_deref && other_opt.is_pointer == it->is_pointer ) {
                            remove_option = true;
                            break;
                        }
                        // If not an ivar, AND both are either unsize/pointer AND the deref flags are different
                        // TODO: Ivars have been removed?
                        if( !it->ty->m_data.is_Infer() && other_opt.is_pointer == it->is_pointer && other_opt.can_deref != it->can_deref )
                        {
                            // TODO: Possible duplicate with a check above...
                            DEBUG("Source and destination possibility, picking " << *it->ty);
                            context.equate_types(sp, ty_l, *it->ty);
                            return true;
                        }
                        // - Otherwise, we want to keep the option which doesn't allow dereferencing (remove current
                        // if it's the deref option)
                        if( it->can_deref && other_opt.is_pointer == it->is_pointer ) {
                            remove_option = true;
                            break;
                        }
                    }
                }
                it = (remove_option ? possible_tys.erase(it) : it + 1);
            }
            DEBUG("possible_tys = " << possible_tys);
            // Remove any options that are filled by other options (e.g. `str` and a derferencable String)
            for(auto it = possible_tys.begin(); it != possible_tys.end(); )
            {
                bool remove_option = false;
                if( it->can_deref && !it->ty->m_data.is_Infer() )
                {
                    DEBUG("> " << *it);
                    // Dereference once before starting the search
                    ::HIR::TypeRef  tmp, tmp2;
                    const auto* dty = it->ty;
                    auto src_bty = ::HIR::BorrowType::Shared;
                    if(it->is_pointer)
                    {
                        if( dty->m_data.is_Borrow() )
                            src_bty = dty->m_data.as_Borrow().type;
                        dty = context.m_resolve.autoderef(sp, *dty, tmp);
                        // NOTE: Coercions can also do closure->fn, so deref isn't always possible
                        //ASSERT_BUG(sp, dty, "Pointer (coercion source) that can't dereference - " << *it->ty);
                    }
                    //while( dty && !remove_option && (dty = context.m_resolve.autoderef(sp, *dty, tmp)) )
                    if( dty )
                    {
                        for(const auto& other_opt : possible_tys)
                        {
                            if( &other_opt == &*it )
                                continue ;
                            if( other_opt.ty->m_data.is_Infer() )
                                continue ;
                            DEBUG(" > " << other_opt);

                            const auto* oty = other_opt.ty;
                            auto o_bty = ::HIR::BorrowType::Owned;
                            if(other_opt.is_pointer)
                            {
                                if( oty->m_data.is_Borrow() )
                                    o_bty = oty->m_data.as_Borrow().type;
                                oty = context.m_resolve.autoderef(sp, *oty, tmp2);
                                //ASSERT_BUG(sp, oty, "Pointer (coercion src/dst) that can't dereference - " << *other_opt.ty);
                            }
                            if( o_bty > src_bty )   // Smaller means less powerful. Converting & -> &mut is invalid
                            {
                                // Borrow types aren't compatible
                                DEBUG("BT " << o_bty << " > " << src_bty);
                                break;
                            }
                            // TODO: Check if unsize is possible from `dty` to `oty`
                            if( oty )
                            {
                                DEBUG(" > " << *dty << " =? " << *oty);
                                auto cmp = check_unsize_tys(context, sp, *oty, *dty, nullptr, false);
                                DEBUG(" = " << cmp);
                                if( cmp == CoerceResult::Equality )
                                {
                                    //TODO(sp, "Impossibility for " << *oty << " := " << *dty);
                                }
                                else if( cmp == CoerceResult::Unknown )
                                {
                                }
                                else
                                {
                                    remove_option = true;
                                    DEBUG("- Remove " << *it << ", can deref to " << other_opt);
                                    break;
                                }
                            }
                        }
                    }
                }
                if( !remove_option && !it->ty->m_data.is_Infer() && check_ivar_poss__fails_bounds(sp, context, ty_l, *it->ty) )
                {
                    remove_option = true;
                    DEBUG("- Remove " << *it << " due to bounds");
                }
                it = (remove_option ? possible_tys.erase(it) : it + 1);
            }
            DEBUG("possible_tys = " << possible_tys << " (" << n_src_ivars << " src ivars, " << n_dst_ivars << " dst ivars)");

            // Find a CD option that can deref to a `--` option
            for(const auto& e : possible_tys)
            {
                if( e.is_pointer && e.can_deref )
                {
                    ::HIR::TypeRef  tmp;
                    const auto* dty = context.m_resolve.autoderef(sp, *e.ty, tmp);
                    if( dty && !dty->m_data.is_Infer() )
                    {
                        for(const auto& e2 : possible_tys)
                        {
                            if( !e2.is_pointer && !e2.can_deref )
                            {
                                if( context.m_ivars.types_equal(*dty, *e2.ty) )
                                {
                                    DEBUG("Coerce source can deref once to an unsize destination, picking source " << *e.ty);
                                    context.equate_types(sp, ty_l, *e.ty);
                                    return true;
                                }
                            }
                        }
                    }
                }
            }

            // If there's only one option (or one real option w/ ivars, if in fallback mode) - equate it
            //if( possible_tys.size() == 1 && (n_ivars == 0 || !honour_disable) )
            if( possible_tys.size() == 1 && n_ivars == 0 )
            {
                const auto& new_ty = *possible_tys[0].ty;
                DEBUG("Only " << new_ty << " is an option");
                context.equate_types(sp, ty_l, new_ty);
                return true;
            }
            // If there's only one non-deref in the list OR there's only one deref in the list
            if( !honour_disable && n_src_ivars == 0 && ::std::count_if(possible_tys.begin(), possible_tys.end(), [](const PossibleType& pt){ return pt.can_deref; }) == 1 )
            {
                auto it = ::std::find_if(possible_tys.begin(), possible_tys.end(), [](const PossibleType& pt){ return pt.can_deref; });
                const auto& new_ty = *it->ty;
                DEBUG("Picking " << new_ty << " as the only source [" << possible_tys << "]");
                context.equate_types(sp, ty_l, new_ty);
                return true;
            }
            if( !honour_disable && n_dst_ivars == 0 && ::std::count_if(possible_tys.begin(), possible_tys.end(), [](const PossibleType& pt){ return !pt.can_deref; }) == 1 )
            {
                auto it = ::std::find_if(possible_tys.begin(), possible_tys.end(), [](const PossibleType& pt){ return !pt.can_deref; });
                const auto& new_ty = *it->ty;
                if( it->is_pointer )
                {
                    DEBUG("Picking " << new_ty << " as the only target [" << possible_tys << "]");
                    context.equate_types(sp, ty_l, new_ty);
                    return true;
                }
                else
                {
                    // HACK: Work around failure in librustc
                    DEBUG("Would pick " << new_ty << " as the only target, but it's an unsize");
                }
            }
            // If there's multiple possiblilties, we're in fallback mode, AND there's no ivars in the list
            if( possible_tys.size() > 0 && !honour_disable && n_ivars == 0 )
            {
                //::std::sort(possible_tys.begin(), possible_tys.end());  // Sorts ivars to the front
                const auto& new_ty = *possible_tys.back().ty;
                DEBUG("Picking " << new_ty << " as an arbitary an option from [" << possible_tys << "]");
                context.equate_types(sp, ty_l, new_ty);
                return true;
            }

            // If there's no ivars, and no instances of &_ or Box<_>, then error/bug here.
#if 0
            if( possible_tys.size() > 0 )
            {
                struct H {
                    static const ::HIR::TypeRef& get_pointer_inner(const Context& context, const ::HIR::TypeRef& t_raw) {
                        const auto& t = context.m_ivars.get_type(t_raw);
                        if( const auto* te = t.m_data.opt_Borrow() ) {
                            return get_pointer_inner(context, *te->inner);
                        }
                        else if( const auto* te = t.m_data.opt_Pointer() ) {
                            return get_pointer_inner(context, *te->inner);
                        }
                        else if( TU_TEST2(t.m_data, Path, .binding, Struct, ->m_struct_markings.coerce_unsized != ::HIR::StructMarkings::Coerce::None) )
                        {
                            const auto& te = t.m_data.as_Path();
                            auto param_idx = te.binding.as_Struct()->m_struct_markings.coerce_param;
                            assert(param_idx != ~0u);
                            const auto& path = te.path.m_data.as_Generic();
                            return get_pointer_inner(context, path.m_params.m_types.at(param_idx));
                        }
                        else {
                            return t;
                        }
                    }
                };
                bool has_all_info = true;
                if( n_ivars > 0 )
                {
                    has_all_info = false;
                }
                for(const auto& e : possible_tys)
                {
                    const auto& inner = H::get_pointer_inner(context, *e.ty);
                    DEBUG(e << ", inner=" << inner);
                    if( inner.m_data.is_Infer() )
                    {
                        has_all_info = false;
                    }
                    if( TU_TEST1(inner.m_data, Path, .binding.is_Unbound()) )
                    {
                        has_all_info = false;
                    }
                }
                if( has_all_info )
                {
                    BUG(sp, "Sufficient information for " << ty_l << " but didn't pick a type - options are [" << possible_tys << "]");
                }
            }
#endif

            // If only one bound meets the possible set, use it
            if( ! possible_tys.empty() )
            {
                DEBUG("Checking bounded [" << ivar_ent.bounded << "]");
                ::std::vector<const ::HIR::TypeRef*>    feasable_bounds;
                for(const auto& new_ty : ivar_ent.bounded)
                {
                    bool failed_a_bound = false;
                    // TODO: Check if this bounded type _cannot_ work with any of the existing bounds
                    // - Don't add to the possiblity list if so
                    for(const auto& opt : possible_tys)
                    {
                        if( opt.can_deref ) {
                            const auto* dty = opt.ty;

                            DEBUG(" > " << new_ty << " =? " << *dty);
                            auto cmp = check_unsize_tys(context, sp, new_ty, *dty, nullptr, false);
                            DEBUG(" = " << cmp);
                            if( cmp == CoerceResult::Equality ) {
                                failed_a_bound = true;
                                break;
                            }
                        }
                        else {
                            // Destination type, this option must deref to it
                            DEBUG(" > " << *opt.ty << " =? " << new_ty);
                            auto cmp = check_unsize_tys(context, sp, *opt.ty, new_ty, nullptr, false);
                            DEBUG(" = " << cmp);
                            if( cmp == CoerceResult::Equality ) {
                                failed_a_bound = true;
                                break;
                            }
                        }
                    }
                    // TODO: Should this also check check_ivar_poss__fails_bounds
                    if( !failed_a_bound )
                    {
                        feasable_bounds.push_back(&new_ty);
                    }
                }
                if( feasable_bounds.size() == 1 )
                {
                    const auto& new_ty = *feasable_bounds.front();
                    DEBUG("Picking " << new_ty << " as it's the only bound that fits coercions");
                    context.equate_types(sp, ty_l, new_ty);
                    return true;
                }
            }
            else
            {
                // Not checking bounded list, because there's nothing to check
            }

            has_no_coerce_posiblities = possible_tys.empty() && n_ivars == 0;
        }

        if( has_no_coerce_posiblities && !ivar_ent.bounded.empty() )
        {
            // TODO: Search know possibilties and check if they satisfy the bounds for this ivar
            DEBUG("Options: " << ivar_ent.bounded);
            unsigned int n_good_ints = 0;
            ::std::vector<const ::HIR::TypeRef*>    good_types;
            good_types.reserve(ivar_ent.bounded.size());
            for(const auto& new_ty : ivar_ent.bounded)
            {
                DEBUG("- Test " << new_ty << " against current rules");
                if( check_ivar_poss__fails_bounds(sp, context, ty_l, new_ty) )
                {
                }
                else
                {
                    good_types.push_back(&new_ty);

                    if( new_ty.m_data.is_Primitive() )
                        n_good_ints ++;

                    DEBUG("> " << new_ty << " feasible");
                }
            }
            DEBUG(good_types.size() << " valid options (" << n_good_ints << " primitives)");
            // Picks the first if in fallback mode (which is signalled by `honour_disable` being false)
            // - This handles the case where there's multiple valid options (needed for libcompiler_builtins)
            // TODO: Only pick any if all options are the same class (or just all are integers)
            if( good_types.empty() )
            {

            }
            else if( good_types.size() == 1 )
            {
                // Since it's the only possibility, choose it?
                DEBUG("Only " << *good_types.front() << " fits current bound sets");
                context.equate_types(sp, ty_l, *good_types.front());
                return true;
            }
            else if( good_types.size() > 0 && fallback_ty == IvarPossFallbackType::FinalOption )
            {
                auto typ_is_borrow = [&](const ::HIR::TypeRef* typ) { return typ->m_data.is_Borrow(); };
                // NOTE: We want to select from sets of primitives and generics (which can be interchangable)
                if( ::std::all_of(good_types.begin(), good_types.end(), typ_is_borrow) == ::std::any_of(good_types.begin(), good_types.end(), typ_is_borrow) )
                {
                    DEBUG("Picking " << *good_types.front() << " as first of " << good_types.size() << " options [" << FMT_CB(ss, for(auto e:good_types) ss << *e << ",";) << "]");
                    context.equate_types(sp, ty_l, *good_types.front());
                    return true;
                }
                else
                {
                    // Mix of borrows with non-borrows
                }
            }
        }

        return false;
    }
}



void Typecheck_Code_CS(const typeck::ModuleState& ms, t_args& args, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr)
{
    TRACE_FUNCTION;

    auto root_ptr = expr.into_unique();
    assert(!ms.m_mod_paths.empty());
    Context context { ms.m_crate, ms.m_impl_generics, ms.m_item_generics, ms.m_mod_paths.back() };

    for( auto& arg : args ) {
        context.handle_pattern( Span(), arg.first, arg.second );
    }

    // - Build up ruleset from node tree
    {
        const Span& sp = root_ptr->span();
        // If the result type contans an erased type, replace that with a new ivar and emit trait bounds for it.
        ::HIR::TypeRef  new_res_ty = clone_ty_with(sp, result_type, [&](const auto& tpl, auto& rv) {
            if( tpl.m_data.is_ErasedType() )
            {
                const auto& e = tpl.m_data.as_ErasedType();
                rv = context.m_ivars.new_ivar_tr();
                expr.m_erased_types.push_back( rv.clone() );
                for(const auto& trait : e.m_traits)
                {
                    if( trait.m_type_bounds.size() == 0 )
                    {
                        context.equate_types_assoc(sp, ::HIR::TypeRef(), trait.m_path.m_path, trait.m_path.m_params.clone(), rv, "", false);
                    }
                    else
                    {
                        for(const auto& aty : trait.m_type_bounds)
                        {
                            context.equate_types_assoc(sp, aty.second, trait.m_path.m_path, trait.m_path.m_params.clone(), rv, aty.first.c_str(), false);
                        }
                    }
                }
                return true;
            }
            return false;
            });

        if(true)
        {
            ExprVisitor_AddIvars    visitor(context);
            context.add_ivars(root_ptr->m_res_type);
            root_ptr->visit(visitor);
        }

        ExprVisitor_Enum    visitor(context, ms.m_traits, result_type);
        context.add_ivars(root_ptr->m_res_type);
        root_ptr->visit(visitor);

        DEBUG("Return type = " << new_res_ty << ", root_ptr = " << typeid(*root_ptr).name() << " " << root_ptr->m_res_type);
        context.equate_types_coerce(sp, new_res_ty, root_ptr);
    }

    const unsigned int MAX_ITERATIONS = 1000;
    unsigned int count = 0;
    while( context.take_changed() /*&& context.has_rules()*/ && count < MAX_ITERATIONS )
    {
        TRACE_FUNCTION_F("=== PASS " << count << " ===");
        context.dump();

        // 1. Check coercions for ones that cannot coerce due to RHS type (e.g. `str` which doesn't coerce to anything)
        // 2. (???) Locate coercions that cannot coerce (due to being the only way to know a type)
        // - Keep a list in the ivar of what types that ivar could be equated to.
        DEBUG("--- Coercion checking");
        for(size_t i = 0; i < context.link_coerce.size(); )
        {
            auto ent = mv$(context.link_coerce[i]);
            const auto& span = (*ent->right_node_ptr)->span();
            auto& src_ty = (*ent->right_node_ptr)->m_res_type;
            //src_ty = context.m_resolve.expand_associated_types( span, mv$(src_ty) );
            ent->left_ty = context.m_resolve.expand_associated_types( span, mv$(ent->left_ty) );
            if( check_coerce(context, *ent) )
            {
                DEBUG("- Consumed coercion R" << ent->rule_idx << " " << ent->left_ty << " := " << src_ty);

#if 0
                // If this isn't the last item in the list
                if( i != context.link_coerce.size() - 1 )
                {
                    // Swap with the last item
                    context.link_coerce[i] = mv$(context.link_coerce.back());
                }
                // Remove the last item.
                context.link_coerce.pop_back();
#else
                context.link_coerce.erase( context.link_coerce.begin() + i );
#endif
            }
            else
            {
                context.link_coerce[i] = mv$(ent);
                ++ i;
            }
        }
        // 3. Check associated type rules
        DEBUG("--- Associated types");
        unsigned int link_assoc_iter_limit = context.link_assoc.size() * 4;
        for(unsigned int i = 0; i < context.link_assoc.size(); ) {
            // - Move out (and back in later) to avoid holding a bad pointer if the list is updated
            auto rule = mv$(context.link_assoc[i]);

            DEBUG("- " << rule);
            for( auto& ty : rule.params.m_types ) {
                ty = context.m_resolve.expand_associated_types(rule.span, mv$(ty));
            }
            if( rule.name != "" ) {
                rule.left_ty = context.m_resolve.expand_associated_types(rule.span, mv$(rule.left_ty));
                // HACK: If the left type is `!`, remove the type bound
                //if( rule.left_ty.m_data.is_Diverge() ) {
                //    rule.name = "";
                //}
            }
            rule.impl_ty = context.m_resolve.expand_associated_types(rule.span, mv$(rule.impl_ty));

            if( check_associated(context, rule) ) {
                DEBUG("- Consumed associated type rule " << i << "/" << context.link_assoc.size() << " - " << rule);
                if( i != context.link_assoc.size()-1 )
                {
                    //assert( context.link_assoc[i] != context.link_assoc.back() );
                    context.link_assoc[i] = mv$( context.link_assoc.back() );
                }
                context.link_assoc.pop_back();
            }
            else {
                context.link_assoc[i] = mv$(rule);
                i ++;
            }

            if( link_assoc_iter_limit -- == 0 )
            {
                DEBUG("link_assoc iteration limit exceeded");
                break;
            }
        }
        // 4. Revisit nodes that require revisiting
        DEBUG("--- Node revisits");
        for( auto it = context.to_visit.begin(); it != context.to_visit.end(); )
        {
            ::HIR::ExprNode& node = **it;
            ExprVisitor_Revisit visitor { context };
            DEBUG("> " << &node << " " << typeid(node).name() << " -> " << context.m_ivars.fmt_type(node.m_res_type));
            node.visit( visitor );
            //  - If the node is completed, remove it
            if( visitor.node_completed() ) {
                DEBUG("- Completed " << &node << " - " << typeid(node).name());
                it = context.to_visit.erase(it);
            }
            else {
                ++ it;
            }
        }
        {
            ::std::vector<bool> adv_revisit_remove_list;
            size_t  len = context.adv_revisits.size();
            for(size_t i = 0; i < len; i ++)
            {
                auto& ent = *context.adv_revisits[i];
                adv_revisit_remove_list.push_back( ent.revisit(context, /*is_fallback=*/false) );
            }
            for(size_t i = len; i --;)
            {
                if( adv_revisit_remove_list[i] ) {
                    context.adv_revisits.erase( context.adv_revisits.begin() + i );
                }
            }
        }

        // If nothing changed this pass, apply ivar possibilities
        // - This essentially forces coercions not to happen.
        if( ! context.m_ivars.peek_changed() )
        {
            // Check the possible equations
            DEBUG("--- IVar possibilities");
            // TODO: De-duplicate this with the block ~80 lines below
            //for(unsigned int i = context.possible_ivar_vals.size(); i --; ) // NOTE: Ordering is a hack for libgit2
            for(unsigned int i = 0; i < context.possible_ivar_vals.size(); i ++ )
            {
                if( check_ivar_poss(context, i, context.possible_ivar_vals[i]) ) {
                    static Span sp;
                    //assert( context.possible_ivar_vals[i].has_rules() );
                    // Disable all metioned ivars in the possibilities
                    for(const auto& ty : context.possible_ivar_vals[i].types_coerce_to)
                        context.equate_types_from_shadow(sp,ty);
                    for(const auto& ty : context.possible_ivar_vals[i].types_unsize_to)
                        context.equate_types_from_shadow(sp,ty);
                    for(const auto& ty : context.possible_ivar_vals[i].types_coerce_from)
                        context.equate_types_to_shadow(sp,ty);
                    for(const auto& ty : context.possible_ivar_vals[i].types_unsize_from)
                        context.equate_types_to_shadow(sp,ty);

                    // Also disable inferrence (for this pass) for all ivars in affected bounds
                    if(false)
                    for(const auto& la : context.link_assoc)
                    {
                        bool found = false;
                        auto cb = [&](const auto& t) { return TU_TEST1(t.m_data, Infer, .index == i); };
                        if( la.left_ty != ::HIR::TypeRef() )
                            found |= visit_ty_with( la.left_ty, cb );
                        found |= visit_ty_with( la.impl_ty, cb );
                        for(const auto& t : la.params.m_types)
                            found |= visit_ty_with( t, cb );
                        if( found )
                        {
                            if(la.left_ty != ::HIR::TypeRef())
                                context.equate_types_shadow(sp, la.left_ty, false);
                            context.equate_types_shadow(sp, la.impl_ty, false);
                            for(const auto& t : la.params.m_types)
                                context.equate_types_shadow(sp, t, false);
                        }
                    }
                }
                else {
                    //assert( !context.m_ivars.peek_changed() );
                }
            }
        } // `if peek_changed` (ivar possibilities)

#if 0
        if( !context.m_ivars.peek_changed() )
        {
            DEBUG("--- Coercion consume");
            if( ! context.link_coerce.empty() )
            {
                auto ent = mv$(context.link_coerce.front());
                context.link_coerce.erase( context.link_coerce.begin() );

                const auto& sp = (*ent->right_node_ptr)->span();
                auto& src_ty = (*ent->right_node_ptr)->m_res_type;
                //src_ty = context.m_resolve.expand_associated_types( sp, mv$(src_ty) );
                ent->left_ty = context.m_resolve.expand_associated_types( sp, mv$(ent->left_ty) );
                DEBUG("- Equate coercion R" << ent->rule_idx << " " << ent->left_ty << " := " << src_ty);

                context.equate_types(sp, ent->left_ty, src_ty);
            }
        }
#endif
        // If nothing has changed, run check_ivar_poss again but allow it to assume is has all the options
        if( !context.m_ivars.peek_changed() )
        {
            // Check the possible equations
            DEBUG("--- IVar possibilities (fallback 1)");
            //for(unsigned int i = context.possible_ivar_vals.size(); i --; ) // NOTE: Ordering is a hack for libgit2
            for(unsigned int i = 0; i < context.possible_ivar_vals.size(); i ++ )
            {
                if( check_ivar_poss(context, i, context.possible_ivar_vals[i], IvarPossFallbackType::Assume) ) {
                    break;
                }
            }
        }
#if 0
        if( !context.m_ivars.peek_changed() )
        {
            DEBUG("--- Coercion consume");
            if( ! context.link_coerce.empty() )
            {
                auto ent = mv$(context.link_coerce.front());
                context.link_coerce.erase( context.link_coerce.begin() );

                const auto& sp = (*ent->right_node_ptr)->span();
                auto& src_ty = (*ent->right_node_ptr)->m_res_type;
                //src_ty = context.m_resolve.expand_associated_types( sp, mv$(src_ty) );
                ent->left_ty = context.m_resolve.expand_associated_types( sp, mv$(ent->left_ty) );
                DEBUG("- Equate coercion R" << ent->rule_idx << " " << ent->left_ty << " := " << src_ty);

                context.equate_types(sp, ent->left_ty, src_ty);
            }
        }
#endif
        // If nothing has changed, run check_ivar_poss again but ignoring the 'disable' flag
#if 1
        if( !context.m_ivars.peek_changed() )
        {
            // Check the possible equations
            DEBUG("--- IVar possibilities (fallback)");
            //for(unsigned int i = context.possible_ivar_vals.size(); i --; ) // NOTE: Ordering is a hack for libgit2
            for(unsigned int i = 0; i < context.possible_ivar_vals.size(); i ++ )
            {
                if( check_ivar_poss(context, i, context.possible_ivar_vals[i], IvarPossFallbackType::IgnoreWeakDisable) ) {
# if 1
                    break;
# else
                    static Span sp;
                    assert( context.possible_ivar_vals[i].has_rules() );
                    // Disable all metioned ivars in the possibilities
                    for(const auto& ty : context.possible_ivar_vals[i].types_coerce_to)
                        context.equate_types_from_shadow(sp,ty);
                    for(const auto& ty : context.possible_ivar_vals[i].types_unsize_to)
                        context.equate_types_from_shadow(sp,ty);
                    for(const auto& ty : context.possible_ivar_vals[i].types_coerce_from)
                        context.equate_types_to_shadow(sp,ty);
                    for(const auto& ty : context.possible_ivar_vals[i].types_unsize_from)
                        context.equate_types_to_shadow(sp,ty);

                    // Also disable inferrence (for this pass) for all ivars in affected bounds
                    for(const auto& la : context.link_assoc)
                    {
                        bool found = false;
                        auto cb = [&](const auto& t) { return TU_TEST1(t.m_data, Infer, .index == i); };
                        if( la.left_ty != ::HIR::TypeRef() )
                            found |= visit_ty_with( la.left_ty, cb );
                        found |= visit_ty_with( la.impl_ty, cb );
                        for(const auto& t : la.params.m_types)
                            found |= visit_ty_with( t, cb );
                        if( found )
                        {
                            if(la.left_ty != ::HIR::TypeRef())
                                context.equate_types_shadow(sp, la.left_ty, false);
                            context.equate_types_shadow(sp, la.impl_ty, false);
                            for(const auto& t : la.params.m_types)
                                context.equate_types_shadow(sp, t, false);
                        }
                    }
# endif
                }
                else {
                    //assert( !context.m_ivars.peek_changed() );
                }
            }
#endif
        } // `if peek_changed` (ivar possibilities #2)

        if( !context.m_ivars.peek_changed() )
        {
            DEBUG("--- Node revisits (fallback)");
            for( auto it = context.to_visit.begin(); it != context.to_visit.end(); )
            {
                ::HIR::ExprNode& node = **it;
                ExprVisitor_Revisit visitor { context, true };
                DEBUG("> " << &node << " " << typeid(node).name() << " -> " << context.m_ivars.fmt_type(node.m_res_type));
                node.visit( visitor );
                //  - If the node is completed, remove it
                if( visitor.node_completed() ) {
                    DEBUG("- Completed " << &node << " - " << typeid(node).name());
                    it = context.to_visit.erase(it);
                }
                else {
                    ++ it;
                }
            }
            #if 0
            for( auto it = context.adv_revisits.begin(); it != context.adv_revisits.end(); )
            {
                auto& ent = **it;
                if( ent.revisit(context, true) ) {
                    it = context.adv_revisits.erase(it);
                }
                else {
                    ++ it;
                }
            }
            #endif
        } // `if peek_changed` (node revisits)

        if( !context.m_ivars.peek_changed() )
        {
            size_t  len = context.adv_revisits.size();
            for(size_t i = 0; i < len; i ++)
            {
                auto& ent = *context.adv_revisits[i];
                ent.revisit(context, /*is_fallback=*/true);
                if( context.m_ivars.peek_changed() ) {
                    break;
                }
            }
        }

#if 1
        if( !context.m_ivars.peek_changed() )
        {
            // Check the possible equations
            DEBUG("--- IVar possibilities (final fallback)");
            //for(unsigned int i = context.possible_ivar_vals.size(); i --; ) // NOTE: Ordering is a hack for libgit2
            for(unsigned int i = 0; i < context.possible_ivar_vals.size(); i ++ )
            {
                if( check_ivar_poss(context, i, context.possible_ivar_vals[i], IvarPossFallbackType::FinalOption) ) {
                    break;
                }
            }
        }
#endif

#if 1
        if( !context.m_ivars.peek_changed() )
        {
            DEBUG("--- Coercion consume");
            if( ! context.link_coerce.empty() )
            {
                auto ent = mv$(context.link_coerce.front());
                context.link_coerce.erase( context.link_coerce.begin() );

                const auto& sp = (*ent->right_node_ptr)->span();
                auto& src_ty = (*ent->right_node_ptr)->m_res_type;
                //src_ty = context.m_resolve.expand_associated_types( sp, mv$(src_ty) );
                ent->left_ty = context.m_resolve.expand_associated_types( sp, mv$(ent->left_ty) );
                DEBUG("- Equate coercion R" << ent->rule_idx << " " << ent->left_ty << " := " << src_ty);

                context.equate_types(sp, ent->left_ty, src_ty);
            }
        }
#endif

        // Finally. If nothing changed, apply ivar defaults
        if( !context.m_ivars.peek_changed() )
        {
            DEBUG("- Applying defaults");
            if( context.m_ivars.apply_defaults() ) {
                context.m_ivars.mark_change();
            }
        }

        // Clear ivar possibilities for next pass
        for(auto& ivar_ent : context.possible_ivar_vals)
        {
            ivar_ent.reset();
        }

        count ++;
        context.m_resolve.compact_ivars(context.m_ivars);
    }
    if( count == MAX_ITERATIONS ) {
        BUG(root_ptr->span(), "Typecheck ran for too many iterations, max - " << MAX_ITERATIONS);
    }

    if( context.has_rules() )
    {
        for(const auto& coercion_p : context.link_coerce)
        {
            const auto& coercion = *coercion_p;
            const auto& sp = (**coercion.right_node_ptr).span();
            const auto& src_ty = (**coercion.right_node_ptr).m_res_type;
            WARNING(sp, W0000, "Spare Rule - " << context.m_ivars.fmt_type(coercion.left_ty) << " := " << context.m_ivars.fmt_type(src_ty) );
        }
        for(const auto& rule : context.link_assoc)
        {
            const auto& sp = rule.span;
            if( rule.name == "" || rule.left_ty == ::HIR::TypeRef() )
            {
                WARNING(sp, W0000, "Spare Rule - " << context.m_ivars.fmt_type(rule.impl_ty) << " : " << rule.trait << rule.params );
            }
            else
            {
                WARNING(sp, W0000, "Spare Rule - " << context.m_ivars.fmt_type(rule.left_ty)
                    << " = < " << context.m_ivars.fmt_type(rule.impl_ty) << " as " << rule.trait << rule.params << " >::" << rule.name );
            }
        }
        // TODO: Print revisit rules and advanced revisit rules.
        for(const auto& node : context.to_visit)
        {
            const auto& sp = node->span();
            WARNING(sp, W0000, "Spare rule - " << FMT_CB(os, { ExprVisitor_Print ev(context, os); node->visit(ev); }) << " -> " << context.m_ivars.fmt_type(node->m_res_type));
        }
        for(const auto& adv : context.adv_revisits)
        {
            WARNING(adv->span(), W0000, "Spare Rule - " << FMT_CB(os, adv->fmt(os)));
        }
        BUG(root_ptr->span(), "Spare rules left after typecheck stabilised");
    }
    DEBUG("root_ptr = " << typeid(*root_ptr).name() << " " << root_ptr->m_res_type);

    // - Recreate the pointer
    expr.reset( root_ptr.release() );
    //  > Steal the binding types
    expr.m_bindings.reserve( context.m_bindings.size() );
    for(auto& binding : context.m_bindings) {
        expr.m_bindings.push_back( binding.ty.clone() );
    }

    // - Validate typeck
    {
        DEBUG("==== VALIDATE ==== (" << count << " rounds)");
        context.dump();

        ExprVisitor_Apply   visitor { context };
        visitor.visit_node_ptr( expr );
    }

    {
        StaticTraitResolve  static_resolve(ms.m_crate);
        if( ms.m_impl_generics )
        {
            static_resolve.set_impl_generics_raw(*ms.m_impl_generics);
        }
        if( ms.m_item_generics )
        {
            static_resolve.set_item_generics_raw(*ms.m_item_generics);
        }
        Typecheck_Expressions_ValidateOne(static_resolve, args, result_type, expr);
    }
}

