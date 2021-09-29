/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/hir_ops.cpp
 * - Complex operations on the HIR
 */
#include "hir.hpp"
#include <algorithm>
#include <hir_typeck/common.hpp>
#include <hir_typeck/expr_visit.hpp>    // for invoking typecheck
#include "item_path.hpp"
#include "expr_state.hpp"
#include <hir_conv/main_bindings.hpp>
#include <hir_expand/main_bindings.hpp>
#include <mir/main_bindings.hpp>
#include <trans/target.hpp>

namespace {
    bool matches_genericpath(const ::HIR::GenericPath& left, const ::HIR::GenericPath& right, ::HIR::t_cb_resolve_type ty_res, bool expand_generic);

    bool matches_constgeneric(const ::HIR::ConstGeneric& left, const ::HIR::ConstGeneric& right, ::HIR::t_cb_resolve_type ty_res, bool expand_generic)
    {
        assert( !left.is_Infer() );
        if(right.is_Infer())
        {
            return true;
        }
        if(right.is_Generic())
        {
            return left.is_Generic();
        }

        if(left.is_Generic()) {
            //DEBUG("> Generic left, success");
            return true;
        }

        if( left.tag() != right.tag() ) {
            //DEBUG("> Tag mismatch, failure");
            return false;
        }

        return left == right;
    }

    bool matches_type_int(const ::HIR::TypeRef& left,  const ::HIR::TypeRef& right_in, ::HIR::t_cb_resolve_type ty_res, bool expand_generic)
    {
        assert(! left.data().is_Infer() );
        const auto& right = (right_in.data().is_Infer() ? ty_res(right_in) : right_in);
        if( right_in.data().is_Generic() )
            expand_generic = false;

        //DEBUG("left = " << left << ", right = " << right);

        // TODO: What indicates what out of ty_res?

        if( const auto* re = right.data().opt_Infer() )
        {
            //DEBUG("left = " << left << ", right = " << right);
            switch(re->ty_class)
            {
            case ::HIR::InferClass::None:
                //return left.m_data.is_Generic();
                return true;
            case ::HIR::InferClass::Integer:
                if(const auto* le = left.data().opt_Primitive()) {
                    return is_integer(*le);
                }
                else {
                    return left.data().is_Generic();
                }
                break;
            case ::HIR::InferClass::Float:
                if(const auto* le = left.data().opt_Primitive()) {
                    return is_float(*le);
                }
                else {
                    return left.data().is_Generic();
                }
                break;
            }
            throw "";
        }

        // A local generic could match anything, leave that up to the caller
        if( left.data().is_Generic() ) {
            DEBUG("> Generic left, success");
            return true;
        }
        // A local UfcsKnown can only be becuase it couldn't be expanded earlier, assume it could match
        if( left.data().is_Path() && left.data().as_Path().path.m_data.is_UfcsKnown() ) {
            // True?
            //DEBUG("> UFCS Unknown left, success");
            return true;
        }

        // If the RHS (provided) is generic, it can only match if it binds to a local type parameter
        if( right.data().is_Generic() ) {
            // TODO: This is handled above?
            //DEBUG("> Generic right, only if left generic");
            return left.data().is_Generic();
        }
        // If the RHS (provided) is generic, it can only match if it binds to a local type parameter
        if( TU_TEST1(right.data(), Path, .binding.is_Unbound()) ) {
            //DEBUG("> UFCS Unknown right, fuzzy");
            return true;
        }

        if( left.data().tag() != right.data().tag() ) {
            //DEBUG("> Tag mismatch, failure");
            return false;
        }
        TU_MATCH_HDRA( (left.data(), right.data()), {)
        TU_ARMA(Infer, le, re) throw "infer";
        TU_ARMA(Diverge, le, re) return true;
        TU_ARMA(Primitive, le, re) return le == re;
        TU_ARMA(Path, le, re) {
            if( le.path.m_data.tag() != re.path.m_data.tag() )
                return false;
            TU_MATCH_DEF(::HIR::Path::Data, (le.path.m_data, re.path.m_data), (ple, pre),
            (
                return false;
                ),
            (Generic,
                return matches_genericpath( ple, pre, ty_res, expand_generic);
                )
            )
            }
        TU_ARMA(Generic, le, re) {
            throw "";
            }
        TU_ARMA(TraitObject, le, re) {
            if( !matches_genericpath(le.m_trait.m_path, re.m_trait.m_path, ty_res, expand_generic) )
                return false;
            if( le.m_markers.size() != re.m_markers.size() )
                return false;
            for(unsigned int i = 0; i < le.m_markers.size(); i ++)
            {
                const auto& lm = le.m_markers[i];
                const auto& rm = re.m_markers[i];
                if( !matches_genericpath(lm, rm, ty_res, expand_generic) )
                    return false;
            }
            return true;
            }
        TU_ARMA(ErasedType, le, re) {
            throw "Unexpected ErasedType in matches_type_int";
            }
        TU_ARMA(Array, le, re) {
            if( ! matches_type_int(le.inner, re.inner, ty_res, expand_generic) )
                return false;
            if(le.size.is_Unevaluated()) {
                // If the left is a generic, allow it.
                if( re.size.is_Unevaluated()) {
                    if( !matches_constgeneric(le.size.as_Unevaluated(), re.size.as_Unevaluated(), ty_res, expand_generic) )
                        return false;
                }
                else {
                    if( le.size.as_Unevaluated().is_Generic() ) {
                    }
                    else {
                        TODO(Span(), "Match an unevaluated array with an evaluated one - " << le.size << " " << re.size);
                    }
                }
            }
            else {
                // TODO: Other unresolved sizes?
                if( le.size != re.size )
                    return false;
            }
            return true;
            }
        TU_ARMA(Slice, le, re) {
            return matches_type_int(le.inner, re.inner, ty_res, expand_generic);
            }
        TU_ARMA(Tuple, le, re) {
            if( le.size() != re.size() )
                return false;
            for( unsigned int i = 0; i < le.size(); i ++ )
                if( !matches_type_int(le[i], re[i], ty_res, expand_generic) )
                    return false;
            return true;
            }
        TU_ARMA(Borrow, le, re) {
            if( le.type != re.type )
                return false;
            return matches_type_int(le.inner, re.inner, ty_res, expand_generic);
            }
        TU_ARMA(Pointer, le, re) {
            if( le.type != re.type )
                return false;
            return matches_type_int(le.inner, re.inner, ty_res, expand_generic);
            }
        TU_ARMA(Function, le, re) {
            if( le.is_unsafe != re.is_unsafe )
                return false;
            if( le.m_abi != re.m_abi )
                return false;
            if( le.m_arg_types.size() != re.m_arg_types.size() )
                return false;
            for( unsigned int i = 0; i < le.m_arg_types.size(); i ++ )
                if( !matches_type_int(le.m_arg_types[i], re.m_arg_types[i], ty_res, expand_generic) )
                    return false;
            return matches_type_int(le.m_rettype, re.m_rettype, ty_res, expand_generic);
            }
        TU_ARMA(Closure, le, re) {
            return le.node == re.node;
            }
        TU_ARMA(Generator, le, re) {
            return le.node == re.node;
            }
        }
        return false;
    }
    bool matches_genericpath(const ::HIR::GenericPath& left, const ::HIR::GenericPath& right, ::HIR::t_cb_resolve_type ty_res, bool expand_generic)
    {
        if( left.m_path.m_crate_name != right.m_path.m_crate_name )
            return false;
        if( left.m_path.m_components.size() != right.m_path.m_components.size() )
            return false;
        for(unsigned int i = 0; i < left.m_path.m_components.size(); i ++ )
        {
            if( left.m_path.m_components[i] != right.m_path.m_components[i] )
                return false;
        }

        if( left.m_params.m_types.size() > 0 || right.m_params.m_types.size() > 0 )
        {
            // Count mismatch. Allow due to defaults.
            if( left.m_params.m_types.size() != right.m_params.m_types.size() ) {
            }
            else {
                for( unsigned int i = 0; i < right.m_params.m_types.size(); i ++ )
                {
                    if( ! matches_type_int(left.m_params.m_types[i], right.m_params.m_types[i], ty_res, expand_generic) )
                        return false;
                }
            }
        }

        if( left.m_params.m_values.size() > 0 || right.m_params.m_values.size() > 0 )
        {
            auto num = std::min( left.m_params.m_values.size(), right.m_params.m_values.size() );
            for(size_t i = 0; i < num; i ++)
            {
                if( !matches_constgeneric(left.m_params.m_values[i], right.m_params.m_values[i], ty_res, expand_generic) )
                    return false;
            }
        }

        return true;
    }
}

namespace {
    bool is_unbounded_infer(const ::HIR::TypeRef& type) {
        if( const auto* e = type.data().opt_Infer() ) {
            return e->ty_class == ::HIR::InferClass::None;
        }
        else {
            return false;
        }
    }
}

bool ::HIR::TraitImpl::matches_type(const ::HIR::TypeRef& type, ::HIR::t_cb_resolve_type ty_res) const
{
    // NOTE: Don't return any impls when the type is an unbouned ivar. Wouldn't be able to pick anything anyway
    // TODO: For `Unbound`, it could be valid, if the target is a generic.
    // - Pure infer could also be useful (for knowing if there's any other potential impls)

    // HACK: Assume an unbounded matches
    if( is_unbounded_infer(type) ) {
        return true;
    }
    // TODO: Allow unbounded types iff there's some non-unbounded parameters?
    if( is_unbounded_infer(type) || TU_TEST1(type.data(), Path, .binding.is_Unbound()) ) {
        return false;
    }
    return matches_type_int(m_type, type, ty_res, true);
}
bool ::HIR::TypeImpl::matches_type(const ::HIR::TypeRef& type, ::HIR::t_cb_resolve_type ty_res) const
{
    if( is_unbounded_infer(type) || TU_TEST1(type.data(), Path, .binding.is_Unbound()) ) {
        return false;
    }
    return matches_type_int(m_type, type, ty_res, true);
}
bool ::HIR::MarkerImpl::matches_type(const ::HIR::TypeRef& type, ::HIR::t_cb_resolve_type ty_res) const
{
    if( is_unbounded_infer(type) || TU_TEST1(type.data(), Path, .binding.is_Unbound()) ) {
        return false;
    }
    return matches_type_int(m_type, type, ty_res, true);
}

namespace {

    struct TypeOrdSpecific_MixedOrdering
    {
    };

    ::Ordering typelist_ord_specific(const Span& sp, const ::std::vector<::HIR::TypeRef>& left, const ::std::vector<::HIR::TypeRef>& right);

    ::Ordering type_ord_specific(const Span& sp, const ::HIR::TypeRef& left, const ::HIR::TypeRef& right)
    {
        // TODO: What happens if you get `impl<T> Foo<T> for T` vs `impl<T,U> Foo<U> for T`

        // A generic can't be more specific than any other type we can see
        // - It's equally as specific as another Generic, so still false
        if( left.data().is_Generic() ) {
            return right.data().is_Generic() ? ::OrdEqual : ::OrdLess;
        }
        // - A generic is always less specific than anything but itself (handled above)
        if( right.data().is_Generic() ) {
            return ::OrdGreater;
        }

        if( left == right ) {
            return ::OrdEqual;
        }

        TU_MATCH_HDRA( (left.data()), {)
        TU_ARMA(Generic, le)
            throw "";
        TU_ARMA(Infer, le) {
            BUG(sp, "Hit infer");
            }
        TU_ARMA(Diverge, le) {
            BUG(sp, "Hit diverge");
            }
        TU_ARMA(Closure, le) {
            BUG(sp, "Hit closure");
            }
        TU_ARMA(Generator, le) {
            BUG(sp, "Hit generator");
            }
        TU_ARMA(Primitive, le) {
            if(const auto* re = right.data().opt_Primitive() )
            {
                if( le != *re )
                    BUG(sp, "Mismatched types - " << left << " and " << right);
                return ::OrdEqual;
            }
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            }
        TU_ARMA(Path, le) {
            if( !right.data().is_Path() || le.path.m_data.tag() != right.data().as_Path().path.m_data.tag() )
                BUG(sp, "Mismatched types - " << left << " and " << right);
            TU_MATCHA( (le.path.m_data, right.data().as_Path().path.m_data), (lpe, rpe),
            (Generic,
                if( lpe.m_path != rpe.m_path )
                    BUG(sp, "Mismatched types - " << left << " and " << right);
                return typelist_ord_specific(sp, lpe.m_params.m_types, rpe.m_params.m_types);
                ),
            (UfcsUnknown,
                ),
            (UfcsKnown,
                ),
            (UfcsInherent,
                )
            )
            TODO(sp, "Path - " << le.path << " and " << right);
            }
        TU_ARMA(TraitObject, le) {
            ASSERT_BUG(sp, right.data().is_TraitObject(), "Mismatched types - "<< left << " vs " << right);
            const auto& re = right.data().as_TraitObject();
            ASSERT_BUG(sp, le.m_trait.m_path.m_path == re.m_trait.m_path.m_path, "Mismatched types - "<< left << " vs " << right);
            ASSERT_BUG(sp, le.m_markers.size() == re.m_markers.size(), "Mismatched types - "<< left << " vs " << right);

            auto ord = typelist_ord_specific(sp, le.m_trait.m_path.m_params.m_types, re.m_trait.m_path.m_params.m_types);
            if( ord != ::OrdEqual )
                return ord;
            for(size_t i = 0; i < le.m_markers.size(); i ++)
            {
                ASSERT_BUG(sp, le.m_markers[i].m_path == re.m_markers[i].m_path, "Mismatched types - " << left << " vs " << right);
                ord = typelist_ord_specific(sp, le.m_markers[i].m_params.m_types, re.m_markers[i].m_params.m_types);
                if(ord != ::OrdEqual)
                    return ord;
            }
            return ::OrdEqual;
            }
        TU_ARMA(ErasedType, le) {
            TODO(sp, "ErasedType - " << left);
            }
        TU_ARMA(Function, le) {
            if(/*const auto* re =*/ right.data().opt_Function() ) {
                if( left == right )
                    return ::OrdEqual;
                TODO(sp, "Function - " << left << " vs " << right);
                //return typelist_ord_specific(sp, le.arg_types, re->arg_types);
            }
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            }
        TU_ARMA(Tuple, le) {
            if(const auto* re = right.data().opt_Tuple())
            {
                return typelist_ord_specific(sp, le, *re);
            }
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            }
        TU_ARMA(Slice, le) {
            if(const auto* re = right.data().opt_Slice() )
            {
                return type_ord_specific(sp, le.inner, re->inner);
            }
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            }
        TU_ARMA(Array, le) {
            if(const auto* re = right.data().opt_Array() )
            {
                if( le.size != re->size )
                    BUG(sp, "Mismatched types - " << left << " and " << right);
                return type_ord_specific(sp, le.inner, re->inner);
            }
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            }
        TU_ARMA(Pointer, le) {
            if(const auto* re = right.data().opt_Pointer() )
            {
                if( le.type != re->type )
                    BUG(sp, "Mismatched types - " << left << " and " << right);
                return type_ord_specific(sp, le.inner, re->inner);
            }
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            }
        TU_ARMA(Borrow, le) {
            if(const auto* re = right.data().opt_Borrow())
            {
                if( le.type != re->type )
                    BUG(sp, "Mismatched types - " << left << " and " << right);
                return type_ord_specific(sp, le.inner, re->inner);
            }
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            }
        }
        throw "Fell off end of type_ord_specific";
    }

    ::Ordering typelist_ord_specific(const Span& sp, const ::std::vector<::HIR::TypeRef>& le, const ::std::vector<::HIR::TypeRef>& re)
    {
        auto rv = ::OrdEqual;
        assert(le.size() == re.size());
        for(unsigned int i = 0; i < le.size(); i ++) {
            auto a = type_ord_specific(sp, le[i], re[i]);
            if( a != ::OrdEqual ) {
                if( rv != ::OrdEqual && a != rv )
                {
                    DEBUG("Inconsistent ordering between type lists - i=" << i << " [" << le << "] vs [" << re << "]");
                    throw TypeOrdSpecific_MixedOrdering {};
                }
                rv = a;
            }
        }
        return rv;
    }
}

namespace {
    void add_bound_from_trait(::std::vector< ::HIR::GenericBound>& rv,  const ::HIR::TypeRef& type, const ::HIR::TraitPath& cur_trait)
    {
        static Span sp;
        assert( cur_trait.m_trait_ptr );
        const auto& tr = *cur_trait.m_trait_ptr;
        auto monomorph_cb = MonomorphStatePtr(&type, &cur_trait.m_path.m_params, nullptr);

        for(const auto& trait_path_raw : tr.m_all_parent_traits)
        {
            // 1. Monomorph
            auto trait_path_mono = monomorph_cb.monomorph_traitpath(sp, trait_path_raw, false);
            // 2. Add
            rv.push_back( ::HIR::GenericBound::make_TraitBound({ type.clone(), mv$(trait_path_mono) }) );
        }

        // TODO: Add traits from `Self: Foo` bounds?
        // TODO: Move associated types to the source trait.
    }
    ::std::vector< ::HIR::GenericBound> flatten_bounds(const ::std::vector<::HIR::GenericBound>& bounds)
    {
        ::std::vector< ::HIR::GenericBound >    rv;
        for(const auto& b : bounds)
        {
            TU_MATCHA( (b), (be),
            (Lifetime,
                rv.push_back( ::HIR::GenericBound(be) );
                ),
            (TypeLifetime,
                rv.push_back( ::HIR::GenericBound::make_TypeLifetime({ be.type.clone(), be.valid_for }) );
                ),
            (TraitBound,
                rv.push_back( ::HIR::GenericBound::make_TraitBound({ be.type.clone(), be.trait.clone() }) );
                add_bound_from_trait(rv,  be.type, be.trait);
                ),
            (TypeEquality,
                rv.push_back( ::HIR::GenericBound::make_TypeEquality({ be.type.clone(), be.other_type.clone() }) );
                )
            )
        }
        ::std::sort(rv.begin(), rv.end(), [](const auto& a, const auto& b){ return ::ord(a,b) == OrdLess; });
        return rv;
    }
}

bool ::HIR::TraitImpl::more_specific_than(const ::HIR::TraitImpl& other) const
{
    static const Span   _sp;
    const Span& sp = _sp;
    TRACE_FUNCTION;
    //DEBUG("this  = " << *this);
    //DEBUG("other = " << other);

    // >> https://github.com/rust-lang/rfcs/blob/master/text/1210-impl-specialization.md#defining-the-precedence-rules
    // 1. If this->m_type is less specific than other.m_type: return false
    try
    {
        // If any in te.impl->m_params is less specific than oe.impl->m_params: return false
        auto ord = typelist_ord_specific(sp, this->m_trait_args.m_types, other.m_trait_args.m_types);
        if( ord != ::OrdEqual ) {
            DEBUG("- Trait arguments " << (ord == ::OrdLess ? "less" : "more") << " specific");
            return ord == ::OrdGreater;
        }

        ord = type_ord_specific(sp, this->m_type, other.m_type);
        // If `*this` < `other` : false
        if( ord != ::OrdEqual ) {
            DEBUG("- Type " << this->m_type << " " << (ord == ::OrdLess ? "less" : "more") << " specific than " << other.m_type);
            return ord == ::OrdGreater;
        }
    }
    catch(const TypeOrdSpecific_MixedOrdering& e)
    {
        BUG(sp, "Mixed ordering in more_specific_than");
    }

    //if( other.m_params.m_bounds.size() == 0 ) {
    //    DEBUG("- Params (none in other, some in this)");
    //    return m_params.m_bounds.size() > 0;
    //}
    // 3. Compare bound set, if there is a rule in oe that is missing from te; return false
    // TODO: Cache these lists (calculate after outer typecheck?)
    auto bounds_t = flatten_bounds(m_params.m_bounds);
    auto bounds_o = flatten_bounds(other.m_params.m_bounds);

    DEBUG("bounds_t = " << bounds_t);
    DEBUG("bounds_o = " << bounds_o);

    // If there are less bounds in this impl, it can't be more specific.
    if( bounds_t.size() < bounds_o.size() )
    {
        DEBUG("Bound count");
        return false;
    }

    auto it_t = bounds_t.begin();
    auto it_o = bounds_o.begin();
    bool is_equal = true;
    while( it_t != bounds_t.end() && it_o != bounds_o.end() )
    {
        auto cmp = ::ord(*it_t, *it_o);
        // Equal bounds? advance both
        if( cmp == OrdEqual )
        {
            ++it_t;
            ++it_o;
            continue ;
        }

        // If the two bounds are similar
        if( it_t->tag() == it_o->tag() && it_t->is_TraitBound() )
        {
            const auto& b_t = it_t->as_TraitBound();
            const auto& b_o = it_o->as_TraitBound();
            // Check if the type is equal
            if( b_t.type == b_o.type && b_t.trait.m_path.m_path == b_o.trait.m_path.m_path )
            {
                const auto& params_t = b_t.trait.m_path.m_params;
                const auto& params_o = b_o.trait.m_path.m_params;
                switch( typelist_ord_specific(sp, params_t.m_types, params_o.m_types) )
                {
                case ::OrdLess: return false;
                case ::OrdGreater: return true;
                case ::OrdEqual:    break;
                }
                // TODO: Find cases where there's `T: Foo<T>` and `T: Foo<U>`
                for(unsigned int i = 0; i < params_t.m_types.size(); i ++ )
                {
                    if( params_t.m_types[i] != params_o.m_types[i] && params_t.m_types[i] == b_t.type )
                    {
                        return true;
                    }
                }
                TODO(sp, *it_t << " ?= " << *it_o);
            }
        }

        if( cmp == OrdLess )
        {
            is_equal = false;
            ++ it_t;
        }
        else
        {
            //++ it_o;
            DEBUG(*it_t << " ?= " << *it_o << " : " << cmp);
            return false;
        }
    }
    if( it_t != bounds_t.end() )
    {
        DEBUG("Remaining local bounds - " << *it_t);
        return true;
    }
    else
    {
        DEBUG("Out of local bounds, equal or less specific");
        return !is_equal;
    }
}

namespace {

    struct ImplTyMatcher:
        public ::HIR::MatchGenerics,
        public Monomorphiser
    {
        ::std::vector<const ::HIR::TypeRef*>    impl_tys;


        ::HIR::Compare match_ty(const ::HIR::GenericRef& g, const ::HIR::TypeRef& ty, ::HIR::t_cb_resolve_type _resolve_cb) override {
            assert(g.binding < impl_tys.size());
            if( impl_tys.at(g.binding) )
            {
                DEBUG("Compare " << ty << " and " << *impl_tys.at(g.binding));
                return (ty == *impl_tys.at(g.binding) ? ::HIR::Compare::Equal : ::HIR::Compare::Unequal);
            }
            else
            {
                impl_tys.at(g.binding) = &ty;
                return ::HIR::Compare::Equal;
            }
        }
        ::HIR::Compare match_val(const ::HIR::GenericRef& g, const ::HIR::ConstGeneric& sz) override {
            TODO(Span(), "Matcher::match_val " << g << " with " << sz);
        }

        ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& g) const override {
            ASSERT_BUG(sp, g.group() == 0, "");
            ASSERT_BUG(sp, g.idx() < impl_tys.size(), "");
            ASSERT_BUG(sp, impl_tys[g.idx()], "");
            return impl_tys[g.idx()]->clone();
        }
        ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& g) const override {
            TODO(Span(), "Matcher::get_value " << g);
        }


        void reinit(const HIR::GenericParams& params) {
            this->impl_tys.clear();
            this->impl_tys.resize(params.m_types.size());
        }
    };
}

// Returns `true` if the two impls overlap in the types they will accept
bool ::HIR::TraitImpl::overlaps_with(const Crate& crate, const ::HIR::TraitImpl& other) const
{
    // TODO: Pre-calculate impl trees (with pointers to parent impls)
    struct H {
        static bool types_overlap(const ::HIR::PathParams& a, const ::HIR::PathParams& b)
        {
            for(unsigned int i = 0; i < ::std::min(a.m_types.size(), b.m_types.size()); i ++)
            {
                if( ! H::types_overlap(a.m_types[i], b.m_types[i]) )
                    return false;
            }
            return true;
        }
        static bool types_overlap(const ::HIR::TypeRef& a, const ::HIR::TypeRef& b)
        {
            static Span sp;
            //DEBUG("(" << a << "," << b << ")");
            if( a.data().is_Generic() || b.data().is_Generic() )
                return true;
            // TODO: Unbound/Opaque paths?
            if( a.data().tag() != b.data().tag() )
                return false;
            TU_MATCH_HDRA( (a.data(), b.data()), {)
            TU_ARMA(Generic, ae, be) {
                }
            TU_ARMA(Infer, ae, be) {
                }
            TU_ARMA(Diverge, ae, be) {
                }
            TU_ARMA(Closure, ae, be) {
                BUG(sp, "Hit closure");
                }
            TU_ARMA(Generator, ae, be) {
                BUG(sp, "Hit generator");
                }
            TU_ARMA(Primitive, ae, be) {
                if( ae != be )
                    return false;
                }
            TU_ARMA(Path, ae, be) {
                if( ae.path.m_data.tag() != be.path.m_data.tag() )
                    return false;
                TU_MATCHA( (ae.path.m_data, be.path.m_data), (ape, bpe),
                (Generic,
                    if( ape.m_path != bpe.m_path )
                        return false;
                    return H::types_overlap(ape.m_params, bpe.m_params);
                    ),
                (UfcsUnknown,
                    ),
                (UfcsKnown,
                    ),
                (UfcsInherent,
                    )
                )
                TODO(sp, "Path - " << ae.path << " and " << be.path);
                }
            TU_ARMA(TraitObject, ae, be) {
                if( ae.m_trait.m_path.m_path != be.m_trait.m_path.m_path )
                    return false;
                if( !H::types_overlap(ae.m_trait.m_path.m_params, be.m_trait.m_path.m_params) )
                    return false;
                // Marker traits only overlap if the lists are the same (with overlap)
                if( ae.m_markers.size() != be.m_markers.size() )
                    return false;
                for(size_t i = 0; i < ae.m_markers.size(); i++)
                {
                    if( ae.m_markers[i].m_path != be.m_markers[i].m_path )
                        return false;
                    if( !H::types_overlap(ae.m_markers[i].m_params, be.m_markers[i].m_params) )
                        return false;
                }
                return true;
                }
            TU_ARMA(ErasedType, ae, be) {
                TODO(sp, "ErasedType - " << a);
                }
            TU_ARMA(Function, ae, be) {
                if( ae.is_unsafe != be.is_unsafe )
                    return false;
                if( ae.m_abi != be.m_abi )
                    return false;
                if( ae.m_arg_types.size() != be.m_arg_types.size() )
                    return false;
                for(unsigned int i = 0; i < ae.m_arg_types.size(); i ++)
                {
                    if( ! H::types_overlap(ae.m_arg_types[i], be.m_arg_types[i]) )
                        return false;
                }
                if( !H::types_overlap(ae.m_rettype, be.m_rettype) )
                    return false;
                }
            TU_ARMA(Tuple, ae, be) {
                if( ae.size() != be.size() )
                    return false;
                for(unsigned int i = 0; i < ae.size(); i ++)
                {
                    if( ! H::types_overlap(ae[i], be[i]) )
                        return false;
                }
                }
            TU_ARMA(Slice, ae, be) {
                return H::types_overlap( ae.inner, be.inner );
                }
            TU_ARMA(Array, ae, be) {
                if( ae.size != be.size )
                    return false;
                return H::types_overlap( ae.inner, be.inner );
                }
            TU_ARMA(Pointer, ae, be) {
                if( ae.type != be.type )
                    return false;
                return H::types_overlap( ae.inner, be.inner );
                }
            TU_ARMA(Borrow, ae, be) {
                if( ae.type != be.type )
                    return false;
                return H::types_overlap( ae.inner, be.inner );
                }
            }
            return true;
        }
    };

    // Quick Check: If the types are equal, they do overlap
    if(this->m_type == other.m_type && this->m_trait_args == other.m_trait_args)
    {
        return true;
    }

    // 1. Are the impl types of the same form (or is one generic)
    if( ! H::types_overlap(this->m_type, other.m_type) )
        return false;
    if( ! H::types_overlap(this->m_trait_args, other.m_trait_args) )
        return false;

    DEBUG("TODO: Handle potential overlap (when not exactly equal)");
    //return this->m_type == other.m_type && this->m_trait_args == other.m_trait_args;
    Span    sp;

    // TODO: Use `type_ord_specific` but treat any case of mixed ordering as this returning `false`
    try
    {
        type_ord_specific(sp, this->m_type, other.m_type);
        typelist_ord_specific(sp, this->m_trait_args.m_types, other.m_trait_args.m_types);
    }
    catch(const TypeOrdSpecific_MixedOrdering& /*e*/)
    {
        return false;
    }

    // TODO: Detect `impl<T> Foo<T> for Bar<T>` vs `impl<T> Foo<&T> for Bar<T>`
    // > Create values for impl params from the type, then check if the trait params are compatible
    // > Requires two lists, and telling which one to use by the end
    auto cb_ident = [](const ::HIR::TypeRef& x)->const ::HIR::TypeRef& { return x; };
    bool is_reversed = false;
    ImplTyMatcher matcher;
    matcher.reinit(this->m_params);
    if( ! this->m_type.match_test_generics(sp, other.m_type, cb_ident, matcher) )
    {
        DEBUG("- Type mismatch, try other ordering");
        is_reversed = true;
        matcher.reinit( other.m_params );
        if( !other.m_type.match_test_generics(sp, this->m_type, cb_ident, matcher) )
        {
            DEBUG("- Type mismatch in both orderings");
            return false;
        }
        if( other.m_trait_args.match_test_generics_fuzz(sp, this->m_trait_args, cb_ident, matcher) != ::HIR::Compare::Equal )
        {
            DEBUG("- Params mismatch");
            return false;
        }
        // Matched with second ording
    }
    else if( this->m_trait_args.match_test_generics_fuzz(sp, other.m_trait_args, cb_ident, matcher) != ::HIR::Compare::Equal )
    {
        DEBUG("- Param mismatch, try other ordering");
        is_reversed = true;
        matcher.reinit(other.m_params);
        if( !other.m_type.match_test_generics(sp, this->m_type, cb_ident, matcher) )
        {
            DEBUG("- Type mismatch in alt ordering");
            return false;
        }
        if( other.m_trait_args.match_test_generics_fuzz(sp, this->m_trait_args, cb_ident, matcher) != ::HIR::Compare::Equal )
        {
            DEBUG("- Params mismatch in alt ordering");
            return false;
        }
        // Matched with second ordering
    }
    else
    {
        // Matched with first ordering
    }

    struct H2 {
        static const ::HIR::TypeRef& monomorph(const Span& sp, const ::HIR::TypeRef& in_ty, const Monomorphiser& ms, ::HIR::TypeRef& tmp)
        {
            if( ! monomorphise_type_needed(in_ty) ) {
                return in_ty;
            }
            else {
                tmp = ms.monomorph_type(sp, in_ty);
                // TODO: EAT?
                return tmp;
            }
        }
        static const ::HIR::TraitPath& monomorph(const Span& sp, const ::HIR::TraitPath& in, const Monomorphiser& ms, ::HIR::TraitPath& tmp)
        {
            if( ! monomorphise_traitpath_needed(in) ) {
                return in;
            }
            else {
                tmp = ms.monomorph_traitpath(sp, in, true);
                // TODO: EAT?
                return tmp;
            }
        }
        static bool check_bounds(const ::HIR::Crate& crate, const ::HIR::TraitImpl& id, const Monomorphiser& ms, const ::HIR::TraitImpl& g_src)
        {
            TRACE_FUNCTION;
            static Span sp;
            for(const auto& tb : id.m_params.m_bounds)
            {
                DEBUG(tb);
                if(tb.is_TraitBound())
                {
                    ::HIR::TypeRef  tmp_ty;
                    ::HIR::TraitPath    tmp_tp;
                    const auto& ty = H2::monomorph(sp, tb.as_TraitBound().type, ms, tmp_ty);
                    const auto& trait = H2::monomorph(sp, tb.as_TraitBound().trait, ms, tmp_tp);;

                    // Determine if `ty` would be bounded (it's an ATY or generic)
                    if( ty.data().is_Generic() ) {
                        bool found = false;
                        for(const auto& bound : g_src.m_params.m_bounds)
                        {
                            if(const auto* be = bound.opt_TraitBound())
                            {
                                if( be->type != ty ) continue;
                                if( be->trait != trait ) continue;
                                found = true;
                            }
                        }
                        if( !found )
                        {
                            DEBUG("No matching bound for " << ty << " : " << trait << " in source bounds - " << g_src.m_params.fmt_bounds());
                            return false;
                        }
                    }
                    else if( TU_TEST1(ty.data(), Path, .binding.is_Opaque()) ) {
                        TODO(sp, "Check bound " << ty << " : " << trait << " in source bounds or trait bounds");
                    }
                    else {
                        // Search the crate for an impl
                        bool rv = crate.find_trait_impls(trait.m_path.m_path, ty, [](const auto&t)->const auto&{ return t; }, [&](const ::HIR::TraitImpl& ti)->bool {
                                DEBUG("impl" << ti.m_params.fmt_args() << " " << trait.m_path.m_path << ti.m_trait_args << " for " << ti.m_type << ti.m_params.fmt_bounds());

                                auto cb_ident = [](const ::HIR::TypeRef& x)->const ::HIR::TypeRef& { return x; };
                                ImplTyMatcher   matcher;
                                matcher.reinit(ti.m_params);
                                // 1. Triple-check the type matches (and get generics)
                                if( ! ti.m_type.match_test_generics(sp, ty, cb_ident, matcher) )
                                    return false;
                                // 2. Check trait params
                                assert(trait.m_path.m_params.m_types.size() == ti.m_trait_args.m_types.size());
                                for(size_t i = 0; i < trait.m_path.m_params.m_types.size(); i ++)
                                {
                                    if( !ti.m_trait_args.m_types[i].match_test_generics(sp, trait.m_path.m_params.m_types[i], cb_ident, matcher) )
                                        return false;
                                }
                                // 3. Check bounds on the impl
                                if( !H2::check_bounds(crate, ti, matcher, g_src) )
                                    return false;
                                // 4. Check ATY bounds on the trait path
                                for(const auto& atyb : trait.m_type_bounds)
                                {
                                    if( ti.m_types.count(atyb.first) == 0 ) {
                                       DEBUG("Associated type '" << atyb.first << "' not in trait impl, assuming good");
                                    }
                                    else {
                                        const auto& aty = ti.m_types.at(atyb.first);
                                        if( !aty.data.match_test_generics(sp, atyb.second.type, cb_ident, matcher) )
                                            return false;
                                    }
                                }
                                // All those pass? It's good.
                                return true;
                            });
                        if( !rv )
                        {
                            return false;
                        }
                    }
                }
                else
                {
                    // TODO: Other bound types?
                }
            }
            // No bounds failed, it's good
            return true;
        }
    };

    // The two impls could overlap, pending on trait bounds
    if(is_reversed)
    {
        DEBUG("(reversed) impl params " << FMT_CB(os,
                for(auto* p : matcher.impl_tys)
                {
                    if(p)
                        os << *p;
                    else
                        os << "?";
                    os << ",";
                }
                ));
        // Check bounds on `other` using these params
        // TODO: Take a callback that does the checks. Or somehow return a "maybe overlaps" result?
        return H2::check_bounds(crate, other, matcher, *this);
    }
    else
    {
        DEBUG("impl params " << FMT_CB(os,
                for(auto* p : matcher.impl_tys)
                {
                    if(p)
                        os << *p;
                    else
                        os << "?";
                    os << ",";
                }
                ));
        // Check bounds on `*this`
        return H2::check_bounds(crate, *this, matcher, other);
    }
}

namespace
{
    template<typename ImplType>
    bool find_impls_list(const typename ::HIR::Crate::ImplGroup<::std::unique_ptr<ImplType>>::list_t& impl_list, const ::HIR::TypeRef& type, ::HIR::t_cb_resolve_type ty_res, ::std::function<bool(const ImplType&)> callback)
    {
        for(const auto& impl : impl_list)
        {
            if( impl->matches_type(type, ty_res) )
            {
                if( callback(*impl) )
                {
                    return true;
                }
            }
        }
        return false;
    }
    template<typename ImplType>
    bool find_impls_list(const typename ::HIR::Crate::ImplGroup<const ImplType*>::list_t& impl_list, const ::HIR::TypeRef& type, ::HIR::t_cb_resolve_type ty_res, ::std::function<bool(const ImplType&)> callback)
    {
        for(const auto& impl : impl_list)
        {
            if( impl->matches_type(type, ty_res) )
            {
                if( callback(*impl) )
                {
                    return true;
                }
            }
        }
        return false;
    }
}
namespace
{
    bool find_trait_impls_int(
            const ::HIR::Crate& crate, const ::HIR::SimplePath& trait, const ::HIR::TypeRef& type,
            ::HIR::t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::TraitImpl&)> callback
            )
    {
        auto it = crate.m_trait_impls.find( trait );
        if( it != crate.m_trait_impls.end() )
        {
            // 1. Find named impls (associated with named types)
            if( const auto* impl_list = it->second.get_list_for_type(type) )
            {
                if( find_impls_list(*impl_list, type, ty_res, callback) )
                    return true;
            }
            // - If the type is an ivar, search all types
            if( type.data().is_Infer() && !type.data().as_Infer().is_lit() )
            {
                DEBUG("Search all lists");
                for(const auto& list : it->second.named)
                {
                    if( find_impls_list(list.second, type, ty_res, callback) )
                        return true;
                }
            }

            // 2. Search fully generic list.
            if( find_impls_list(it->second.generic, type, ty_res, callback) )
                return true;
        }

        return false;
    }

    // Obtain the crate that defined the named type
    // See https://github.com/rust-lang/rfcs/blob/master/text/2451-re-rebalancing-coherence.md
    // - Catch: The above allows `impl ForeignTrait for ForeignType<LocalType>`
    // Could return a list of crates to search
    RcString get_type_crate(const ::HIR::Crate& crate, const ::HIR::TypeRef& ty)
    {
        TU_MATCH_HDRA( (ty.data()), { )
        TU_ARMA(Infer, _)
            return RcString();
        TU_ARMA(Generic, _)
            return RcString();
        TU_ARMA(Diverge, _)
            TODO(Span(), "Find the crate with " << ty << "'s inherent impl");
        TU_ARMA(Primitive, _) {
            TODO(Span(), "Find the crate with " << ty << "'s inherent impl");
            }
        TU_ARMA(Slice, _) {
            TODO(Span(), "Find the crate with " << ty << "'s inherent impl");
            }
        TU_ARMA(Array, _)
            TODO(Span(), "Find the crate with " << ty << "'s inherent impl");
        // Paths: Unowned if unknown/generic, otherwise defining crate
        TU_ARMA(Path, te) {
            if( te.binding.is_Unbound() || te.binding.is_Opaque() )
                return RcString();
            assert( te.path.m_data.is_Generic() );
            // TODO: If the type is marked as fundamental, then recurse into the first generic
            return te.path.m_data.as_Generic().m_path.m_crate_name;
            }
        TU_ARMA(TraitObject, te) {
            return te.m_trait.m_path.m_path.m_crate_name;
            }
        TU_ARMA(Closure, _)
            return crate.m_crate_name;
        TU_ARMA(Generator, _)
            return crate.m_crate_name;
        // Functions aren't owned
        TU_ARMA(Function, _)
            return RcString();
        TU_ARMA(ErasedType, _)
            return RcString();
        TU_ARMA(Tuple, _)
            return RcString();
        // Recurse into pointers
        TU_ARMA(Borrow, te)
            return get_type_crate(crate, te.inner);
        TU_ARMA(Pointer, te)
            return get_type_crate(crate, te.inner);
        }
        throw "Unreachable";
    }
}

bool ::HIR::Crate::find_trait_impls(const ::HIR::SimplePath& trait, const ::HIR::TypeRef& type, t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::TraitImpl&)> callback) const
{
    if( this->m_all_trait_impls.size() > 0 )
    {
        auto it = this->m_all_trait_impls.find( trait );
        if( it != this->m_all_trait_impls.end() )
        {
            // 1. Find named impls (associated with named types)
            if( const auto* impl_list = it->second.get_list_for_type(type) )
            {
                if( find_impls_list(*impl_list, type, ty_res, callback) )
                    return true;
            }
            // - If the type is an ivar, search all types
            if( type.data().is_Infer() && !type.data().as_Infer().is_lit() )
            {
                DEBUG("Search all lists");
                for(const auto& list : it->second.named)
                {
                    if( find_impls_list(list.second, type, ty_res, callback) )
                        return true;
                }
            }

            // 2. Search fully generic list.
            if( find_impls_list(it->second.generic, type, ty_res, callback) )
                return true;
        }

        return false;
    }

    // TODO: Determine the source crates for this type and trait (coherence) and only search those
    if( find_trait_impls_int(*this, trait, type, ty_res, callback) )
    {
        return true;
    }
    for( const auto& ec : this->m_ext_crates )
    {
        if( find_trait_impls_int(*ec.second.m_data, trait, type, ty_res, callback) )
        {
            return true;
        }
    }
    return false;
}
namespace
{
    bool find_auto_trait_impls_int(
            const ::HIR::Crate& crate, const ::HIR::SimplePath& trait, const ::HIR::TypeRef& type,
            ::HIR::t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::MarkerImpl&)> callback
            )
    {
        auto it = crate.m_marker_impls.find( trait );
        if( it != crate.m_marker_impls.end() )
        {
            // 1. Find named impls (associated with named types)
            if( const auto* impl_list = it->second.get_list_for_type(type) )
            {
                if( find_impls_list(*impl_list, type, ty_res, callback) )
                    return true;
            }

            // 2. Search fully generic list.
            if( find_impls_list(it->second.generic, type, ty_res, callback) )
                return true;
        }

        return false;
    }
}
bool ::HIR::Crate::find_auto_trait_impls(const ::HIR::SimplePath& trait, const ::HIR::TypeRef& type, t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::MarkerImpl&)> callback) const
{
    if( this->m_all_marker_impls.size() > 0 ) {
        auto it = this->m_all_marker_impls.find( trait );
        if( it != this->m_all_marker_impls.end() )
        {
            // 1. Find named impls (associated with named types)
            if( const auto* impl_list = it->second.get_list_for_type(type) )
            {
                if( find_impls_list(*impl_list, type, ty_res, callback) )
                    return true;
            }

            // 2. Search fully generic list.
            if( find_impls_list(it->second.generic, type, ty_res, callback) )
                return true;
        }

        return false;
    }

    if( find_auto_trait_impls_int(*this, trait, type, ty_res, callback) )
    {
        return true;
    }
    for( const auto& ec : this->m_ext_crates )
    {
        if( find_auto_trait_impls_int(*ec.second.m_data, trait, type, ty_res, callback) )
        {
            return true;
        }
    }
    return false;
}
namespace
{
    bool find_type_impls_int(const ::HIR::Crate& crate, const ::HIR::TypeRef& type, ::HIR::t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::TypeImpl&)> callback)
    {
        // 1. Find named impls (associated with named types)
        if( const auto* impl_list = crate.m_type_impls.get_list_for_type(type) )
        {
            if( find_impls_list(*impl_list, type, ty_res, callback) )
                return true;
        }

        // 2. Search fully generic list?
        if( find_impls_list(crate.m_type_impls.generic, type, ty_res, callback) )
            return true;

        return false;
    }
}
bool ::HIR::Crate::find_type_impls(const ::HIR::TypeRef& type, t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::TypeImpl&)> callback) const
{
    if( m_all_trait_impls.size() > 0 ) {
        // 1. Find named impls (associated with named types)
        if( const auto* impl_list = this->m_all_type_impls.get_list_for_type(type) )
        {
            if( find_impls_list(*impl_list, type, ty_res, callback) )
                return true;
        }

        // 2. Search fully generic list?
        if( find_impls_list(this->m_all_type_impls.generic, type, ty_res, callback) )
            return true;

        return false;
    }
    // TODO: Determine the source crate for this type (coherence) and only search that

    // > Current crate
    if( find_type_impls_int(*this, type, ty_res, callback) )
    {
        return true;
    }
    for( const auto& ec : this->m_ext_crates )
    {
        //DEBUG("- " << ec.first);
        if( find_type_impls_int(*ec.second.m_data, type, ty_res, callback) )
        {
            return true;
        }
    }
    return false;
}

const ::MIR::Function* HIR::Crate::get_or_gen_mir(const ::HIR::ItemPath& ip, const ::HIR::ExprPtr& ep, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_ty) const
{
    if( !ep )
    {
        // No HIR, so has to just have MIR - from a extern crate most likely
        ASSERT_BUG(Span(), ep.m_mir, "No HIR (!ep) and no MIR (!ep.m_mir) for " << ip);
        return &*ep.m_mir;
    }
    else
    {
        if( !ep.m_mir )
        {
            TRACE_FUNCTION_F(ip);
            ASSERT_BUG(Span(), ep.m_state, "No ExprState for " << ip);

            auto& ep_mut = const_cast<::HIR::ExprPtr&>(ep);

            // TODO: Ensure that all referenced items have constants evaluated
            if( ep.m_state->stage < ::HIR::ExprState::Stage::ConstEval )
            {
                if( ep.m_state->stage == ::HIR::ExprState::Stage::ConstEvalRequest )
                    ERROR(Span(), E0000, "Loop in constant evaluation");
                ep.m_state->stage = ::HIR::ExprState::Stage::ConstEvalRequest;
                ConvertHIR_ConstantEvaluate_Expr(*this, ip, ep_mut);
                ep.m_state->stage = ::HIR::ExprState::Stage::ConstEval;
            }

            // Ensure typechecked
            if( ep.m_state->stage < ::HIR::ExprState::Stage::Typecheck )
            {
                if( ep.m_state->stage == ::HIR::ExprState::Stage::TypecheckRequest )
                    ERROR(Span(), E0000, "Loop in constant evaluation");
                ep.m_state->stage = ::HIR::ExprState::Stage::TypecheckRequest;

                // TODO: Set debug/timing stage
                //Debug_SetStagePre("HIR Typecheck");
                // - Can store that on the Expr, OR get it from the item path
                typeck::ModuleState ms { const_cast<::HIR::Crate&>(*this) };
                //ms.prepare_from_path( ip );   // <- Ideally would use this, but it's a lot of code for one usage
                ms.m_impl_generics = ep.m_state->m_impl_generics;
                ms.m_item_generics = ep.m_state->m_item_generics;
                ms.m_traits = ep.m_state->m_traits;
                ms.m_mod_paths.push_back(ep.m_state->m_mod_path);
                Typecheck_Code(ms, const_cast<::HIR::Function::args_t&>(args), ret_ty, ep_mut);
                //Debug_SetStagePre("Expand HIR Annotate");
                HIR_Expand_AnnotateUsage_Expr(*this, ep_mut);
                // NOTE: Disabled due to challenges in making new statics at this stage
                //HIR_Expand_StaticBorrowConstants_Expr(*this, ep_mut);
                //Debug_SetStagePre("Expand HIR Closures");
                HIR_Expand_Closures_Expr(*this, ep_mut);
                //Debug_SetStagePre("Expand HIR Calls");
                HIR_Expand_UfcsEverything_Expr(*this, ep_mut);
                //Debug_SetStagePre("Expand HIR Reborrows");
                HIR_Expand_Reborrows_Expr(*this, ep_mut);
                //Debug_SetStagePre("Expand HIR ErasedType");
                //HIR_Expand_ErasedType(*this, ep_mut);    // - Maybe?
                //Typecheck_Expressions_Validate(*hir_crate);

                ep.m_state->stage = ::HIR::ExprState::Stage::Typecheck;
            }
            // Generate MIR
            if( ep.m_state->stage < ::HIR::ExprState::Stage::Mir )
            {
                if( ep.m_state->stage == ::HIR::ExprState::Stage::MirRequest )
                    ERROR(Span(), E0000, "Loop in constant evaluation");
                ep.m_state->stage = ::HIR::ExprState::Stage::MirRequest;
                //Debug_SetStage("Lower MIR");
                HIR_GenerateMIR_Expr(*this, ip, ep_mut, args, ret_ty);
                ep.m_state->stage = ::HIR::ExprState::Stage::Mir;
            }
            assert(ep.m_mir);
        }
        return &*ep.m_mir;
    }
}


::HIR::TypeRef HIR::Trait::get_vtable_type(const Span& sp, const ::HIR::Crate& crate, const ::HIR::TypeData::Data_TraitObject& te) const
{
    assert(te.m_trait.m_trait_ptr == this);

    const auto& vtable_ty_spath = this->m_vtable_path;
    const auto& vtable_ref = crate.get_struct_by_path(sp, vtable_ty_spath);
    // Copy the param set from the trait in the trait object
    ::HIR::PathParams   vtable_params = te.m_trait.m_path.m_params.clone();
    // - Include associated types on bound
    for(const auto& ty_b : te.m_trait.m_type_bounds) {
        auto idx = this->m_type_indexes.at(ty_b.first);
        if(vtable_params.m_types.size() <= idx)
            vtable_params.m_types.resize(idx+1);
        vtable_params.m_types[idx] = ty_b.second.type.clone();
    }
    return ::HIR::TypeRef::new_path( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref );
}

unsigned HIR::Trait::get_vtable_value_index(const HIR::GenericPath& trait_path, const RcString& name) const
{
    auto its = this->m_value_indexes.equal_range(name);
    for(auto it = its.first; it != its.second; ++it)
    {
        DEBUG(trait_path << " :: " << name << " - " << it->second.second);
        if( it->second.second.m_path == trait_path.m_path )
        {
            // TODO: Match generics using match_test_generics comparing to the trait args
            assert(it->second.first > 0);
            return it->second.first;
        }
    }
    return 0;
}

/// Helper for getting the struct associated with a pattern path
const ::HIR::Struct& HIR::pattern_get_struct(const Span& sp, const ::HIR::Path& path, const ::HIR::Pattern::PathBinding& binding, bool is_tuple)
{
    const ::HIR::Struct* str_p = nullptr;
    TU_MATCH_HDRA( (binding), { )
    TU_ARMA(Unbound, be)
        BUG(sp, "Unexpected unbound named pattern - " << path);
    TU_ARMA(Struct, be) {
        str_p = be;
        }
    TU_ARMA(Enum, be) {
        const auto& enm = *be.ptr;
        if(is_tuple) {
            ASSERT_BUG(sp, enm.m_data.is_Data(), "PathTuple pattern with non-data enum - " << path);
        }
        else {
            ASSERT_BUG(sp, enm.m_data.is_Data(), "PathNamed pattern with non-data enum - " << path);
        }
        const auto& enm_d = enm.m_data.as_Data();
        ASSERT_BUG(sp, be.var_idx < enm_d.size(), "Variant index " << be.var_idx << " out of range - " << path);
        if(is_tuple) {
            ASSERT_BUG(sp, !enm_d[be.var_idx].is_struct, "PathTuple pattern with brace enum variant - " << path);
        }
        else {
            ASSERT_BUG(sp, enm_d[be.var_idx].is_struct, "PathNamed pattern with non-brace enum variant - " << path);
        }
        str_p = enm_d[be.var_idx].type.data().as_Path().binding.as_Struct();
        }
    }
    const auto& str = *str_p;

    if(is_tuple) {
        ASSERT_BUG(sp, str.m_data.is_Tuple(), "PathTuple pattern with non-tuple struct - " << str.m_data.tag_str());
    }
    else {
        ASSERT_BUG(sp, str.m_data.is_Named(), "Struct pattern on non-brace struct");
    }
    return str;
}
const ::HIR::t_tuple_fields& HIR::pattern_get_tuple(const Span& sp, const ::HIR::Path& path, const ::HIR::Pattern::PathBinding& binding) {
    return pattern_get_struct(sp, path, binding, true).m_data.as_Tuple();
}
const ::HIR::t_struct_fields& HIR::pattern_get_named(const Span& sp, const ::HIR::Path& path, const ::HIR::Pattern::PathBinding& binding) {
    return pattern_get_struct(sp, path, binding, false).m_data.as_Named();
}

namespace HIR {
EncodedLiteralPtr::EncodedLiteralPtr(EncodedLiteral el)
{
    p = new EncodedLiteral(mv$(el));
}
EncodedLiteralPtr::~EncodedLiteralPtr()
{
    if(p) {
        delete p;
        p = nullptr;
    }
}
}

// ---
EncodedLiteral EncodedLiteral::make_usize(uint64_t v)
{
    EncodedLiteral  rv;
    rv.bytes.resize(Target_GetPointerBits() / 8);
    rv.write_usize(0, v);
    return rv;
}
EncodedLiteral EncodedLiteral::clone() const
{
    EncodedLiteral  rv;
    rv.bytes = bytes;
    rv.relocations.reserve( relocations.size() );
    for(const auto& r : relocations) {
        if( r.p ) {
            rv.relocations.push_back(Reloc::new_named(r.ofs, r.len, r.p->clone()));
        }
        else {
            rv.relocations.push_back(Reloc::new_bytes(r.ofs, r.len, r.bytes));
        }
    }
    return rv;
}

void EncodedLiteral::write_uint(size_t ofs, size_t size,  uint64_t v)
{
    assert(ofs + size <= bytes.size());
    for(size_t i = 0; i < size; i ++) {
        size_t bit = (Target_GetCurSpec().m_arch.m_big_endian ? (size-1-i)*8 : i*8);
        if(bit < 64)
        {
            auto b = static_cast<uint8_t>(v >> bit);
            bytes[ofs + i] = b;
        }
    }
}
void EncodedLiteral::write_usize(size_t ofs,  uint64_t v)
{
    this->write_uint(ofs, Target_GetPointerBits() / 8, v);
}
uint64_t EncodedLiteral::read_usize(size_t ofs) const
{
    return EncodedLiteralSlice(*this).slice(ofs).read_uint(Target_GetPointerBits() / 8);
}
uint64_t EncodedLiteralSlice::read_uint(size_t size/*=0*/) const {
    if(size == 0)   size = m_size;
    ASSERT_BUG(Span(), size <= m_size, "Over-large read (" << size << " > " << m_size << ")");
    uint64_t v = 0;
    for(size_t i = 0; i < size; i ++) {
        size_t bit = (Target_GetCurSpec().m_arch.m_big_endian ? (size-1-i)*8 : i*8 );
        if(bit < 64)
            v |= static_cast<uint64_t>(m_base.bytes[m_ofs + i]) << bit;
    }
    DEBUG("("<<size<<") = " << v);
    return v;
}
int64_t EncodedLiteralSlice::read_sint(size_t size/*=0*/) const {
    auto v = read_uint(size);
    if(size < 64/8 && v >> (8*size-1) ) {
        // Sign extend
        v |= INT64_MAX << (8*size);
    }
    DEBUG("("<<size<<") = " << v);
    return v;
}
double EncodedLiteralSlice::read_float(size_t size/*=0*/) const {
    if(size == 0)   size = m_size;
    assert(size <= m_size);
    switch(size)
    {
    case 4: { float v; memcpy(&v, &m_base.bytes[m_ofs], 4); return v; }
    case 8: { double v; memcpy(&v, &m_base.bytes[m_ofs], 8); return v; }
    default: abort();
    }
}
const Reloc* EncodedLiteralSlice::get_reloc() const {
    for(const auto& r : m_base.relocations) {
        if(r.ofs == m_ofs)
            return &r;
    }
    return nullptr;
}

bool EncodedLiteralSlice::operator==(const EncodedLiteralSlice& x) const
{
    if(m_size != x.m_size)
        return false;
    for(size_t i = 0; i < m_size; i ++)
        if(m_base.bytes[m_ofs + i] != x.m_base.bytes[x.m_ofs + i])
            return false;
    auto it1 = std::find_if(  m_base.relocations.begin(),   m_base.relocations.end(), [&](const Reloc& r){ return r.ofs >=   m_ofs; });
    auto it2 = std::find_if(x.m_base.relocations.begin(), x.m_base.relocations.end(), [&](const Reloc& r){ return r.ofs >= x.m_ofs; });
    for(; it1 != m_base.relocations.end() && it2 != x.m_base.relocations.end(); ++it1, ++it2)
    {
        if( it1->ofs - m_ofs != it2->ofs - x.m_ofs )
            return false;
        if( it1->len != it2->len )
            return false;
        if( bool(it1->p) != bool(it2->p) )
            return false;
        if( it1->p )
        {
            if( *it1->p != *it2->p )
                return false;
        }
        else
        {
            if(it1->bytes != it2->bytes)
                return false;
        }
    }
    return true;
}

Ordering EncodedLiteralSlice::ord(const EncodedLiteralSlice& x) const
{
    // NOTE: Check the data first (to maintain some level of lexical ordering)
    auto min_size = std::min(m_size, x.m_size);
    for(size_t i = 0; i < min_size; i ++)
        if(auto cmp = ::ord(m_base.bytes[m_ofs + i], x.m_base.bytes[x.m_ofs + i]))
            return cmp;
    if( auto cmp = ::ord(m_size, x.m_size) )
        return cmp;

    auto it1 = std::find_if(  m_base.relocations.begin(),   m_base.relocations.end(), [&](const Reloc& r){ return r.ofs >=   m_ofs; });
    auto it2 = std::find_if(x.m_base.relocations.begin(), x.m_base.relocations.end(), [&](const Reloc& r){ return r.ofs >= x.m_ofs; });

    for(; it1 != m_base.relocations.end() && it2 != x.m_base.relocations.end(); ++it1, ++it2)
    {
        if( auto cmp = ::ord(it1->ofs - m_ofs, it2->ofs - x.m_ofs) )
            return cmp;
        if( auto cmp = ::ord(it1->len, it2->len) )
            return cmp;
        if( auto cmp = ::ord(bool(it1->p), bool(it2->p)) )
            return cmp;
        if( it1->p )
        {
            if( auto cmp = ::ord(*it1->p, *it2->p) )
                return cmp;
        }
        else
        {
            if(auto cmp = ::ord(it1->bytes, it2->bytes) )
                return cmp;
        }
    }
    return OrdEqual;
}

::std::ostream& operator<<(std::ostream& os, const EncodedLiteralSlice& x) {
    auto it = std::find_if(x.m_base.relocations.begin(), x.m_base.relocations.end(), [&](const Reloc& r){ return r.ofs >= x.m_ofs; });
    for(size_t i = 0; i < x.m_size; i++)
    {
        const char* HEX = "0123456789ABCDEF";
        auto o = x.m_ofs + i;
        auto b = x.m_base.bytes[o];
        if( it != x.m_base.relocations.end() && it->ofs == o ) {
            auto& r = *it;
            if(r.p) {
                os << "&" << *r.p;
            }
            else {
                os << "\"" << FmtEscaped(r.bytes) << "\"";
            }
            ++ it;
        }
        os << HEX[b>>4] << HEX[b&0xF];
        if( (i+1)%8 == 0 && i + 1 < x.m_size ) {
            os << " ";
        }
    }
    return os;
}

