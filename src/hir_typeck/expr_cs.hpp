/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/expr_cs.cpp
 * - "Constraint Solver" type inferrence
 */
#pragma once
#include <hir/type_ref.hpp>
#include <hir/expr_ptr.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/expr_visit.hpp>
#include <span.hpp>
#include "helpers.hpp"

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
        // Strong disable (depends on a trait impl)
        bool force_disable = false;
        // 
        bool force_no_to = false;
        bool force_no_from = false;
        struct CoerceTy {
            enum Op {
                Coercion,
                Unsizing,
            } op;
            ::HIR::TypeRef  ty;

            CoerceTy(::HIR::TypeRef ty, bool is_coerce):
                op(is_coerce ? Coercion : Unsizing),
                ty(ty)
            {
            }
        };
        // Target types for coercion/unsizing (these types are known to exist in the function)
        ::std::vector<CoerceTy> types_coerce_to;
        // Source types for coercion/unsizing (these types are known to exist in the function)
        ::std::vector<CoerceTy> types_coerce_from;
        // Possible default types (from generic defaults)
        ::std::set<::HIR::TypeRef>   types_default;

        // Possible types from trait impls (may introduce new types)
        // - This is union of all input bounds
        bool    has_bounded = false;
        /// If the bounds include this ivar, mark differently (permits any incoming type, but types can be removed)
        /// - If an existing type isn't in the incoming set, it is removed
        /// - But any type in an incoming set is accepted (even if it doesn't already exist)
        bool    bounds_include_self = false;
        ::std::vector<::HIR::TypeRef>   bounded;

        void reset() {
            auto tmp = mv$(this->types_default);
            *this = IVarPossible();
            this->types_default = mv$(tmp);
        }
        bool has_rules() const {
            if( force_disable )
                return true;
            if( force_no_to   || !types_coerce_to.empty() )
                return true;
            if( force_no_from || !types_coerce_from.empty() )
                return true;
            //if( !types_default.empty() )
            //    return true;
            if( has_bounded )
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

    Context(
        const ::HIR::Crate& crate,
        const ::HIR::GenericParams* impl_params,
        const ::HIR::GenericParams* item_params,
        const ::HIR::SimplePath& mod_path,
        const ::HIR::GenericPath* current_trait
        )
        :m_crate(crate)
        ,m_resolve(m_ivars, crate, impl_params, item_params, mod_path, current_trait)
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

    /// Get the `possible_ivar_vals` entry for the given ivar index
    /// Returns `nullptr` if the ivar is already known
    IVarPossible* get_ivar_possibilities(const Span& sp, unsigned int ivar_index);

    enum class IvarUnknownType {
        /// Coercion to an unknown type (disables 
        To,
        /// Coercion from an unknown type
        From,
        /// Bounded to be an unknown type (a strong disable)
        Bound,
    };
    /// Type is unknown (e.g. no used/results from a trait impl that can't be looked up)
    void possible_equate_type_unknown(const Span& sp, const ::HIR::TypeRef& ty, IvarUnknownType src_ty);
    /// Type must be one of the provided set
    void possible_equate_type_bounds(const Span& sp, const ::HIR::TypeRef& ty, ::std::vector<::HIR::TypeRef> t);

    // ----
    // IVar possibilties
    // ----

    enum class PossibleTypeSource
    {
        CoerceTo,   //!< IVar must coerce to this type
        UnsizeTo,   //!< IVar must unsize to this type
        CoerceFrom,   //!< IVar must coerce from this type
        UnsizeFrom,   //!< IVar must unsize from this type
    };

    /// Default type
    //void possible_equate_ivar_def(unsigned int ivar_index, const ::HIR::TypeRef& t);

    /// Record that the IVar may be this type (and what the source is)
    void possible_equate_ivar(const Span& sp, unsigned int ivar_index, const ::HIR::TypeRef& t, PossibleTypeSource src_ty);
    /// Add a possible type for an ivar (which is used if only one possibility meets available bounds)
    void possible_equate_ivar_bounds(const Span& sp, unsigned int ivar_index, ::std::vector<::HIR::TypeRef> t);
    /// Record that the IVar is equated to an unknown type
    void possible_equate_ivar_unknown(const Span& sp, unsigned int ivar_index, IvarUnknownType src_ty);

    // ----
    // Patterns and bindings
    // ----

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

namespace typecheck
{
    extern bool visit_call_populate_cache(Context& context, const Span& sp, ::HIR::Path& path, ::HIR::ExprCallCache& cache) __attribute__((warn_unused_result));
}

extern void Typecheck_Code_CS__EnumerateRules(Context& context, const typeck::ModuleState& ms, t_args& args, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr, ::std::unique_ptr<HIR::ExprNode>& root_ptr);
