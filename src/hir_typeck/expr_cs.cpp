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

#include "helpers.hpp"
#include "expr_visit.hpp"

namespace {
    inline HIR::ExprNodeP mk_exprnodep(HIR::ExprNode* en, ::HIR::TypeRef ty){ en->m_res_type = mv$(ty); return HIR::ExprNodeP(en); }
}
#define NEWNODE(TY, SP, CLASS, ...)  mk_exprnodep(new HIR::ExprNode##CLASS(SP ,## __VA_ARGS__), TY)

// PLAN: Build up a set of conditions that are easier to solve
struct Context
{
    class Revisitor
    {
    public:
        virtual ~Revisitor() = default;
        virtual void fmt(::std::ostream& os) const = 0;
        virtual bool revisit(Context& context) = 0;
    };

    struct Binding
    {
        ::std::string   name;
        ::HIR::TypeRef  ty;
        //unsigned int ivar;
    };

    /// Inferrence variable equalities
    struct Coercion
    {
        ::HIR::TypeRef  left_ty;
        ::HIR::ExprNodeP* right_node_ptr;

        friend ::std::ostream& operator<<(::std::ostream& os, const Coercion& v) {
            os << v.left_ty << " := " << v.right_node_ptr << " " << &**v.right_node_ptr << " (" << (*v.right_node_ptr)->m_res_type << ")";
            return os;
        }
    };
    struct Associated
    {
        Span    span;
        ::HIR::TypeRef  left_ty;

        ::HIR::SimplePath   trait;
        ::HIR::PathParams   params;
        ::HIR::TypeRef  impl_ty;
        ::std::string   name;   // if "", no type is used (and left is ignored) - Just does trait selection

        // HACK: operators are special - the result when both types are primitives is ALWAYS the lefthand side
        bool    is_operator;

        friend ::std::ostream& operator<<(::std::ostream& os, const Associated& v) {
            if( v.name == "" ) {
                os << "req ty " << v.impl_ty << " impl " << v.trait << v.params;
            }
            else {
                os << v.left_ty << " = " << "< `" << v.impl_ty << "` as `" << v.trait << v.params << "` >::" << v.name;
            }
            return os;
        }
    };

    struct IVarPossible
    {
        bool force_no_to = false;
        bool force_no_from = false;
        ::std::vector<::HIR::TypeRef>   types_coerce_to;
        ::std::vector<::HIR::TypeRef>   types_unsize_to;
        ::std::vector<::HIR::TypeRef>   types_coerce_from;
        ::std::vector<::HIR::TypeRef>   types_unsize_from;
        //::std::vector<::HIR::TypeRef>   types_default;

        void reset() {
            //auto tmp = mv$(this->types_default);
            *this = IVarPossible();
            //this->types_default = mv$(tmp);
        }
        bool has_rules() const {
            return !types_unsize_to.empty() || !types_coerce_to.empty() || !types_unsize_from.empty() || !types_coerce_from.empty() /* || !types_default.empty()*/;
        }
    };

    const ::HIR::Crate& m_crate;

    ::std::vector<Binding>  m_bindings;
    HMTypeInferrence    m_ivars;
    TraitResolution m_resolve;

    ::std::vector<Coercion> link_coerce;
    ::std::vector<Associated> link_assoc;
    /// Nodes that need revisiting (e.g. method calls when the receiver isn't known)
    ::std::vector< ::HIR::ExprNode*>    to_visit;
    /// Callback-based revisits (e.g. for slice patterns handling slices/arrays)
    ::std::vector< ::std::unique_ptr<Revisitor> >   adv_revisits;

    ::std::vector<bool> m_ivars_sized;
    ::std::vector< IVarPossible>    possible_ivar_vals;

    const ::HIR::SimplePath m_lang_Box;

    Context(const ::HIR::Crate& crate, const ::HIR::GenericParams* impl_params, const ::HIR::GenericParams* item_params):
        m_crate(crate),
        m_resolve(m_ivars, crate, impl_params, item_params),
        m_lang_Box( crate.get_lang_item_path_opt("owned_box") )
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

    void possible_equate_type(unsigned int ivar_index, const ::HIR::TypeRef& t, bool is_to, bool is_borrow);
    void possible_equate_type_disable(unsigned int ivar_index, bool is_to);

    // - Add a pattern binding (forcing the type to match)
    void add_binding(const Span& sp, ::HIR::Pattern& pat, const ::HIR::TypeRef& type);
    void add_binding_inner(const Span& sp, const ::HIR::PatternBinding& pb, ::HIR::TypeRef type);

    void add_var(const Span& sp, unsigned int index, const ::std::string& name, ::HIR::TypeRef type);
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
                    if( !context.m_resolve.trait_contains_type(sp, real_trait, *be.trait.m_trait_ptr, assoc.first,  type_trait_path) )
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

        TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
        (Generic,
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
            ),
        (UfcsKnown,
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
            ),
        (UfcsUnknown,
            // TODO: Eventually, the HIR `Resolve UFCS` pass will be removed, leaving this code responsible for locating the item.
            TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
            ),
        (UfcsInherent,
            // NOTE: This case is kinda long, so it's refactored out into a helper
            if( !visit_call_populate_cache_UfcsInherent(context, sp, path, cache, fcn_ptr) ) {
                return false;
            }
            )
        )

        assert( fcn_ptr );
        cache.m_fcn = fcn_ptr;
        const auto& fcn = *fcn_ptr;
        const auto& monomorph_cb = cache.m_monomorph_cb;

        // --- Monomorphise the argument/return types (into current context)
        for(const auto& arg : fcn.m_args) {
            DEBUG("Arg " << arg.first << ": " << arg.second);
            cache.m_arg_types.push_back( monomorphise_type_with(sp, arg.second,  monomorph_cb, false) );
        }
        DEBUG("Ret " << fcn.m_return);
        cache.m_arg_types.push_back( monomorphise_type_with(sp, fcn.m_return,  monomorph_cb, false) );

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
            TRACE_FUNCTION_F(&node << " { ... }");

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
                    TU_IFLET(::HIR::TypeRef::Data, this->context.get_type(snp->m_res_type).m_data, Infer, e,
                        switch(e.ty_class)
                        {
                        case ::HIR::InferClass::Integer:
                        case ::HIR::InferClass::Float:
                            diverges = false;
                            break;
                        default:
                            defer = true;
                            break;
                        }
                    )
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
        }

        void visit(::HIR::ExprNode_Let& node) override
        {
            TRACE_FUNCTION_F(&node << " let " << node.m_pattern << ": " << node.m_type);

            this->context.add_ivars( node.m_type );
            this->context.add_binding(node.span(), node.m_pattern, node.m_type);

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
        }
        void visit(::HIR::ExprNode_Match& node) override
        {
            TRACE_FUNCTION_F(&node << " match ...");

            const auto& val_type = node.m_value->m_res_type;

            {
                auto _ = this->push_inner_coerce_scoped(true);
                this->context.add_ivars(node.m_value->m_res_type);

                // TODO: If a coercion point (and ivar for the value) is placed here, it will allow `match &string { "..." ... }`
                node.m_value->visit( *this );
            }

            for(auto& arm : node.m_arms)
            {
                TRACE_FUNCTION_F("ARM " << arm.m_patterns);
                for(auto& pat : arm.m_patterns)
                {
                    this->context.add_binding(node.span(), pat, val_type);
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
                case ::HIR::ExprNode_BinOp::Op::CmpLt:   item_name = "ord"; break;
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:  item_name = "ord"; break;
                case ::HIR::ExprNode_BinOp::Op::CmpGt:   item_name = "ord"; break;
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:  item_name = "ord"; break;
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
            this->context.add_ivars( node.m_index->m_res_type );

            node.m_value->visit( *this );
            node.m_index->visit( *this );

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

                return ::HIR::TypeRef::new_path( gp.clone(), ::HIR::TypeRef::TypePathBinding::make_Struct(&str) );
            }
            else
            {
                auto s_path = gp.m_path;
                s_path.m_components.pop_back();

                const auto& enm = this->context.m_crate.get_enum_by_path(sp, s_path);
                fix_param_count(sp, this->context, ::HIR::TypeRef(), false, gp, enm.m_params, gp.m_params);

                return ::HIR::TypeRef::new_path( ::HIR::GenericPath(mv$(s_path), gp.m_params.clone()), ::HIR::TypeRef::TypePathBinding::make_Enum(&enm) );
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
            TU_MATCH(::HIR::TypeRef::TypePathBinding, (ty.m_data.as_Path().binding), (e),
            (Unbound, ),
            (Opaque, ),
            (Enum,
                const auto& var_name = node.m_path.m_path.m_components.back();
                const auto& enm = *e;
                size_t idx = enm.find_variant(var_name);
                const auto& var_ty = enm.m_data.as_Data()[idx].type;
                const auto& str = *var_ty.m_data.as_Path().binding.as_Struct();
                ASSERT_BUG(sp, str.m_data.is_Tuple(), "Pointed variant of TupleVariant (" << node.m_path << ") isn't a Tuple");
                fields_ptr = &str.m_data.as_Tuple();
                ),
            (Struct,
                ASSERT_BUG(sp, e->m_data.is_Tuple(), "Pointed struct in TupleVariant (" << node.m_path << ") isn't a Tuple");
                fields_ptr = &e->m_data.as_Tuple();
                ),
            (Union,
                BUG(sp, "TupleVariant pointing to a union");
                )
            )
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
            TRACE_FUNCTION_F(&node << " " << node.m_path << "{...} [" << (node.m_is_struct ? "struct" : "enum") << "]");
            this->add_ivars_generic_path(node.span(), node.m_path);
            for( auto& val : node.m_values ) {
                this->context.add_ivars( val.second->m_res_type );
            }
            if( node.m_base_value ) {
                this->context.add_ivars( node.m_base_value->m_res_type );
            }

            // - Create ivars in path, and set result type
            const auto ty = this->get_structenum_ty(node.span(), node.m_is_struct, node.m_path);
            this->context.equate_types(node.span(), node.m_res_type, ty);
            if( node.m_base_value ) {
                this->context.equate_types(node.span(), node.m_base_value->m_res_type, ty);
            }

            const ::HIR::t_struct_fields* fields_ptr = nullptr;
            const ::HIR::GenericParams* generics;
            TU_MATCH(::HIR::TypeRef::TypePathBinding, (ty.m_data.as_Path().binding), (e),
            (Unbound, ),
            (Opaque, ),
            (Enum,
                const auto& var_name = node.m_path.m_path.m_components.back();
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
                ),
            (Union,
                TODO(node.span(), "StructLiteral of a union - " << ty);
                ),
            (Struct,
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
                )
            )
            ASSERT_BUG(node.span(), fields_ptr, "");
            const ::HIR::t_struct_fields& fields = *fields_ptr;

            const auto& ty_params = node.m_path.m_params.m_types;
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
                ASSERT_BUG(node.span(), it != fields.end(), "Field '" << name << "' not found in struct " << node.m_path);
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

            auto _ = this->push_inner_coerce_scoped(true);
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
            const auto ty = ::HIR::TypeRef::new_path( node.m_path.clone(), ::HIR::TypeRef::TypePathBinding::make_Union(&unm) );

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
            this->equate_types_inner_coerce(node.span(), *des_ty,  node.m_value);

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
            const ::std::string& method_name = node.m_method;
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
                TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Tuple, e,
                    if( e.size() != node.m_vals.size() ) {
                        ERROR(node.span(), E0000, "Tuple literal node count mismatches with return type");
                    }
                )
                else if( ty.m_data.is_Infer() ) {
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

            // Cleanly equate into array (with coercions)
            // - Result type already set, just need to extract ivar
            const auto& inner_ty = *node.m_res_type.m_data.as_Array().inner;
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
            auto ty = ::HIR::TypeRef::new_array( ::HIR::TypeRef(), node.m_size_val );
            this->context.add_ivars(ty);
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

            TU_MATCH(::HIR::Path::Data, (node.m_path.m_data), (e),
            (Generic,
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

                    auto ty = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Function(mv$(ft)) );
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
                        box$( ::HIR::TypeRef( node.m_path.clone(), ::HIR::TypeRef::TypePathBinding::make_Struct(&s) ) ),
                        {}
                        };
                    for( const auto& arg : se )
                    {
                        ft.m_arg_types.push_back( monomorphise_type(sp, s.m_params, e.m_params, arg.ent) );
                    }
                    //apply_bounds_as_rules(this->context, sp, s.m_params, monomorph_cb, /*is_impl_level=*/true);

                    auto ty = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Function(mv$(ft)) );
                    this->context.equate_types(sp, node.m_res_type, ty);
                    } break;
                case ::HIR::ExprNode_PathValue::ENUM_VAR_CONSTR: {
                    const auto& var_name = e.m_path.m_components.back();
                    auto enum_path = e.m_path;
                    enum_path.m_components.pop_back();
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
                        box$( ::HIR::TypeRef( ::HIR::GenericPath(mv$(enum_path), e.m_params.clone()), ::HIR::TypeRef::TypePathBinding::make_Enum(&enm) ) ),
                        {}
                        };
                    for( const auto& arg : var_data )
                    {
                        ft.m_arg_types.push_back( monomorphise_type(sp, enm.m_params, e.m_params, arg.ent) );
                    }
                    //apply_bounds_as_rules(this->context, sp, enm.m_params, monomorph_cb, /*is_impl_level=*/true);

                    auto ty = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Function(mv$(ft)) );
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
                ),
            (UfcsUnknown,
                BUG(sp, "Encountered UfcsUnknown");
                ),
            (UfcsKnown,
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
                ),
            (UfcsInherent,
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
                )
            )
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
            TRACE_FUNCTION_F(&node << " " << node.m_name << "{" << node.m_slot << "}");

            this->context.equate_types(node.span(), node.m_res_type,  this->context.get_var(node.span(), node.m_slot));
        }

        void visit(::HIR::ExprNode_Closure& node) override
        {
            TRACE_FUNCTION_F(&node << " |...| ...");
            for(auto& arg : node.m_args) {
                this->context.add_ivars( arg.second );
                this->context.add_binding( node.span(), arg.first, arg.second );
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
                return ty.m_data.is_Diverge();// || (ty.m_data.is_Infer() && ty.m_data.as_Infer().ty_class == ::HIR::InferClass::Diverge);
                };

            assert( !node.m_nodes.empty() );
            const auto& last_ty = this->context.get_type( node.m_nodes.back()->m_res_type );
            DEBUG("_Block: last_ty = " << last_ty);

            bool diverges = false;
            // NOTE: If the final statement in the block diverges, mark this as diverging
            TU_IFLET(::HIR::TypeRef::Data, last_ty.m_data, Infer, e,
                switch(e.ty_class)
                {
                case ::HIR::InferClass::Integer:
                case ::HIR::InferClass::Float:
                    diverges = false;
                    break;
                default:
                    return ;
                }
            )
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

            TU_MATCH( ::HIR::TypeRef::Data, (tgt_ty.m_data), (e),
            (Infer,
                // Can't know anything
                //this->m_completed = true;
                DEBUG("- Target type is still _");
                ),
            (Diverge,
                BUG(sp, "");
                ),
            (Primitive,
                // Don't have anything to contribute
                this->m_completed = true;
                ),
            (Path,
                this->context.equate_types_coerce(sp, tgt_ty, node.m_value);
                this->m_completed = true;
                return ;
                ),
            (Generic,
                TODO(sp, "_Cast Generic");
                ),
            (TraitObject,
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                ),
            (ErasedType,
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                ),
            (Array,
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                ),
            (Slice,
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                ),
            (Tuple,
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                ),
            (Borrow,
                // Emit a coercion and delete this revisit
                this->context.equate_types_coerce(sp, tgt_ty, node.m_value);
                this->m_completed = true;
                return ;
                ),
            (Pointer,
                const auto& ity = this->context.get_type(*e.inner);
                TU_MATCH_DEF( ::HIR::TypeRef::Data, (src_ty.m_data), (s_e),
                (
                    ERROR(sp, E0000, "Invalid cast to pointer from " << src_ty);
                    ),
                (Function,
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
                    ),
                (Primitive,
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
                    ),
                (Infer,
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
                    ),
                (Borrow,
                    // Check class (must be equal) and type
                    if( s_e.type != e.type ) {
                        ERROR(sp, E0000, "Invalid cast from " << src_ty << " to " << tgt_ty);
                    }
                    const auto& src_inner = this->context.get_type(*s_e.inner);

                    // NOTE: &mut T -> *mut U where T: Unsize<U> is allowed
                    // TODO: Wouldn't this be better served by a coercion point?
                    TU_IFLET( ::HIR::TypeRef::Data, src_inner.m_data, Infer, s_e_i,
                        // If the type is an ivar, possible equate
                        this->context.possible_equate_type_unsize_to(s_e_i.index, *e.inner);
                    )
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
                    ),
                (Pointer,
                    // Allow with no link?
                    // TODO: In some rare cases, this ivar could be completely
                    // unrestricted. If in fallback mode
                    const auto& dst_inner = this->context.get_type(*e.inner);
                    if(this->m_is_fallback)
                    {
                        if( dst_inner.m_data.is_Infer() )
                        {
                            this->context.equate_types(sp, *e.inner, *s_e.inner);
                        }
                    }
                    else if( dst_inner.m_data.is_Infer() )
                    {
                        return ;
                    }
                    else
                    {
                    }
                    this->m_completed = true;
                    )
                )
                ),
            (Function,
                // NOTE: Valid if it's causing a fn item -> fn pointer coercion
                TU_MATCH_DEF( ::HIR::TypeRef::Data, (src_ty.m_data), (s_e),
                (
                    ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty) << " to " << this->context.m_ivars.fmt_type(src_ty));
                    ),
                (Function,
                    // Check that the ABI and unsafety is correct
                    if( s_e.m_abi != e.m_abi || s_e.is_unsafe != e.is_unsafe || s_e.m_arg_types.size() != e.m_arg_types.size() )
                        ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty) << " to " << this->context.m_ivars.fmt_type(src_ty));
                    // TODO: Equate inner types
                    this->context.equate_types(sp, tgt_ty, src_ty);
                    )
                )
                ),
            (Closure,
                BUG(sp, "Attempting to cast to a closure type - impossible");
                )
            )
        }
        void visit(::HIR::ExprNode_Unsize& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Index& node) override {
            const auto& lang_Index = this->context.m_crate.get_lang_item_path(node.span(), "index");
            const auto& val_ty = this->context.get_type(node.m_value->m_res_type);
            const auto& idx_ty = this->context.get_type(node.m_index->m_res_type);
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

            TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (e),
            (
                const auto& op_trait = this->context.m_crate.get_lang_item_path(node.span(), "deref");
                this->context.equate_types_assoc(node.span(), node.m_res_type,  op_trait, {}, node.m_value->m_res_type.clone(), "Target");
                ),
            (Infer,
                // Keep trying
                this->context.equate_types_from_shadow(node.span(), node.m_res_type);
                return ;
                ),
            (Borrow,
                // - Not really needed, but this is cheaper.
                this->context.equate_types(node.span(), node.m_res_type, *e.inner);
                ),
            (Pointer,
                // TODO: Figure out if this node is in an unsafe block.
                this->context.equate_types(node.span(), node.m_res_type, *e.inner);
                )
            )
            this->m_completed = true;
        }
        void visit(::HIR::ExprNode_Emplace& node) override {
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
                    return ;
                }

                // Where P = `placer_ty` and D = `data_ty`
                // Result type is <<P as Placer<D>>::Place as InPlace<D>>::Owner
                const auto& lang_Placer = this->context.m_crate.get_lang_item_path(sp, "placer_trait");
                const auto& lang_InPlace = this->context.m_crate.get_lang_item_path(sp, "in_place_trait");
                // - Bound P: Placer<D>
                this->context.equate_types_assoc(sp, {}, lang_Placer, ::make_vec1(data_ty.clone()), placer_ty, "");
                // - 
                auto place_ty = ::HIR::TypeRef( ::HIR::Path(placer_ty.clone(), ::HIR::GenericPath(lang_Placer, ::HIR::PathParams(data_ty.clone())), "Place") );
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

            // - Shadow (prevent ivar guessing) every parameter
            this->context.equate_types_from_shadow(node.span(), node.m_res_type);
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
                else
                {
                    ::HIR::TypeRef  fcn_args_tup;
                    ::HIR::TypeRef  fcn_ret;

                    // TODO: Use `find_trait_impls` instead of two different calls
                    // - This will get the TraitObject impl search too

                    // Locate an impl of FnOnce (exists for all other Fn* traits)
                    unsigned int count = 0;
                    this->context.m_resolve.find_trait_impls(node.span(), lang_FnOnce, trait_pp, ty, [&](auto impl, auto cmp)->bool {
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
                    if(count == 1)
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
                            fcn_ret = ::HIR::TypeRef(::HIR::Path(::HIR::Path::Data::make_UfcsKnown({
                                box$(ty.clone()),
                                // - Clone argument tuple, as it's stolen into cache below
                                ::HIR::GenericPath(lang_FnOnce, ::HIR::PathParams(fcn_args_tup.clone())),
                                "Output",
                                {}
                            })));
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
            TRACE_FUNCTION_F("(CallMethod) {" << this->context.m_ivars.fmt_type(ty) << "}." << node.m_method << node.m_params);

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
            unsigned int deref_count = this->context.m_resolve.autoderef_find_method(node.span(), node.m_traits, node.m_trait_param_ivars, ty, node.m_method,  possible_methods);
        try_again:
            if( deref_count != ~0u )
            {
                DEBUG("possible_methods = " << possible_methods);
                if( possible_methods.empty() )
                {
                    ERROR(sp, E0000, "No applicable methods for {" << ty << "}." << node.m_method);
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

            this->context.equate_types_from_shadow(node.span(), node.m_res_type);

            ::HIR::TypeRef  out_type;

            // Using autoderef, locate this field
            unsigned int deref_count = 0;
            ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
            const auto* current_ty = &node.m_value->m_res_type;
            ::std::vector< ::HIR::TypeRef>  deref_res_types;

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
                if( this->context.m_resolve.find_field(node.span(), ty, field_name, out_type) ) {
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
    };

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
            TRACE_FUNCTION_FR(&node << " " << &node << " " << node_ty << " : " << node.m_res_type, node_ty);
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
            this->check_type_resolved_pp(node.span(), node.m_path.m_params, ::HIR::TypeRef());
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
            auto tmp = ::HIR::TypeRef(path.clone());
            //auto tmp = ::HIR::TypeRef();
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
            auto tmp = ::HIR::TypeRef(path.clone());
            check_type_resolved_pp(sp, path.m_params, tmp);
        }
        void check_type_resolved(const Span& sp, ::HIR::TypeRef& ty, const ::HIR::TypeRef& top_type) const {
            TRACE_FUNCTION_F(ty);
            TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
            (Infer,
                auto new_ty = this->ivars.get_type(ty).clone();
                // - Move over before checking, so that the source type mentions the correct ivar
                ty = mv$(new_ty);
                if( ty.m_data.is_Infer() ) {
                    ERROR(sp, E0000, "Failed to infer type " << ty << " in "  << top_type);
                }
                check_type_resolved(sp, ty, top_type);
                ),
            (Diverge,
                // Leaf
                ),
            (Primitive,
                // Leaf
                ),
            (Path,
                check_type_resolved_path(sp, e.path, top_type);
                ),
            (Generic,
                // Leaf - no ivars
                ),
            (TraitObject,
                check_type_resolved_pp(sp, e.m_trait.m_path.m_params, top_type);
                for(auto& at : e.m_trait.m_type_bounds)
                    check_type_resolved(sp, at.second, top_type);
                for(auto& marker : e.m_markers) {
                    check_type_resolved_pp(sp, marker.m_params, top_type);
                }
                ),
            (ErasedType,
                ASSERT_BUG(sp, e.m_origin != ::HIR::SimplePath(), "ErasedType " << ty << " wasn't bound to its origin");
                check_type_resolved_path(sp, e.m_origin, top_type);
                ),
            (Array,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Slice,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Tuple,
                for(auto& st : e)
                    this->check_type_resolved(sp, st, top_type);
                ),
            (Borrow,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Pointer,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Function,
                this->check_type_resolved(sp, *e.m_rettype, top_type);
                for(auto& st : e.m_arg_types)
                    this->check_type_resolved(sp, st, top_type);
                ),
            (Closure,
                this->check_type_resolved(sp, *e.m_rettype, top_type);
                for(auto& st : e.m_arg_types)
                    this->check_type_resolved(sp, st, top_type);
                )
            )
        }
    };
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
    for(const auto& v : link_coerce) {
        DEBUG(v);
    }
    for(const auto& v : link_assoc) {
        DEBUG(v);
    }
    for(const auto& v : to_visit) {
        DEBUG(&*v << " " << typeid(*v).name() << " -> " << this->m_ivars.fmt_type(v->m_res_type));
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

    visit_ty_with(ri, [&](const auto& ty)->bool {
        if( ty.m_data.is_Generic() && ty.m_data.as_Generic().binding >> 8 == 2 ) {
            BUG(sp, "Type contained an impl placeholder parameter - " << ri);
        }
        return false;
        });
    visit_ty_with(li, [&](const auto& ty)->bool {
        if( ty.m_data.is_Generic() && ty.m_data.as_Generic().binding >> 8 == 2 ) {
            BUG(sp, "Type contained an impl placeholder parameter - " << li);
        }
        return false;
        });

    ::HIR::TypeRef  l_tmp;
    ::HIR::TypeRef  r_tmp;
    const auto& l_t = this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(li), l_tmp);
    const auto& r_t = this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(ri), r_tmp);

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
    TU_IFLET(::HIR::TypeRef::Data, r_t.m_data, Path, r_e,
        TU_IFLET(::HIR::Path::Data, r_e.path.m_data, UfcsKnown, rpe,
            if( r_e.binding.is_Unbound() ) {
                this->equate_types_assoc(sp, l_t,  rpe.trait.m_path, rpe.trait.m_params.clone().m_types, *rpe.type,  rpe.item.c_str());
                return ;
            }
        )
    )
    TU_IFLET(::HIR::TypeRef::Data, l_t.m_data, Path, l_e,
        TU_IFLET(::HIR::Path::Data, l_e.path.m_data, UfcsKnown, lpe,
            if( l_e.binding.is_Unbound() ) {
                this->equate_types_assoc(sp, r_t,  lpe.trait.m_path, lpe.trait.m_params.clone().m_types, *lpe.type,  lpe.item.c_str());
                return ;
            }
        )
    )

    DEBUG("- l_t = " << l_t << ", r_t = " << r_t);
    TU_IFLET(::HIR::TypeRef::Data, r_t.m_data, Infer, r_e,
        TU_IFLET(::HIR::TypeRef::Data, l_t.m_data, Infer, l_e,
            // If both are infer, unify the two ivars (alias right to point to left)
            // TODO: Unify sized flags

            if( (r_e.index < m_ivars_sized.size() && m_ivars_sized.at(r_e.index))
              ||(l_e.index < m_ivars_sized.size() && m_ivars_sized.at(l_e.index))
                )
            {
                this->require_sized(sp, l_t);
                this->require_sized(sp, r_t);
            }

            this->m_ivars.ivar_unify(l_e.index, r_e.index);
        )
        else {
            // Righthand side is infer, alias it to the left
            if( r_e.index < m_ivars_sized.size() && m_ivars_sized.at(r_e.index) ) {
                this->require_sized(sp, l_t);
            }
            this->m_ivars.set_ivar_to(r_e.index, l_t.clone());
        }
    )
    else {
        TU_IFLET(::HIR::TypeRef::Data, l_t.m_data, Infer, l_e,
            // Lefthand side is infer, alias it to the right
            if( l_e.index < m_ivars_sized.size() && m_ivars_sized.at(l_e.index) ) {
                this->require_sized(sp, r_t);
            }
            this->m_ivars.set_ivar_to(l_e.index, r_t.clone());
        )
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
                TU_IFLET(::HIR::TypeRef::Data, li.m_data, Infer, l_e,
                    this->m_ivars.set_ivar_to(l_e.index, r_t.clone());
                )
                return ;
            }*/
            else if( r_t.m_data.is_Diverge() ) {
                TU_IFLET(::HIR::TypeRef::Data, ri.m_data, Infer, r_e,
                    this->m_ivars.set_ivar_to(r_e.index, l_t.clone());
                )
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
            TU_MATCH( ::HIR::TypeRef::Data, (l_t.m_data, r_t.m_data), (l_e, r_e),
            (Infer,
                throw "";
                ),
            (Diverge,
                // ignore?
                ),
            (Primitive,
                if( l_e != r_e ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                ),
            (Path,
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
                ),
            (Generic,
                if( l_e.binding != r_e.binding ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                ),
            (TraitObject,
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
                ),
            (ErasedType,
                ASSERT_BUG(sp, l_e.m_origin != ::HIR::SimplePath(), "ErasedType " << l_t << " wasn't bound to its origin");
                ASSERT_BUG(sp, r_e.m_origin != ::HIR::SimplePath(), "ErasedType " << r_t << " wasn't bound to its origin");
                // TODO: Ivar equate origin
                if( l_e.m_origin != r_e.m_origin ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - different source");
                }
                ),
            (Array,
                this->equate_types_inner(sp, *l_e.inner, *r_e.inner);
                if( l_e.size_val != r_e.size_val ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - sizes differ");
                }
                ),
            (Slice,
                this->equate_types_inner(sp, *l_e.inner, *r_e.inner);
                ),
            (Tuple,
                if( l_e.size() != r_e.size() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Tuples are of different length");
                }
                for(unsigned int i = 0; i < l_e.size(); i ++)
                {
                    this->equate_types_inner(sp, l_e[i], r_e[i]);
                }
                ),
            (Borrow,
                if( l_e.type != r_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Borrow classes differ");
                }
                this->equate_types_inner(sp, *l_e.inner, *r_e.inner);
                ),
            (Pointer,
                if( l_e.type != r_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Pointer mutability differs");
                }
                this->equate_types_inner(sp, *l_e.inner, *r_e.inner);
                ),
            (Function,
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
                ),
            (Closure,
                if( l_e.m_arg_types.size() != r_e.m_arg_types.size() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                this->equate_types_inner(sp, *l_e.m_rettype, *r_e.m_rettype);
                for( unsigned int i = 0; i < l_e.m_arg_types.size(); i ++ )
                {
                    this->equate_types_inner(sp, l_e.m_arg_types[i], r_e.m_arg_types[i]);
                }
                )
            )
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
void Context::add_binding(const Span& sp, ::HIR::Pattern& pat, const ::HIR::TypeRef& type)
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
    TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
    (Any,
        // Just leave it, the pattern says nothing
        ),
    (Value,
        H::handle_value(*this, sp, type, e.val);
        ),
    (Range,
        H::handle_value(*this, sp, type, e.start);
        H::handle_value(*this, sp, type, e.end);
        ),
    (Box,
        if( m_lang_Box == ::HIR::SimplePath() )
            ERROR(sp, E0000, "Use of `box` pattern without the `owned_box` lang item");
        const auto& ty = this->get_type(type);
        // Two options:
        // 1. Enforce that the current type must be "owned_box"
        // 2. Make a new ivar for the inner and emit an associated type bound on Deref

        // Taking option 1 for now
        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Path, te,
            if( te.path.m_data.is_Generic() && te.path.m_data.as_Generic().m_path == m_lang_Box ) {
                // Box<T>
                const auto& inner = te.path.m_data.as_Generic().m_params.m_types.at(0);
                this->add_binding(sp, *e.sub, inner);
                break ;
            }
        )

        auto inner = this->m_ivars.new_ivar_tr();
        this->add_binding(sp, *e.sub, inner);
        ::HIR::GenericPath  path { m_lang_Box, ::HIR::PathParams(mv$(inner)) };
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypeRef::TypePathBinding(&m_crate.get_struct_by_path(sp, m_lang_Box))) );
        ),
    (Ref,
        const auto& ty = this->get_type(type);
        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Borrow, te,
            if( te.type != e.type ) {
                ERROR(sp, E0000, "Pattern-type mismatch, &-ptr mutability mismatch");
            }
            this->add_binding(sp, *e.sub, *te.inner);
        )
        else {
            auto inner = this->m_ivars.new_ivar_tr();
            this->add_binding(sp, *e.sub, inner);
            this->equate_types(sp, type, ::HIR::TypeRef::new_borrow( e.type, mv$(inner) ));
        }
        ),
    (Tuple,
        const auto& ty = this->get_type(type);
        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Tuple, te,

            if( e.sub_patterns.size() != te.size() ) {
                ERROR(sp, E0000, "Tuple pattern with an incorrect number of fields, expected " << e.sub_patterns.size() << "-tuple, got " << ty);
            }

            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                this->add_binding(sp, e.sub_patterns[i], te[i] );
        )
        else {

            ::std::vector< ::HIR::TypeRef>  sub_types;
            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ ) {
                sub_types.push_back( this->m_ivars.new_ivar_tr() );
                this->add_binding(sp, e.sub_patterns[i], sub_types[i] );
            }
            this->equate_types(sp, ty, ::HIR::TypeRef( mv$(sub_types) ));
        }
        ),
    (SplitTuple,
        const auto& ty = this->get_type(type);
        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Tuple, te,
            // - Should have been checked in AST resolve
            ASSERT_BUG(sp, e.leading.size() + e.trailing.size() <= te.size(), "Invalid field count for split tuple pattern");

            unsigned int tup_idx = 0;
            for(auto& subpat : e.leading) {
                this->add_binding(sp, subpat, te[tup_idx++]);
            }
            tup_idx = te.size() - e.trailing.size();
            for(auto& subpat : e.trailing) {
                this->add_binding(sp, subpat, te[tup_idx++]);
            }

            // TODO: Should this replace the pattern with a non-split?
            // - Changing the address of the pattern means that the below revisit could fail.
            e.total_size = te.size();
        )
        else {
            if( !ty.m_data.is_Infer() ) {
                ERROR(sp, E0000, "Tuple pattern on non-tuple");
            }

            ::std::vector<::HIR::TypeRef>   leading_tys;
            leading_tys.reserve(e.leading.size());
            for(auto& subpat : e.leading) {
                leading_tys.push_back( this->m_ivars.new_ivar_tr() );
                this->add_binding(sp, subpat, leading_tys.back());
            }
            ::std::vector<::HIR::TypeRef>   trailing_tys;
            for(auto& subpat : e.trailing) {
                trailing_tys.push_back( this->m_ivars.new_ivar_tr() );
                this->add_binding(sp, subpat, trailing_tys.back());
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

                void fmt(::std::ostream& os) const override {
                    os << "SplitTuplePatRevisit { " << m_outer_ty << " = (" << m_leading_tys << ", ..., " << m_trailing_tys << ") }";
                }
                bool revisit(Context& context) override {
                    const auto& ty = context.get_type(m_outer_ty);
                    TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Infer, te,
                        return false;
                    )
                    else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Tuple, te,
                        if( te.size() < m_leading_tys.size() + m_trailing_tys.size() )
                            ERROR(sp, E0000, "Tuple pattern too large for tuple");
                        for(unsigned int i = 0; i < m_leading_tys.size(); i ++)
                            context.equate_types(sp, te[i], m_leading_tys[i]);
                        unsigned int ofs = te.size() - m_trailing_tys.size();
                        for(unsigned int i = 0; i < m_trailing_tys.size(); i ++)
                            context.equate_types(sp, te[ofs+i], m_trailing_tys[i]);
                        m_pat_total_size = te.size();
                        return true;
                    )
                    else {
                        ERROR(sp, E0000, "Tuple pattern on non-tuple - " << ty);
                    }
                }
            };

            // Register a revisit and wait until the tuple is known - then bind through.
            this->add_revisit_adv( box$(( SplitTuplePatRevisit { sp, ty.clone(), mv$(leading_tys), mv$(trailing_tys), e.total_size } )) );
        }
        ),
    (Slice,
        const auto& ty = this->get_type(type);
        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Slice, te,
            for(auto& sub : e.sub_patterns)
                this->add_binding(sp, sub, *te.inner );
        )
        else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, te,
            for(auto& sub : e.sub_patterns)
                this->add_binding(sp, sub, *te.inner );
        )
        else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Infer, te,
            auto inner = this->m_ivars.new_ivar_tr();
            for(auto& sub : e.sub_patterns)
                this->add_binding(sp, sub, inner);

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

                void fmt(::std::ostream& os) const override { os << "SlicePatRevisit { " << inner << ", " << type << ", " << size; }
                bool revisit(Context& context) override {
                    const auto& ty = context.get_type(type);
                    TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Slice, te,
                        context.equate_types(sp, *te.inner, inner);
                        return true;
                    )
                    else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, te,
                        if( te.size_val != size ) {
                            ERROR(sp, E0000, "Slice pattern on an array if differing size");
                        }
                        context.equate_types(sp, *te.inner, inner);
                        return true;
                    )
                    else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Infer, te,
                        return false;
                    )
                    else {
                        ERROR(sp, E0000, "Slice pattern on non-array/-slice - " << ty);
                    }
                }
            };
            this->add_revisit_adv( box$(( SlicePatRevisit { sp, mv$(inner), ty.clone(), static_cast<unsigned int>(e.sub_patterns.size()) } )) );
        )
        else {
            ERROR(sp, E0000, "Slice pattern on non-array/-slice - " << ty);
        }
        ),
    (SplitSlice,
        ::HIR::TypeRef  inner;
        unsigned int min_len = e.leading.size() + e.trailing.size();
        const auto& ty = this->get_type(type);
        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Slice, te,
            // Slice - Fetch inner and set new variable also be a slice
            // - TODO: Better new variable handling.
            inner = te.inner->clone();
            if( e.extra_bind.is_valid() ) {
                this->add_binding_inner( sp, e.extra_bind, ty.clone() );
            }
        )
        else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, te,
            inner = te.inner->clone();
            if( te.size_val < min_len ) {
                ERROR(sp, E0000, "Slice pattern on an array smaller than the pattern");
            }
            unsigned extra_len = te.size_val - min_len;

            if( e.extra_bind.is_valid() ) {
                this->add_binding_inner( sp, e.extra_bind, ::HIR::TypeRef::new_array(inner.clone(), extra_len) );
            }
        )
        else if( ty.m_data.is_Infer() ) {
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

                void fmt(::std::ostream& os) const override { os << "SplitSlice inner=" << inner << ", outer=" << type << ", binding="<<var_ty<<", " << min_size; }
                bool revisit(Context& context) override {
                    const auto& ty = context.get_type(this->type);
                    if( ty.m_data.is_Infer() )
                        return false;

                    // Slice - Equate inners
                    TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Slice, te,
                        context.equate_types(this->sp, this->inner, *te.inner);
                        if( this->var_ty != ::HIR::TypeRef() ) {
                            context.equate_types(this->sp, this->var_ty, ty);
                        }
                    )
                    // Array - Equate inners and check size
                    else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, te,
                        context.equate_types(this->sp, this->inner, *te.inner);
                        if( te.size_val < this->min_size ) {
                            ERROR(sp, E0000, "Slice pattern on an array smaller than the pattern");
                        }
                        unsigned extra_len = te.size_val - this->min_size;

                        if( this->var_ty != ::HIR::TypeRef() ) {
                            context.equate_types(this->sp, this->var_ty, ::HIR::TypeRef::new_array(this->inner.clone(), extra_len) );
                        }
                    )
                    else {
                        ERROR(sp, E0000, "Slice pattern on non-array/-slice - " << ty);
                    }
                    return true;
                }
            };
            // Callback
            this->add_revisit_adv( box$(( SplitSlicePatRevisit { sp, inner.clone(), ty.clone(), mv$(var_ty), min_len } )) );
        }
        else {
            ERROR(sp, E0000, "Slice pattern on non-array/-slice - " << ty);
        }

        for(auto& sub : e.leading)
            this->add_binding( sp, sub, inner );
        for(auto& sub : e.trailing)
            this->add_binding( sp, sub, inner );
        ),

    // - Enums/Structs
    (StructValue,
        this->add_ivars_params( e.path.m_params );
        const auto& str = *e.binding;
        assert( str.m_data.is_Unit() );
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)) );
        ),
    (StructTuple,
        this->add_ivars_params( e.path.m_params );
        const auto& str = *e.binding;
        // - assert check from earlier pass
        assert( str.m_data.is_Tuple() );
        const auto& sd = str.m_data.as_Tuple();

        const auto& params = e.path.m_params;
        assert(e.binding);
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)) );

        for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
        {
            /*const*/ auto& sub_pat = e.sub_patterns[i];
            const auto& field_type = sd[i].ent;
            if( monomorphise_type_needed(field_type) ) {
                auto var_ty = monomorphise_type(sp, str.m_params, params,  field_type);
                this->add_binding(sp, sub_pat, var_ty);
            }
            else {
                this->add_binding(sp, sub_pat, field_type);
            }
        }
        ),
    (Struct,
        this->add_ivars_params( e.path.m_params );
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)) );

        if( e.sub_patterns.empty() )
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
                this->add_binding(sp, field_pat.second, field_type_mono);
            }
            else {
                this->add_binding(sp, field_pat.second, field_type);
            }
        }
        ),
    (EnumValue,
        this->add_ivars_params( e.path.m_params );
        {
            auto path = e.path.clone();
            path.m_path.m_components.pop_back();
            this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)) );
        }

        assert(e.binding_ptr);
        const auto& enm = *e.binding_ptr;
        if( enm.m_data.is_Data() )
        {
            //const auto& var = enm.m_data.as_Data()[e.binding_idx];
            //assert(var.is_Value() || var.is_Unit());
        }
        ),
    (EnumTuple,
        this->add_ivars_params( e.path.m_params );
        {
            auto path = e.path.clone();
            path.m_path.m_components.pop_back();

            this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)) );
        }
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
                this->add_binding(sp, e.sub_patterns[i], var_ty);
            }
            else {
                this->add_binding(sp, e.sub_patterns[i], tup_var[i].ent);
            }
        }
        ),
    (EnumStruct,
        this->add_ivars_params( e.path.m_params );
        {
            auto path = e.path.clone();
            path.m_path.m_components.pop_back();

            this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)) );
        }

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
                this->add_binding(sp, field_pat.second, field_type_mono);
            }
            else {
                this->add_binding(sp, field_pat.second, field_type);
            }
        }
        )
    )
}
void Context::equate_types_coerce(const Span& sp, const ::HIR::TypeRef& l, ::HIR::ExprNodeP& node_ptr)
{
    this->m_ivars.get_type(l);
    // - Just record the equality
    this->link_coerce.push_back(Coercion {
        l.clone(), &node_ptr
        });
    DEBUG("equate_types_coerce(" << this->link_coerce.back() << ")");
    this->m_ivars.mark_change();
}
void Context::equate_types_shadow(const Span& sp, const ::HIR::TypeRef& l, bool is_to)
{
    TU_MATCH_DEF(::HIR::TypeRef::Data, (this->get_type(l).m_data), (e),
    (
        // TODO: Shadow sub-types too
        ),
    (Path,
        TU_MATCH_DEF( ::HIR::Path::Data, (e.path.m_data), (pe),
        (
            ),
        (Generic,
            for(const auto& sty : pe.m_params.m_types)
                this->equate_types_shadow(sp, sty, is_to);
            )
        )
        ),
    (Tuple,
        for(const auto& sty : e)
            this->equate_types_shadow(sp, sty, is_to);
        ),
    (Borrow,
        this->equate_types_shadow(sp, *e.inner, is_to);
        ),
    (Array,
        this->equate_types_shadow(sp, *e.inner, is_to);
        ),
    (Slice,
        this->equate_types_shadow(sp, *e.inner, is_to);
        ),
    (Closure,
        for(const auto& aty : e.m_arg_types)
            this->equate_types_shadow(sp, aty, is_to);
        this->equate_types_shadow(sp, *e.m_rettype, is_to);
        ),
    (Infer,
        this->possible_equate_type_disable(e.index, is_to);
        )
    )
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
        sp,
        l.clone(),

        trait.clone(),
        mv$(pp),
        impl_ty.clone(),
        name,
        is_op
        });
    DEBUG("(" << this->link_assoc.back() << ")");
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

void Context::add_var(const Span& sp, unsigned int index, const ::std::string& name, ::HIR::TypeRef type) {
    DEBUG("(" << index << " " << name << " : " << type << ")");
    assert(index != ~0u);
    if( m_bindings.size() <= index )
        m_bindings.resize(index+1);
    if( m_bindings[index].name == "" ) {
        m_bindings[index] = Binding { name, mv$(type) };
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
    CoerceResult check_unsize_tys(Context& context, const Span& sp, const ::HIR::TypeRef& dst_raw, const ::HIR::TypeRef& src_raw, ::HIR::ExprNodeP* node_ptr_ptr=nullptr)
    {
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
            context.possible_equate_type_unsize_to(src.m_data.as_Infer().index, dst);
            context.possible_equate_type_unsize_from(dst.m_data.as_Infer().index, src);
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
            context.possible_equate_type_unsize_from(dep->index, src);
            DEBUG("Dst ivar");
            return CoerceResult::Unknown;
        }
        else if(const auto* sep = src.m_data.opt_Infer())
        {
            if(sep->is_lit() && !dst.m_data.is_TraitObject())
            {
                // Literal to anything other than a trait object must be an equality
                DEBUG("Literal with primitive");
                return CoerceResult::Equality;
            }
            context.possible_equate_type_unsize_to(sep->index, dst);
            DEBUG("Src ivar");
            return CoerceResult::Unknown;
        }
        else
        {
            // Neither side is an ivar, keep going.
        }

        // Array unsize (quicker than going into deref search)
        if(dst.m_data.is_Slice() && src.m_data.is_Array())
        {
            context.equate_types(sp, *dst.m_data.as_Slice().inner, *src.m_data.as_Array().inner);
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
        if(node_ptr_ptr)
        {
            DEBUG("-- Deref coercions");
            auto& node_ptr = *node_ptr_ptr;
            ::HIR::TypeRef  tmp_ty;
            const ::HIR::TypeRef*   out_ty_p = &src;
            unsigned int count = 0;
            ::std::vector< ::HIR::TypeRef>  types;
            while( (out_ty_p = context.m_resolve.autoderef(sp, *out_ty_p, tmp_ty)) )
            {
                const auto& out_ty = context.m_ivars.get_type(*out_ty_p);
                count += 1;

                if( out_ty.m_data.is_Infer() && !out_ty.m_data.as_Infer().is_lit() ) {
                    // Hit a _, so can't keep going
                    break;
                }

                types.push_back( out_ty.clone() );

                // Types aren't equal
                if( context.m_ivars.types_equal(dst, out_ty) == false )
                {
                    // Check if they can be considered equivalent.
                    // - E.g. a fuzzy match, or both are slices/arrays
                    if( dst.m_data.tag() == out_ty.m_data.tag() )
                    {
                        TU_MATCH_DEF( ::HIR::TypeRef::Data, (dst.m_data, out_ty.m_data), (d_e, s_e),
                        (
                            if( dst .compare_with_placeholders(sp, out_ty, context.m_ivars.callback_resolve_infer()) == ::HIR::Compare::Unequal ) {
                                DEBUG("Same tag, but not fuzzy match");
                                continue ;
                            }
                            DEBUG("Same tag and fuzzy match - assuming " << dst << " == " << out_ty);
                            context.equate_types(sp, dst, out_ty);
                            ),
                        (Slice,
                            // Equate!
                            context.equate_types(sp, dst, out_ty);
                            // - Fall through
                            )
                        )
                    }
                    else {
                        continue ;
                    }
                }

                add_coerce_borrow(context, node_ptr, types.back(), [&](auto& node_ptr)->void {
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

                return CoerceResult::Custom;
            }
            // Either ran out of deref, or hit a _
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
                for(size_t i = 0; i < tys_d.size(); i ++)
                {
                    context.equate_types(sp, tys_d[i], tys_s.at(i));
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
                    ImplRef best_impl;
                    unsigned int count = 0;
                    bool found = context.m_resolve.find_trait_impls(sp, trait.m_path, trait.m_params, src, [&](auto impl, auto cmp) {
                        DEBUG("TraitObject coerce from - cmp="<<cmp<<", " << impl);
                        count ++;
                        best_impl = mv$(impl);
                        return cmp == ::HIR::Compare::Equal;
                        });
                    if( count == 0 )
                    {
                        // TODO: Get a better idea of when there won't ever be an applicable impl
                        if( !context.m_ivars.type_contains_ivars(src) ) {
                            ERROR(sp, E0000, "The trait " << dep->m_trait << " is not implemented for " << src);
                        }
                        DEBUG("No impl, but there may eventaully be one");
                        return CoerceResult::Unknown;
                    }
                    if( !found )
                    {
                        if(count > 1)
                        {
                            DEBUG("Defer as there are multiple applicable impls");
                            return CoerceResult::Unknown;
                        }

                        if( best_impl.has_magic_params() ) {
                            DEBUG("Defer as there were magic parameters");
                            return CoerceResult::Unknown;
                        }

                        // TODO: Get a better way of equating these that doesn't require getting copies of the impl's types
                        context.equate_types(sp, src, best_impl.get_impl_type());
                        auto args = best_impl.get_trait_params();
                        assert(trait.m_params.m_types.size() == args.m_types.size());
                        for(unsigned int i = 0; i < trait.m_params.m_types.size(); i ++)
                        {
                            context.equate_types(sp, trait.m_params.m_types[i], args.m_types[i]);
                        }
                        for(const auto& tyb : dep->m_trait.m_type_bounds)
                        {
                            auto ty = best_impl.get_type(tyb.first.c_str());
                            if( ty != ::HIR::TypeRef() )
                            {
                                context.equate_types(sp, tyb.second, ty);
                            }
                            else
                            {
                                // Error? Log? ...
                                DEBUG("Associated type " << tyb.first << " not present in impl, can't equate");
                            }
                        }
                    }
                }

                for(const auto& marker : dep->m_markers)
                {
                    bool found = context.m_resolve.find_trait_impls(sp, marker.m_path, marker.m_params, src, [&](auto impl, auto cmp) {
                        DEBUG("TraitObject coerce from - cmp="<<cmp<<", " << impl);
                        return cmp == ::HIR::Compare::Equal;
                        });
                    // TODO: Allow fuzz and equate same as above?
                    if( !found )
                    {
                        // TODO: Get a better idea of when there won't ever be an applicable impl
                        if( !context.m_ivars.type_contains_ivars(src) ) {
                            ERROR(sp, E0000, "The trait " << marker << " is not implemented for " << src);
                        }
                        return CoerceResult::Unknown;
                    }
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
            bool found = context.m_resolve.find_trait_impls(sp, lang_Unsize, pp, src, [&best_impl,&count](auto impl, auto cmp){
                    DEBUG("[check_unsize_tys] Found impl " << impl << (cmp == ::HIR::Compare::Fuzzy ? " (fuzzy)" : ""));
                    if( impl.more_specific_than(best_impl) )
                    {
                        best_impl = mv$(impl);
                        count ++;
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
                context.equate_types(sp, dst, pp.m_types.at(0));
                return CoerceResult::Unsize;
            }
            else
            {
                // TODO: Fuzzy?
                //context.equate_types(sp, *e.inner, *s_e.inner);
            }
        }

        // Path types
        if( src.m_data.tag() == dst.m_data.tag() )
        {
            TU_MATCH_DEF(::HIR::TypeRef::Data, (src.m_data, dst.m_data), (se, de),
            (
                return CoerceResult::Equality;
                ),
            (Path,
                if( se.binding.tag() == de.binding.tag() )
                {
                    TU_MATCHA( (se.binding, de.binding), (sbe, dbe),
                    (Unbound,
                        // Don't care
                        ),
                    (Opaque,
                        // Handled above in bounded
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
                                return check_unsize_tys(context, sp, idst, isrc, nullptr);
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
                )
            )
        }

        DEBUG("Reached end of check_unsize_tys, return Unknown");
        // TODO: Determine if this unsizing could ever happen.
        return CoerceResult::Unknown;
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
            auto new_src = H::make_pruned(context, dst);
            context.equate_types(sp, src, new_src);
            // TODO: Avoid needless loop return
            return CoerceResult::Unknown;
        }
        if( dst.m_data.is_Infer() && TU_TEST2(src.m_data, Path, .binding, Struct, ->m_struct_markings.coerce_unsized != ::HIR::StructMarkings::Coerce::None) )
        {
            auto new_dst = H::make_pruned(context, src);
            context.equate_types(sp, dst, new_dst);
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
                return check_coerce_tys(context, sp, idst, isrc, nullptr);
            case ::HIR::StructMarkings::Coerce::Pointer:
                DEBUG("Pointer CoerceUnsized");
                return check_unsize_tys(context, sp, idst, isrc, nullptr);
            }
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
                    return CoerceResult::Equality;
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
                        ::HIR::ExprNodeP* npp = node_ptr_ptr;
                        while( auto* p = dynamic_cast< ::HIR::ExprNode_Block*>(&**npp) )
                        {
                            DEBUG("- Propagate to the last node of a _Block");
                            ASSERT_BUG( p->span(), context.m_ivars.types_equal(p->m_res_type, p->m_value_node->m_res_type),
                                "Block and result mismatch - " << context.m_ivars.fmt_type(p->m_res_type) << " != " << context.m_ivars.fmt_type(p->m_value_node->m_res_type));
                            ASSERT_BUG( p->span(), context.m_ivars.types_equal(p->m_res_type, src),
                                "Block and result mismatch - " << context.m_ivars.fmt_type(p->m_res_type) << " != " << context.m_ivars.fmt_type(src)
                                );
                            p->m_res_type = new_type.clone();
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
                        switch( check_unsize_tys(context, sp, *dep->inner, *se.inner, node_ptr_ptr) )
                        {
                        case CoerceResult::Unknown:
                            return CoerceResult::Unknown;
                        case CoerceResult::Custom:
                            return CoerceResult::Custom;
                        case CoerceResult::Equality:
                            context.equate_types(sp, *dep->inner, *se.inner);
                            return CoerceResult::Custom;
                        case CoerceResult::Unsize:
                            DEBUG("- NEWNODE _Unsize " << &*node_ptr << " -> " << dst);
                            auto span = node_ptr->span();
                            node_ptr = NEWNODE( dst.clone(), span, _Unsize,  mv$(node_ptr), dst.clone() );
                            return CoerceResult::Custom;
                        }
                        throw "";
                    }
                    else
                    {
                        TODO(sp, "Borrow strength reduction");
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

        ::HIR::TypeRef  possible_impl_ty;
        ::HIR::PathParams   possible_params;
        ::HIR::TypeRef  output_type;

        struct H {
            static bool type_is_num(const ::HIR::TypeRef& t) {
                TU_MATCH_DEF(::HIR::TypeRef::Data, (t.m_data), (e),
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
            context.equate_types_from_shadow(sp, v.left_ty);
        }

        // Locate applicable trait impl
        unsigned int count = 0;
        DEBUG("Searching for impl " << v.trait << v.params << " for " << context.m_ivars.fmt_type(v.impl_ty));
        ImplRef  best_impl;
        bool found = context.m_resolve.find_trait_impls(sp, v.trait, v.params,  v.impl_ty,
            [&](auto impl, auto cmp) {
                DEBUG("[check_associated] Found cmp=" << cmp << " " << impl);
                if( v.name != "" ) {
                    auto out_ty_o = impl.get_type(v.name.c_str());
                    if( out_ty_o == ::HIR::TypeRef() )
                    {
                        //BUG(sp, "Getting associated type '" << v.name << "' which isn't in " << v.trait << " (" << ty << ")");
                        out_ty_o = ::HIR::TypeRef(::HIR::Path( v.impl_ty.clone(), ::HIR::GenericPath(v.trait, v.params.clone()), v.name, ::HIR::PathParams() ));
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

                    if( possible_impl_ty == ::HIR::TypeRef() ) {
                        DEBUG("[check_associated] First - " << impl);
                        possible_impl_ty = impl.get_impl_type();
                        possible_params = impl.get_trait_params();
                        best_impl = mv$(impl);
                    }
                    // TODO: If there is an existing impl, determine if this is part of the same specialisation tree
                    // - If more specific, replace. If less, ignore.
                    #if 1
                    // NOTE: `overlaps_with` (should be) reflective
                    else if( impl.overlaps_with(context.m_crate, best_impl) )
                    {
                        DEBUG("[check_associated] - Overlaps with existing - " << best_impl);
                        // if not more specific than the existing best, ignore.
                        if( ! impl.more_specific_than(best_impl) )
                        {
                            DEBUG("[check_associated] - Less specific than existing");
                            // NOTE: This picks the _least_ specific impl
                            possible_impl_ty = impl.get_impl_type();
                            possible_params = impl.get_trait_params();
                            best_impl = mv$(impl);
                            count -= 1;
                        }
                        // If the existing best is not more specific than the new one, use the new one
                        else if( ! best_impl.more_specific_than(impl) )
                        {
                            DEBUG("[check_associated] - More specific than existing - " << impl);
                            count -= 1;
                        }
                        else
                        {
                            // Supposedly, `more_specific_than` should be reflexive...
                            DEBUG("[check_associated] > Neither is more specific. Error?");
                        }
                    }
                    else
                    {
                        // Disjoint impls.
                        DEBUG("[check_associated] Disjoint impl -" << impl);
                    }
                    #endif

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
            return true;
        }
        else if( count == 0 ) {
            // No applicable impl
            // - TODO: This should really only fire when there isn't an impl. But it currently fires when _
            DEBUG("No impl of " << v.trait << context.m_ivars.fmt(v.params) << " for " << context.m_ivars.fmt_type(v.impl_ty));

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
            TU_IFLET( ImplRef::Data, best_impl.m_data, TraitImpl, e,
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
            )
            return true;
        }
        else {
            // Multiple possible impls, don't know yet
            DEBUG("Multiple impls");
            return false;
        }
    }

    bool check_ivar_poss(Context& context, unsigned int i, Context::IVarPossible& ivar_ent, bool honour_disable=true)
    {
        static Span _span;
        const auto& sp = _span;

        if( ! ivar_ent.has_rules() ) {
            // No rules, don't do anything (and don't print)
            DEBUG(i << ": No rules");
            return false;
        }

        ::HIR::TypeRef  ty_l_ivar;
        ty_l_ivar.m_data.as_Infer().index = i;
        const auto& ty_l = context.m_ivars.get_type(ty_l_ivar);
        bool allow_unsized = !(i < context.m_ivars_sized.size() ? context.m_ivars_sized.at(i) : false);

        if( ty_l != ty_l_ivar ) {
            DEBUG("- IVar " << i << " had possibilities, but was known to be " << ty_l);
            // Completely clear by reinitialising
            ivar_ent = Context::IVarPossible();
            return false;
        }

        enum class DedupKeep {
            Both,
            Left,
            Right,
        };
        struct H {
            static void dedup_type_list_with(::std::vector< ::HIR::TypeRef>& list, ::std::function<DedupKeep(const ::HIR::TypeRef& l, const ::HIR::TypeRef& r)> cmp) {
                if( list.size() <= 1 )
                    return ;

                for( auto it = list.begin(); it != list.end(); )
                {
                    bool found = false;
                    for( auto it2 = list.begin(); it2 != it; ++ it2 ) {
                        auto action = cmp(*it, *it2);
                        if( action != DedupKeep::Both )
                        {
                            if( action == DedupKeep::Right ) {
                                //DEBUG("Keep " << *it << ", toss " << *it2);
                            }
                            else {
                                ::std::swap(*it2, *it);
                                //DEBUG("Keep " << *it << ", toss " << *it2 << " (swapped)");
                            }
                            found = true;
                            break;
                        }
                    }
                    if( found ) {
                        it = list.erase(it);
                    }
                    else {
                        ++ it;
                    }
                }
            }
            // De-duplicate list (taking into account other ivars)
            // - TODO: Use the direction and do a fuzzy equality based on coercion possibility
            static void dedup_type_list(const Context& context, ::std::vector< ::HIR::TypeRef>& list) {
                dedup_type_list_with(list, [&context](const auto& l, const auto& r){ return H::equal_to(context, l, r) ? DedupKeep::Left : DedupKeep::Both; });
            }

            // Types are equal from the view of being coercion targets
            // - Inequality here means that the targets could coexist in the list (e.g. &[u8; N] and &[u8])
            // - Equality means that they HAVE to be equal (even if they're not currently due to ivars)
            //  - E.g. &mut HashMap<_1> and &mut HashMap<T> would unify
            static bool equal_to(const Context& context, const ::HIR::TypeRef& ia, const ::HIR::TypeRef& ib) {
                const auto& a = context.m_ivars.get_type(ia);
                const auto& b = context.m_ivars.get_type(ib);
                if( a.m_data.tag() != b.m_data.tag() )
                    return false;
                TU_MATCH_DEF(::HIR::TypeRef::Data, (a.m_data, b.m_data), (e_a, e_b),
                (
                    return context.m_ivars.types_equal(a, b);
                    ),
                (Borrow,
                    if( e_a.type != e_b.type )
                        return false;
                    const auto& ia = context.m_ivars.get_type(*e_a.inner);
                    const auto& ib = context.m_ivars.get_type(*e_b.inner);
                    if( ia.m_data.tag() != ib.m_data.tag() )
                        return false;
                    TU_MATCH_DEF(::HIR::TypeRef::Data, (ia.m_data, ib.m_data), (e_ia, e_ib),
                    (
                        return context.m_ivars.types_equal(ia, ib);
                        ),
                    (Infer,
                        return false;
                        ),
                    (Path,
                        if( e_ia.binding.tag() != e_ib.binding.tag() )
                            return false;
                        TU_MATCHA( (e_ia.binding, e_ib.binding), (pbe_a, pbe_b),
                        (Unbound,   return false; ),
                        (Opaque,
                            // TODO: Check bounds?
                            return false;
                            ),
                        (Struct,
                            if(pbe_a != pbe_b)  return false;
                            if( !pbe_a->m_struct_markings.can_unsize )
                                return true;
                            // It _could_ unsize, so let it coexist
                            return false;
                            ),
                        (Union,
                            return false;
                            ),
                        (Enum,
                            return false;
                            )
                        )
                        ),
                    (Slice,
                        const auto& ia2 = context.m_ivars.get_type(*e_ia.inner);
                        const auto& ib2 = context.m_ivars.get_type(*e_ib.inner);
                        if(ia2.m_data.is_Infer() || ib2.m_data.is_Infer())
                            return true;
                        return context.m_ivars.types_equal(ia2, ib2);
                        )
                    )
                    ),
                (Pointer,
                    if( e_a.type != e_b.type )
                        return false;
                    // TODO: Rules are subtly different when coercing from a pointer?
                    const auto& ia2 = context.m_ivars.get_type(*e_a.inner);
                    const auto& ib2 = context.m_ivars.get_type(*e_b.inner);
                    if(ia2.m_data.is_Infer() || ib2.m_data.is_Infer())
                        return true;
                    return context.m_ivars.types_equal(ia2, ib2);
                    )
                )
                //
                return context.m_ivars.types_equal(a, b);
            }
            // Types are equal from the view of being coercion sources
            static bool equal_from(const Context& context, const ::HIR::TypeRef& a, const ::HIR::TypeRef& b) {
                return context.m_ivars.types_equal(a, b);
            }

            // TODO: `can_unsize_to`
            static bool can_coerce_to(const Context& context, const ::HIR::TypeRef& dst, const ::HIR::TypeRef& src) {
                if( dst.m_data.is_Infer() )
                    return false;
                if( src.m_data.is_Infer() )
                    return false;

                if( dst.m_data.is_Borrow() && src.m_data.is_Borrow() ) {
                    const auto& d_e = dst.m_data.as_Borrow();
                    const auto& s_e = src.m_data.as_Borrow();

                    // Higher = more specific (e.g. Unique > Shared)
                    if( s_e.type < d_e.type ) {
                        return false;
                    }
                    else if( s_e.type == d_e.type ) {
                        // Check relationship
                        // - 1. Deref chain.
                        // - 2. Trait object?
                    }
                    else {
                        return context.m_ivars.types_equal(*s_e.inner, *d_e.inner);
                    }
                }

                if( dst.m_data.is_Pointer() && src.m_data.is_Pointer() ) {
                    const auto& d_e = dst.m_data.as_Pointer();
                    const auto& s_e = src.m_data.as_Pointer();
                    // Higher = more specific (e.g. Unique > Shared)
                    if( s_e.type < d_e.type ) {
                        return false;
                    }
                    else if( s_e.type == d_e.type ) {
                        // Check relationship
                        // - 1. Deref chain.
                        // - 2. Trait object?
                    }
                    else {
                        return context.m_ivars.types_equal(*s_e.inner, *d_e.inner);
                    }
                }

                if( dst.m_data.is_Pointer() && src.m_data.is_Borrow() ) {
                    const auto& d_e = dst.m_data.as_Pointer();
                    const auto& s_e = src.m_data.as_Borrow();
                    if( s_e.type == d_e.type ) {
                        return context.m_ivars.types_equal(*s_e.inner, *d_e.inner);
                    }
                }
                return false;
            }

            // Returns true if the `src` concretely cannot coerce to `dst`
            static bool cannot_coerce_to(const Context& context, const ::HIR::TypeRef& dst, const ::HIR::TypeRef& src) {
                TU_IFLET( ::HIR::TypeRef::Data, src.m_data, Borrow, se,
                    TU_IFLET( ::HIR::TypeRef::Data, dst.m_data, Borrow, de,
                    )
                    else TU_IFLET( ::HIR::TypeRef::Data, dst.m_data, Pointer, de,
                    )
                    else {
                        return true;
                    }
                )
                return false;
            }

            static const ::HIR::TypeRef* find_lowest_type(const Context& context, const ::std::vector< ::HIR::TypeRef>& list)
            {
                // 1. Locate types that cannot coerce to anything
                // - &TraitObject and &[T] are the main pair
                for(const auto& ty : list) {
                    TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Borrow, e,
                        TU_MATCH_DEF(::HIR::TypeRef::Data, (e.inner->m_data), (e2),
                        (
                            ),
                        (Slice,
                            return &ty;
                            ),
                        (TraitObject,
                            return &ty;
                            )
                        )
                    )
                }

                // 2. Search the list for a type that is a valid coercion target for all other types in the list
                // - NOTE: Ivars return `false` nomatter what order
                const auto* cur_type = &list[0];
                for(const auto& ty : list) {
                    // If ty can be coerced to the current type
                    if( H::can_coerce_to(context, *cur_type, ty) ) {
                        // - Keep current type
                    }
                    else if( H::can_coerce_to(context, ty, *cur_type) ) {
                        cur_type = &ty;
                    }
                    else {
                        // Error? Give up.
                        cur_type = nullptr;
                        break;
                    }
                }
                if( cur_type ) {
                    // TODO: Replace
                    //return cur_type;
                }

                return nullptr;
            }

            /// Returns true if `dst` is found when dereferencing `src`
            static bool type_derefs_from(const Span& sp, const Context& context, const ::HIR::TypeRef& dst, const ::HIR::TypeRef& src)
            {
                ::HIR::TypeRef  tmp;
                const ::HIR::TypeRef* ty = &src;
                do
                {
                    if( context.m_ivars.types_equal(*ty, dst) )
                        return true;
                } while( (ty = context.m_resolve.autoderef(sp, *ty, tmp)) );
                return false;
            }

            static ::std::vector<::HIR::TypeRef>& merge_lists(const Context& context, ::std::vector<::HIR::TypeRef>& list_a, ::std::vector<::HIR::TypeRef>& list_b, ::std::vector<::HIR::TypeRef>& out)
            {
                if( list_a.size() == 0 )
                    return list_b;
                else if( list_b.size() == 0 )
                    return list_a;
                else {
                    for(const auto& t : list_a) {
                        out.push_back( t.clone() );
                    }
                    for(const auto& t : list_b ) {
                        out.push_back( t.clone() );
                    }
                    H::dedup_type_list(context, out);
                    return out;
                }
            }
        };

        // If this type has an infer class active, don't allw a non-primitive to coerce over it.
        if( ty_l.m_data.as_Infer().is_lit() )
        {
            DEBUG("Literal checks");
            // TODO: Actively search possibility list for the real type.
            for(const auto& ty : ivar_ent.types_coerce_to)
                if( ty.m_data.is_Primitive() ) {
                    context.equate_types(sp, ty_l, ty);
                    return true;
                }
            for(const auto& ty : ivar_ent.types_unsize_to)
                if( ty.m_data.is_Primitive() ) {
                    context.equate_types(sp, ty_l, ty);
                    return true;
                }
            for(const auto& ty : ivar_ent.types_coerce_from)
                if( ty.m_data.is_Primitive() ) {
                    context.equate_types(sp, ty_l, ty);
                    return true;
                }
            for(const auto& ty : ivar_ent.types_unsize_from)
                if( ty.m_data.is_Primitive() ) {
                    context.equate_types(sp, ty_l, ty);
                    return true;
                }
            return false;
        }

        if( honour_disable && (ivar_ent.force_no_to || ivar_ent.force_no_from) )
        {
            DEBUG("- IVar " << ty_l << " is forced unknown");
            return false;
        }
        else
        {
            TRACE_FUNCTION_F(i);


            // TODO: Dedup based on context?
            // - The dedup should probably be aware of the way the types are used (for coercions).
            H::dedup_type_list(context, ivar_ent.types_coerce_to);
            H::dedup_type_list(context, ivar_ent.types_unsize_to);
            H::dedup_type_list(context, ivar_ent.types_coerce_from);
            H::dedup_type_list(context, ivar_ent.types_unsize_from);

            #if 0
            // If there is a default type compatible with all possibilities, use that.
            if( ivar_ent.types_default.size() > 0 ) {
                // TODO: Should multiple options be valid?
                ASSERT_BUG(Span(), ivar_ent.types_def.size() == 1, "TODO: Multiple default types for an ivar - " << ivar_ent.types_def);
            }
            #endif

            if( ivar_ent.types_coerce_from.size() == 0 && ivar_ent.types_coerce_to.size() == 0
             && ivar_ent.types_unsize_from.size() == 0 && ivar_ent.types_unsize_to.size() == 0
                )
            {
                DEBUG("-- No known options for " << ty_l);
                return false;
            }
            DEBUG("-- " << ty_l << " FROM=Coerce:{" << ivar_ent.types_coerce_from << "} / Unsize:{" << ivar_ent.types_unsize_from << "},"
                << " TO=Coerce:{" << ivar_ent.types_coerce_to << "} / Unsize:{" << ivar_ent.types_unsize_to << "}");

            // Find an entry in the `types_unsize_from` list that all other entries can unsize to
            H::dedup_type_list_with(ivar_ent.types_unsize_from, [&](const auto& l, const auto& r) {
                if( l.m_data.is_Infer() || r.m_data.is_Infer() )
                    return DedupKeep::Both;

                // Check for fuzzy equality of types, and keep only one
                // TODO: Ensure that whatever ivar differs can't be different (i.e. it wouldn't change the unsize/coerce)
                // TODO: Use `check_unsize_tys` instead
                if( l.compare_with_placeholders(sp, r, context.m_ivars.callback_resolve_infer()) != ::HIR::Compare::Unequal )
                {
                    DEBUG("Possible match, keep left");
                    return DedupKeep::Left;
                }

                // &T and T
                TU_IFLET( ::HIR::TypeRef::Data, l.m_data, Borrow, le,
                    TU_IFLET( ::HIR::TypeRef::Data, r.m_data, Borrow, re,
                    )
                    else {
                        // if *le.inner == r, return DedupKeep::Right
                        if( context.m_ivars.types_equal(*le.inner, r) )
                            return DedupKeep::Right;
                    }
                )
                else {
                    TU_IFLET( ::HIR::TypeRef::Data, r.m_data, Borrow, re,
                        // if *re.inner == l, return DedupKeep::Left
                        if( context.m_ivars.types_equal(*re.inner, l) )
                            return DedupKeep::Left;
                    )
                }
                return DedupKeep::Both;
                });
            // Find an entry in the `types_coerce_from` list that all other entries can coerce to
            H::dedup_type_list_with(ivar_ent.types_coerce_from, [&](const auto& l, const auto& r) {
                if( l.m_data.is_Infer() || r.m_data.is_Infer() )
                    return DedupKeep::Both;

                // Check for fuzzy equality of types, and keep only one
                // TODO: Ensure that whatever ivar differs can't be different (i.e. it wouldn't change the unsize/coerce)
                // TODO: Use `check_coerce_tys` instead
                if( l.compare_with_placeholders(sp, r, context.m_ivars.callback_resolve_infer()) != ::HIR::Compare::Unequal )
                {
                    DEBUG("Possible match, keep left");
                    return DedupKeep::Left;
                }

                if( l.m_data.is_Borrow() )
                {
                    const auto& le = l.m_data.as_Borrow();
                    ASSERT_BUG(sp, r.m_data.is_Borrow() || r.m_data.is_Pointer(), "Coerce source for borrow isn't a borrow/pointert - " << r);
                    const auto re_borrow_type = r.m_data.is_Borrow() ? r.m_data.as_Borrow().type : r.m_data.as_Pointer().type;
                    const auto& re_inner = r.m_data.is_Borrow() ? r.m_data.as_Borrow().inner : r.m_data.as_Pointer().inner;

                    if( le.type < re_borrow_type ) {
                        return DedupKeep::Left;
                    }
                    else if( le.type > re_borrow_type ) {
                        return DedupKeep::Right;
                    }
                    else {
                    }

                    // Dereference `*re.inner` until it isn't possible or it equals `*le.inner`
                    // - Repeat going the other direction.
                    if( H::type_derefs_from(sp, context, *le.inner, *re_inner) )
                        return DedupKeep::Left;
                    if( H::type_derefs_from(sp, context, *re_inner, *le.inner) )
                        return DedupKeep::Right;
                }
                return DedupKeep::Both;
                });


            // If there's one option for both desination types, and nothing for the source ...
            if( ivar_ent.types_coerce_to.size() == 1 && ivar_ent.types_unsize_to.size() == 1 && ivar_ent.types_coerce_from.empty() && ivar_ent.types_unsize_from.empty() )
            {
                // And the coercion can unsize to the unsize, pick the coercion (it's valid, the other way around isn't)
                // TODO: Use a `can_unsize_to` functon instead (that handles Unsize as well as Deref)
                if( H::type_derefs_from(sp, context, ivar_ent.types_unsize_to[0], ivar_ent.types_coerce_to[0]) )
                {
                    const auto& new_ty = ivar_ent.types_coerce_to[0];
                    DEBUG("- IVar " << ty_l << " = " << new_ty << " (unsize to)");
                    context.equate_types(sp, ty_l, new_ty);
                    return true;
                }
            }
            #if 0
            // If there's only one coerce and unsize from, ...
            if( ivar_ent.types_coerce_from.size() == 1 && ivar_ent.types_unsize_from.size() == 1 && ivar_ent.types_coerce_to.empty() && ivar_ent.types_unsize_to.empty() )
            {
                // and if the coerce can unsize to the unsize target.
                if( H::can_coerce_to(context, ivar_ent.types_unsize_from[0], ivar_ent.types_coerce_from[0]) )
                {
                    const auto& new_ty = ivar_ent.types_coerce_from[0];
                    DEBUG("- IVar " << ty_l << " = " << new_ty << " (coerce from)");
                    context.equate_types(sp, ty_l, new_ty);
                    return true;
                }
            }
            #endif

            //

            // HACK: Merge into a single lists
            ::std::vector< ::HIR::TypeRef>  types_from_o;
            auto& types_from = H::merge_lists(context, ivar_ent.types_coerce_from, ivar_ent.types_unsize_from,  types_from_o);
            ::std::vector< ::HIR::TypeRef>  types_to_o;
            auto& types_to   = H::merge_lists(context, ivar_ent.types_coerce_to  , ivar_ent.types_unsize_to  ,  types_to_o  );
            DEBUG("   " << ty_l << " FROM={" << types_from << "}, TO={" << types_to << "}");

            // TODO: If there is only a single option and it's from an Unsize, is it valid?

            // TODO: Loop both lists and if there's T<_{int}> and T<uN/iN> unify those.

            // Same type on both sides, pick it.
            if( types_from == types_to && types_from.size() == 1 ) {
                const auto& new_ty = types_from[0];
                DEBUG("- IVar " << ty_l << " = " << new_ty << " (only)");
                context.equate_types(sp, ty_l, new_ty);
                return true;
            }

            // Eliminate possibilities that don't fit known constraints
            if( types_to.size() > 0 && types_from.size() > 0 )
            {
                // TODO: Search `types_to` too
                for(auto it = types_from.begin(); it != types_from.end(); )
                {
                    bool remove = false;
                    const auto& new_ty = context.get_type(*it);
                    if( !new_ty.m_data.is_Infer() )
                    {
                        for(const auto& bound : context.link_assoc)
                        {
                            if( bound.impl_ty != ty_l )
                                continue ;

                            // TODO: Monomorphise this type replacing mentions of the current ivar with the replacement?

                            // Search for any trait impl that could match this,
                            bool has = context.m_resolve.find_trait_impls(sp, bound.trait, bound.params, new_ty, [&](const auto , auto){return true;});
                            if( !has ) {
                                // If none was found, remove from the possibility list
                                remove = true;
                                DEBUG("Remove possibility " << new_ty << " because it failed a bound");
                                break ;
                            }
                        }

                        if( !remove && !allow_unsized )
                        {
                            if( context.m_resolve.type_is_sized(sp, new_ty) == ::HIR::Compare::Unequal )
                            {
                                remove = true;
                                DEBUG("Remove possibility " << new_ty << " because it isn't Sized");
                            }
                        }
                    }

                    if( remove ) {
                        it = types_from.erase(it);
                    }
                    else {
                        ++it;
                    }
                }

                // Eliminate `to` types that can't be coerced from `from` types
                if(types_from.size() > 0)
                for(auto it = types_to.begin(); it != types_to.end(); )
                {
                    if( it->m_data.is_Infer() ) {
                        ++ it;
                        continue;
                    }
                    bool remove = false;

                    for(const auto& src : types_from)
                    {
                        if( H::cannot_coerce_to(context, *it, src) ) {
                            remove = true;
                            DEBUG("Remove target type " << *it << " because it cannot be created from a source type");
                            break;
                        }
                    }

                    if( !remove && !allow_unsized )
                    {
                        if( context.m_resolve.type_is_sized(sp, *it) == ::HIR::Compare::Unequal )
                        {
                            remove = true;
                            DEBUG("Remove possibility " << *it << " because it isn't Sized");
                        }
                    }

                    if( remove ) {
                        it = types_to.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            }

            // De-duplicate lists (after unification) using strict equality
            H::dedup_type_list_with(types_from, [&](const auto& l, const auto& r) {
                    return context.m_ivars.types_equal(l, r) ? DedupKeep::Left : DedupKeep::Both;
                    });
            H::dedup_type_list_with(types_to, [&](const auto& l, const auto& r) {
                    return context.m_ivars.types_equal(l, r) ? DedupKeep::Left : DedupKeep::Both;
                    });
            
            // If there is a common type in both lists, use it
            for(const auto& t1 : types_from)
            {
                for(const auto& t2 : types_to)
                {
                    if(t1 == t2) {
                        DEBUG("- IVar " << ty_l << " = " << t1 << " (present in both lists)");
                        context.equate_types(sp, ty_l, t1);
                        return true;
                    }
                }
            }

            // Prefer cases where this type is being created from a known type
            if( types_from.size() == 1 || types_to.size() == 1 ) {
                const char* list_name = (types_from.size() ? "from" : "to");
                const ::HIR::TypeRef& ty_r = (types_from.size() == 1 ? types_from[0] : types_to[0]);
                // Only one possibility
                // - If it would ivar unify, don't if the target has guessing disabled
                if( const auto* e = ty_r.m_data.opt_Infer() )
                {
                    if( e->index < context.possible_ivar_vals.size() )
                    {
                        const auto& p = context.possible_ivar_vals[e->index];
                        if( honour_disable && (p.force_no_to || p.force_no_from) )
                        {
                            DEBUG("- IVar " << ty_l << " ?= " << ty_r << " (single " << list_name << ", rhs disabled)");
                            return false;
                        }
                    }
                }
                // If there are from options, AND the to option is an Unsize
                if( types_from.size() > 1 && ivar_ent.types_unsize_to.size() == 1 && ivar_ent.types_coerce_to.size() == 0
                        && ::std::any_of(types_from.begin(), types_from.end(), [](const auto&x){ return x.m_data.is_Infer(); }) ) {
                    DEBUG("- IVar " << ty_l << " != " << ty_r << " (single " << list_name << ", but was unsize and from has ivars)");
                    return false;
                }

                DEBUG("- IVar " << ty_l << " = " << ty_r << " (single " << list_name << ")");
                context.equate_types(sp, ty_l, ty_r);
                return true;
            }
            else {
                DEBUG("- IVar " << ty_l << " not concretely known {" << types_from << "} and {" << types_to << "}" );

                // If one side is completely unknown, pick the most liberal of the other side
                if( types_to.size() == 0 && types_from.size() > 0 )
                {
                    // Search for the lowest-level source type (e.g. &[T])
                    const auto* lowest_type = H::find_lowest_type(context, types_from);
                    if( lowest_type )
                    {
                        const ::HIR::TypeRef& ty_r = *lowest_type;
                        DEBUG("- IVar " << ty_l << " = " << ty_r << " (from, lowest)");
                        context.equate_types(sp, ty_l, ty_r);
                        return true;
                    }
                }
                else if( types_to.size() > 0 && types_from.size() == 0 )
                {
                    // TODO: Get highest-level target type
                }
                else
                {
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
    Context context { ms.m_crate, ms.m_impl_generics, ms.m_item_generics };

    for( auto& arg : args ) {
        context.add_binding( Span(), arg.first, arg.second );
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

        ExprVisitor_Enum    visitor(context, ms.m_traits, result_type);
        context.add_ivars(root_ptr->m_res_type);
        root_ptr->visit(visitor);

        DEBUG("Return type = " << new_res_ty);
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
            auto& src_ty = (**ent.right_node_ptr).m_res_type;
            //src_ty = context.m_resolve.expand_associated_types( (*ent.right_node_ptr)->span(), mv$(src_ty) );
            ent.left_ty = context.m_resolve.expand_associated_types( (*ent.right_node_ptr)->span(), mv$(ent.left_ty) );
            if( check_coerce(context, ent) )
            {
                DEBUG("- Consumed coercion " << ent.left_ty << " := " << src_ty);

                // If this isn't the last item in the list
                if( i != context.link_coerce.size() - 1 )
                {
                    // Swap with the last item
                    context.link_coerce[i] = mv$(context.link_coerce.back());
                }
                // Remove the last item.
                context.link_coerce.pop_back();
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
        for( auto it = context.adv_revisits.begin(); it != context.adv_revisits.end(); )
        {
            auto& ent = **it;
            if( ent.revisit(context) ) {
                it = context.adv_revisits.erase(it);
            }
            else {
                ++ it;
            }
        }

        // If nothing changed this pass, apply ivar possibilities
        // - This essentially forces coercions not to happen.
        if( ! context.m_ivars.peek_changed() )
        {
            // Check the possible equations
            DEBUG("--- IVar possibilities");
            // NOTE: Ordering is a hack for libgit2
            for(unsigned int i = context.possible_ivar_vals.size(); i --; )
            {
                if( check_ivar_poss(context, i, context.possible_ivar_vals[i]) ) {
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
                }
                else {
                    //assert( !context.m_ivars.peek_changed() );
                }
            }
        } // `if peek_changed` (ivar possibilities)

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

        // If nothing has changed, run check_ivar_poss again but ignoring the 'disable' flag
        if( !context.m_ivars.peek_changed() )
        {
            // Check the possible equations
            DEBUG("--- IVar possibilities (fallback)");
            // NOTE: Ordering is a hack for libgit2
            for(unsigned int i = context.possible_ivar_vals.size(); i --; )
            {
                if( check_ivar_poss(context, i, context.possible_ivar_vals[i], /*honour_disable=*/false) ) {
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
                }
                else {
                    //assert( !context.m_ivars.peek_changed() );
                }
            }
        } // `if peek_changed` (ivar possibilities #2)

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
        for(const auto& coercion : context.link_coerce)
        {
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
            if( const auto* np = dynamic_cast<::HIR::ExprNode_CallMethod*>(node) )
            {
                WARNING(sp, W0000, "Spare Rule - {" << context.m_ivars.fmt_type(np->m_value->m_res_type) << "}." << np->m_method);
            }
            else
            {
            }
        }
        BUG(root_ptr->span(), "Spare rules left after typecheck stabilised");
    }

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
}

