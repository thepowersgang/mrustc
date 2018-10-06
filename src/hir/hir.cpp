/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/hir.cpp
 * - Processed module tree (High-level Intermediate Representation)
 *
 * HIR type helper code
 */
#include "hir.hpp"
#include <algorithm>
#include <hir_typeck/common.hpp>
#include <hir_typeck/expr_visit.hpp>    // for invoking typecheck
#include "item_path.hpp"
#include "expr_state.hpp"
#include <hir_expand/main_bindings.hpp>
#include <mir/main_bindings.hpp>

namespace HIR {
    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::Literal& v)
    {
        TU_MATCH(::HIR::Literal, (v), (e),
        (Invalid,
            os << "!";
            ),
        (List,
            os << "[";
            for(const auto& val : e)
                os << " " << val << ",";
            os << " ]";
            ),
        (Variant,
            os << "#" << e.idx << ":" << *e.val;
            ),
        (Integer,
            os << e;
            ),
        (Float,
            os << e;
            ),
        (BorrowPath,
            os << "&" << e;
            ),
        (BorrowData,
            os << "&" << *e;
            ),
        (String,
            os << "\"" << e << "\"";
            )
        )
        return os;
    }

    bool operator==(const Literal& l, const Literal& r)
    {
        if( l.tag() != r.tag() )
            return false;
        TU_MATCH(::HIR::Literal, (l,r), (le,re),
        (Invalid,
            ),
        (List,
            if( le.size() != re.size() )
                return false;
            for(unsigned int i = 0; i < le.size(); i ++)
                if( le[i] != re[i] )
                    return false;
            ),
        (Variant,
            if( le.idx != re.idx )
                return false;
            return *le.val == *re.val;
            ),
        (Integer,
            return le == re;
            ),
        (Float,
            return le == re;
            ),
        (BorrowPath,
            return le == re;
            ),
        (BorrowData,
            return *le == *re;
            ),
        (String,
            return le == re;
            )
        )
        return true;
    }
}

size_t HIR::Enum::find_variant(const ::std::string& name) const
{
    if( m_data.is_Value() )
    {
        const auto& e = m_data.as_Value();
        auto it = ::std::find_if(e.variants.begin(), e.variants.end(), [&](const auto& x){ return x.name == name; });
        if( it == e.variants.end() )
            return SIZE_MAX;
        return it - e.variants.begin();
    }
    else
    {
        const auto& e = m_data.as_Data();

        auto it = ::std::find_if(e.begin(), e.end(), [&](const auto& x){ return x.name == name; });
        if( it == e.end() )
            return SIZE_MAX;
        return it - e.begin();
    }
}
bool HIR::Enum::is_value() const
{
    return this->m_data.is_Value();
}
uint32_t HIR::Enum::get_value(size_t idx) const
{
    if( m_data.is_Value() )
    {
        const auto& e = m_data.as_Value();
        assert(idx < e.variants.size());

        return e.variants[idx].val;
    }
    else
    {
        assert(!"TODO: Enum::get_value on non-value enum?");
        throw "";
    }
}

namespace {
    bool matches_genericpath(const ::HIR::GenericParams& params, const ::HIR::GenericPath& left, const ::HIR::GenericPath& right, ::HIR::t_cb_resolve_type ty_res, bool expand_generic);

    bool matches_type_int(const ::HIR::GenericParams& params, const ::HIR::TypeRef& left,  const ::HIR::TypeRef& right_in, ::HIR::t_cb_resolve_type ty_res, bool expand_generic)
    {
        assert(! left.m_data.is_Infer() );
        const auto& right = (right_in.m_data.is_Infer() || (right_in.m_data.is_Generic() && expand_generic) ? ty_res(right_in) : right_in);
        if( right_in.m_data.is_Generic() )
            expand_generic = false;

        //DEBUG("left = " << left << ", right = " << right);

        // TODO: What indicates what out of ty_res?

        if( const auto* re = right.m_data.opt_Infer() )
        {
            //DEBUG("left = " << left << ", right = " << right);
            switch(re->ty_class)
            {
            case ::HIR::InferClass::None:
            case ::HIR::InferClass::Diverge:
                //return left.m_data.is_Generic();
                return true;
            case ::HIR::InferClass::Integer:
                TU_IFLET(::HIR::TypeRef::Data, left.m_data, Primitive, le,
                    return is_integer(le);
                )
                else {
                    return left.m_data.is_Generic();
                }
                break;
            case ::HIR::InferClass::Float:
                TU_IFLET(::HIR::TypeRef::Data, left.m_data, Primitive, le,
                    return is_float(le);
                )
                else {
                    return left.m_data.is_Generic();
                }
                break;
            }
            throw "";
        }

        // A local generic could match anything, leave that up to the caller
        if( left.m_data.is_Generic() ) {
            DEBUG("> Generic left, success");
            return true;
        }
        // A local UfcsKnown can only be becuase it couldn't be expanded earlier, assume it could match
        if( left.m_data.is_Path() && left.m_data.as_Path().path.m_data.is_UfcsKnown() ) {
            // True?
            //DEBUG("> UFCS Unknown left, success");
            return true;
        }

        // If the RHS (provided) is generic, it can only match if it binds to a local type parameter
        if( right.m_data.is_Generic() ) {
            // TODO: This is handled above?
            //DEBUG("> Generic right, only if left generic");
            return left.m_data.is_Generic();
        }
        // If the RHS (provided) is generic, it can only match if it binds to a local type parameter
        if( TU_TEST1(right.m_data, Path, .binding.is_Unbound()) ) {
            //DEBUG("> UFCS Unknown right, fuzzy");
            return true;
        }

        if( left.m_data.tag() != right.m_data.tag() ) {
            //DEBUG("> Tag mismatch, failure");
            return false;
        }
        TU_MATCH(::HIR::TypeRef::Data, (left.m_data, right.m_data), (le, re),
        (Infer, assert(!"infer");),
        (Diverge, return true; ),
        (Primitive, return le == re;),
        (Path,
            if( le.path.m_data.tag() != re.path.m_data.tag() )
                return false;
            TU_MATCH_DEF(::HIR::Path::Data, (le.path.m_data, re.path.m_data), (ple, pre),
            (
                return false;
                ),
            (Generic,
                return matches_genericpath(params, ple, pre, ty_res, expand_generic);
                )
            )
            ),
        (Generic,
            throw "";
            ),
        (TraitObject,
            if( !matches_genericpath(params, le.m_trait.m_path, re.m_trait.m_path, ty_res, expand_generic) )
                return false;
            if( le.m_markers.size() != re.m_markers.size() )
                return false;
            for(unsigned int i = 0; i < le.m_markers.size(); i ++)
            {
                const auto& lm = le.m_markers[i];
                const auto& rm = re.m_markers[i];
                if( !matches_genericpath(params, lm, rm, ty_res, expand_generic) )
                    return false;
            }
            return true;
            ),
        (ErasedType,
            throw "Unexpected ErasedType in matches_type_int";
            ),
        (Array,
            if( ! matches_type_int(params, *le.inner, *re.inner, ty_res, expand_generic) )
                return false;
            if( le.size_val != re.size_val )
                return false;
            return true;
            ),
        (Slice,
            return matches_type_int(params, *le.inner, *re.inner, ty_res, expand_generic);
            ),
        (Tuple,
            if( le.size() != re.size() )
                return false;
            for( unsigned int i = 0; i < le.size(); i ++ )
                if( !matches_type_int(params, le[i], re[i], ty_res, expand_generic) )
                    return false;
            return true;
            ),
        (Borrow,
            if( le.type != re.type )
                return false;
            return matches_type_int(params, *le.inner, *re.inner, ty_res, expand_generic);
            ),
        (Pointer,
            if( le.type != re.type )
                return false;
            return matches_type_int(params, *le.inner, *re.inner, ty_res, expand_generic);
            ),
        (Function,
            if( le.is_unsafe != re.is_unsafe )
                return false;
            if( le.m_abi != re.m_abi )
                return false;
            if( le.m_arg_types.size() != re.m_arg_types.size() )
                return false;
            for( unsigned int i = 0; i < le.m_arg_types.size(); i ++ )
                if( !matches_type_int(params, le.m_arg_types[i], re.m_arg_types[i], ty_res, expand_generic) )
                    return false;
            return matches_type_int(params, *le.m_rettype, *re.m_rettype, ty_res, expand_generic);
            ),
        (Closure,
            return le.node == re.node;
            )
        )
        return false;
    }
    bool matches_genericpath(const ::HIR::GenericParams& params, const ::HIR::GenericPath& left, const ::HIR::GenericPath& right, ::HIR::t_cb_resolve_type ty_res, bool expand_generic)
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
                return true;
                //TODO(Span(), "Match generic paths " << left << " and " << right << " - count mismatch");
            }
            for( unsigned int i = 0; i < right.m_params.m_types.size(); i ++ )
            {
                if( ! matches_type_int(params, left.m_params.m_types[i], right.m_params.m_types[i], ty_res, expand_generic) )
                    return false;
            }
        }
        return true;
    }
}

//::HIR::TypeRef HIR::Function::make_ty(const Span& sp, const ::HIR::PathParams& params) const
//{
//    // TODO: Obtain function type for this function (i.e. a type that is specifically for this function)
//    auto fcn_ty_data = ::HIR::FunctionType {
//        m_is_unsafe,
//        m_abi,
//        box$( monomorphise_type(sp, m_params, params,  m_return) ),
//        {}
//        };
//    fcn_ty_data.m_arg_types.reserve( m_args.size() );
//    for(const auto& arg : m_args)
//    {
//        fcn_ty_data.m_arg_types.push_back( monomorphise_type(sp, m_params, params,  arg.second) );
//    }
//    return ::HIR::TypeRef( mv$(fcn_ty_data) );
//}

namespace {
    bool is_unbounded_infer(const ::HIR::TypeRef& type) {
        TU_IFLET( ::HIR::TypeRef::Data, type.m_data, Infer, e,
            return e.ty_class == ::HIR::InferClass::None || e.ty_class == ::HIR::InferClass::Diverge;
        )
        else {
            return false;
        }
    }
}

bool ::HIR::TraitImpl::matches_type(const ::HIR::TypeRef& type, ::HIR::t_cb_resolve_type ty_res) const
{
    // NOTE: Don't return any impls when the type is an unbouned ivar. Wouldn't be able to pick anything anyway
    // TODO: For `Unbound`, it could be valid, if the target is a generic.
    if( is_unbounded_infer(type) || TU_TEST1(type.m_data, Path, .binding.is_Unbound()) ) {
        return false;
    }
    return matches_type_int(m_params, m_type, type, ty_res, true);
}
bool ::HIR::TypeImpl::matches_type(const ::HIR::TypeRef& type, ::HIR::t_cb_resolve_type ty_res) const
{
    if( is_unbounded_infer(type) || TU_TEST1(type.m_data, Path, .binding.is_Unbound()) ) {
        return false;
    }
    return matches_type_int(m_params, m_type, type, ty_res, true);
}
bool ::HIR::MarkerImpl::matches_type(const ::HIR::TypeRef& type, ::HIR::t_cb_resolve_type ty_res) const
{
    if( is_unbounded_infer(type) || TU_TEST1(type.m_data, Path, .binding.is_Unbound()) ) {
        return false;
    }
    return matches_type_int(m_params, m_type, type, ty_res, true);
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
        if( left.m_data.is_Generic() ) {
            return right.m_data.is_Generic() ? ::OrdEqual : ::OrdLess;
        }
        // - A generic is always less specific than anything but itself (handled above)
        if( right.m_data.is_Generic() ) {
            return ::OrdGreater;
        }

        TU_MATCH(::HIR::TypeRef::Data, (left.m_data), (le),
        (Generic,
            throw "";
            ),
        (Infer,
            BUG(sp, "Hit infer");
            ),
        (Diverge,
            BUG(sp, "Hit diverge");
            ),
        (Closure,
            BUG(sp, "Hit closure");
            ),
        (Primitive,
            TU_IFLET(::HIR::TypeRef::Data, right.m_data, Primitive, re,
                if( le != re )
                    BUG(sp, "Mismatched types - " << left << " and " << right);
                return ::OrdEqual;
            )
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            ),
        (Path,
            if( !right.m_data.is_Path() || le.path.m_data.tag() != right.m_data.as_Path().path.m_data.tag() )
                BUG(sp, "Mismatched types - " << left << " and " << right);
            TU_MATCHA( (le.path.m_data, right.m_data.as_Path().path.m_data), (lpe, rpe),
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
            ),
        (TraitObject,
            ASSERT_BUG(sp, right.m_data.is_TraitObject(), "Mismatched types - "<< left << " vs " << right);
            const auto& re = right.m_data.as_TraitObject();
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
            ),
        (ErasedType,
            TODO(sp, "ErasedType - " << left);
            ),
        (Function,
            TU_IFLET(::HIR::TypeRef::Data, right.m_data, Function, re,
                if( left == right )
                    return ::OrdEqual;
                TODO(sp, "Function - " << left << " and " << right);
                //return typelist_ord_specific(sp, le.arg_types, re.arg_types);
            )
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            ),
        (Tuple,
            TU_IFLET(::HIR::TypeRef::Data, right.m_data, Tuple, re,
                return typelist_ord_specific(sp, le, re);
            )
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            ),
        (Slice,
            TU_IFLET(::HIR::TypeRef::Data, right.m_data, Slice, re,
                return type_ord_specific(sp, *le.inner, *re.inner);
            )
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            ),
        (Array,
            TU_IFLET(::HIR::TypeRef::Data, right.m_data, Array, re,
                if( le.size_val != re.size_val )
                    BUG(sp, "Mismatched types - " << left << " and " << right);
                return type_ord_specific(sp, *le.inner, *re.inner);
            )
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            ),
        (Pointer,
            TU_IFLET(::HIR::TypeRef::Data, right.m_data, Pointer, re,
                if( le.type != re.type )
                    BUG(sp, "Mismatched types - " << left << " and " << right);
                return type_ord_specific(sp, *le.inner, *re.inner);
            )
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            ),
        (Borrow,
            TU_IFLET(::HIR::TypeRef::Data, right.m_data, Borrow, re,
                if( le.type != re.type )
                    BUG(sp, "Mismatched types - " << left << " and " << right);
                return type_ord_specific(sp, *le.inner, *re.inner);
            )
            else {
                BUG(sp, "Mismatched types - " << left << " and " << right);
            }
            )
        )
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
        auto monomorph_cb = monomorphise_type_get_cb(sp, &type, &cur_trait.m_path.m_params, nullptr);

        for(const auto& trait_path_raw : tr.m_all_parent_traits)
        {
            // 1. Monomorph
            auto trait_path_mono = monomorphise_traitpath_with(sp, trait_path_raw, monomorph_cb, false);
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
        auto ord = type_ord_specific(sp, this->m_type, other.m_type);
        // If `*this` < `other` : false
        if( ord != ::OrdEqual ) {
            DEBUG("- Type " << this->m_type << " " << (ord == ::OrdLess ? "less" : "more") << " specific than " << other.m_type);
            return ord == ::OrdGreater;
        }
        // 2. If any in te.impl->m_params is less specific than oe.impl->m_params: return false
        ord = typelist_ord_specific(sp, this->m_trait_args.m_types, other.m_trait_args.m_types);
        if( ord != ::OrdEqual ) {
            DEBUG("- Trait arguments " << (ord == ::OrdLess ? "less" : "more") << " specific");
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
    while( it_t != bounds_t.end() && it_o != bounds_o.end() )
    {
        auto cmp = ::ord(*it_t, *it_o);
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
            ++ it_t;
        }
        else
        {
            //++ it_o;
            return false;
        }
    }
    if( it_t != bounds_t.end() )
    {
        return true;
    }
    else
    {
        return false;
    }
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
            if( a.m_data.is_Generic() || b.m_data.is_Generic() )
                return true;
            // TODO: Unbound/Opaque paths?
            if( a.m_data.tag() != b.m_data.tag() )
                return false;
            TU_MATCHA( (a.m_data, b.m_data), (ae, be),
            (Generic,
                ),
            (Infer,
                ),
            (Diverge,
                ),
            (Closure,
                BUG(sp, "Hit closure");
                ),
            (Primitive,
                if( ae != be )
                    return false;
                ),
            (Path,
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
                ),
            (TraitObject,
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
                ),
            (ErasedType,
                TODO(sp, "ErasedType - " << a);
                ),
            (Function,
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
                ),
            (Tuple,
                if( ae.size() != be.size() )
                    return false;
                for(unsigned int i = 0; i < ae.size(); i ++)
                {
                    if( ! H::types_overlap(ae[i], be[i]) )
                        return false;
                }
                ),
            (Slice,
                return H::types_overlap( *ae.inner, *be.inner );
                ),
            (Array,
                if( ae.size_val != be.size_val )
                    return false;
                return H::types_overlap( *ae.inner, *be.inner );
                ),
            (Pointer,
                if( ae.type != be.type )
                    return false;
                return H::types_overlap( *ae.inner, *be.inner );
                ),
            (Borrow,
                if( ae.type != be.type )
                    return false;
                return H::types_overlap( *ae.inner, *be.inner );
                )
            )
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
    ::std::vector<const ::HIR::TypeRef*>    impl_tys;
    auto cb_match = [&](unsigned int idx, const auto& /*name*/, const ::HIR::TypeRef& x)->::HIR::Compare {
        assert(idx < impl_tys.size());
        if( impl_tys.at(idx) )
        {
            DEBUG("Compare " << x << " and " << *impl_tys.at(idx));
            return (x == *impl_tys.at(idx) ? ::HIR::Compare::Equal : ::HIR::Compare::Unequal);
        }
        else
        {
            impl_tys.at(idx) = &x;
            return ::HIR::Compare::Equal;
        }
        };
    impl_tys.resize( this->m_params.m_types.size() );
    if( ! this->m_type.match_test_generics(sp, other.m_type, cb_ident, cb_match) )
    {
        DEBUG("- Type mismatch, try other ordering");
        is_reversed = true;
        impl_tys.clear(); impl_tys.resize( other.m_params.m_types.size() );
        if( !other.m_type.match_test_generics(sp, this->m_type, cb_ident, cb_match) )
        {
            DEBUG("- Type mismatch in both orderings");
            return false;
        }
        if( other.m_trait_args.match_test_generics_fuzz(sp, this->m_trait_args, cb_ident, cb_match) != ::HIR::Compare::Equal )
        {
            DEBUG("- Params mismatch");
            return false;
        }
        // Matched with second ording
    }
    else if( this->m_trait_args.match_test_generics_fuzz(sp, other.m_trait_args, cb_ident, cb_match) != ::HIR::Compare::Equal )
    {
        DEBUG("- Param mismatch, try other ordering");
        is_reversed = true;
        impl_tys.clear(); impl_tys.resize( other.m_params.m_types.size() );
        if( !other.m_type.match_test_generics(sp, this->m_type, cb_ident, cb_match) )
        {
            DEBUG("- Type mismatch in alt ordering");
            return false;
        }
        if( other.m_trait_args.match_test_generics_fuzz(sp, this->m_trait_args, cb_ident, cb_match) != ::HIR::Compare::Equal )
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
        static const ::HIR::TypeRef& monomorph(const Span& sp, const ::HIR::TypeRef& in_ty, const ::std::vector<const ::HIR::TypeRef*>& args, ::HIR::TypeRef& tmp)
        {
            if( ! monomorphise_type_needed(in_ty) ) {
                return in_ty;
            }
            else if( const auto* tep = in_ty.m_data.opt_Generic() ) {
                ASSERT_BUG(sp, tep->binding < args.size(), "");
                ASSERT_BUG(sp, args[tep->binding], "");
                return *args[tep->binding];
            }
            else {
                auto monomorph_cb = [&](const auto& t)->const auto& {
                    const auto& te = t.m_data.as_Generic();
                    assert(te.binding < args.size());
                    ASSERT_BUG(sp, te.binding < args.size(), "");
                    ASSERT_BUG(sp, args[te.binding], "");
                    return *args[te.binding];
                    };
                tmp = monomorphise_type_with(sp, in_ty, monomorph_cb);
                // TODO: EAT?
                return tmp;
            }
        }
        static const ::HIR::TraitPath& monomorph(const Span& sp, const ::HIR::TraitPath& in, const ::std::vector<const ::HIR::TypeRef*>& args, ::HIR::TraitPath& tmp)
        {
            if( ! monomorphise_traitpath_needed(in) ) {
                return in;
            }
            else {
                auto monomorph_cb = [&](const auto& t)->const auto& {
                    const auto& te = t.m_data.as_Generic();
                    assert(te.binding < args.size());
                    ASSERT_BUG(sp, te.binding < args.size(), "");
                    ASSERT_BUG(sp, args[te.binding], "");
                    return *args[te.binding];
                    };
                tmp = monomorphise_traitpath_with(sp, in, monomorph_cb, true);
                // TODO: EAT?
                return tmp;
            }
        }
        static bool check_bounds(const ::HIR::Crate& crate, const ::HIR::TraitImpl& id, const ::std::vector<const ::HIR::TypeRef*>& args, const ::HIR::TraitImpl& g_src)
        {
            TRACE_FUNCTION;
            static Span sp;
            for(const auto& tb : id.m_params.m_bounds)
            {
                if(tb.is_TraitBound())
                {
                    ::HIR::TypeRef  tmp_ty;
                    ::HIR::TraitPath    tmp_tp;
                    const auto& ty = H2::monomorph(sp, tb.as_TraitBound().type, args, tmp_ty);
                    const auto& trait = H2::monomorph(sp, tb.as_TraitBound().trait, args, tmp_tp);;

                    // Determine if `ty` would be bounded (it's an ATY or generic)
                    if( ty.m_data.is_Generic() ) {
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
                    else if( TU_TEST1(ty.m_data, Path, .binding.is_Opaque()) ) {
                        TODO(Span(), "Check bound " << ty << " : " << trait << " in source bounds or trait bounds");
                    }
                    else {
                        // Search the crate for an impl
                        bool rv = crate.find_trait_impls(trait.m_path.m_path, ty, [](const auto&t)->const auto&{ return t; }, [&](const ::HIR::TraitImpl& ti)->bool {
                                DEBUG("impl" << ti.m_params.fmt_args() << " " << trait.m_path.m_path << ti.m_trait_args << " for " << ti.m_type << ti.m_params.fmt_bounds());

                                ::std::vector<const ::HIR::TypeRef*>   impl_tys { ti.m_params.m_types.size() };
                                auto cb_ident = [](const ::HIR::TypeRef& x)->const ::HIR::TypeRef& { return x; };
                                auto cb_match = [&](unsigned int idx, const auto& /*name*/, const ::HIR::TypeRef& x)->::HIR::Compare {
                                    assert(idx < impl_tys.size());
                                    if( impl_tys.at(idx) )
                                    {
                                        DEBUG("Compare " << x << " and " << *impl_tys.at(idx));
                                        return (x == *impl_tys.at(idx) ? ::HIR::Compare::Equal : ::HIR::Compare::Unequal);
                                    }
                                    else
                                    {
                                        impl_tys.at(idx) = &x;
                                        return ::HIR::Compare::Equal;
                                    }
                                    };
                                // 1. Triple-check the type matches (and get generics)
                                if( ! ti.m_type.match_test_generics(sp, ty, cb_ident, cb_match) )
                                    return false;
                                // 2. Check trait params
                                assert(trait.m_path.m_params.m_types.size() == ti.m_trait_args.m_types.size());
                                for(size_t i = 0; i < trait.m_path.m_params.m_types.size(); i ++)
                                {
                                    if( !ti.m_trait_args.m_types[i].match_test_generics(sp, trait.m_path.m_params.m_types[i], cb_ident, cb_match) )
                                        return false;
                                }
                                // 3. Check bounds on the impl
                                if( !H2::check_bounds(crate, ti, impl_tys, g_src) )
                                    return false;
                                // 4. Check ATY bounds on the trait path
                                for(const auto& atyb : trait.m_type_bounds)
                                {
                                    const auto& aty = ti.m_types.at(atyb.first);
                                    if( !aty.data.match_test_generics(sp, atyb.second, cb_ident, cb_match) )
                                        return false;
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
                for(auto* p : impl_tys)
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
        return H2::check_bounds(crate, other, impl_tys, *this);
    }
    else
    {
        DEBUG("impl params " << FMT_CB(os,
                for(auto* p : impl_tys)
                {
                    if(p)
                        os << *p;
                    else
                        os << "?";
                    os << ",";
                }
                ));
        // Check bounds on `*this`
        return H2::check_bounds(crate, *this, impl_tys, other);
    }
}


const ::HIR::SimplePath& ::HIR::Crate::get_lang_item_path(const Span& sp, const char* name) const
{
    auto it = this->m_lang_items.find( name );
    if( it == this->m_lang_items.end() ) {
        ERROR(sp, E0000, "Undefined language item '" << name << "' required");
    }
    return it->second;
}
const ::HIR::SimplePath& ::HIR::Crate::get_lang_item_path_opt(const char* name) const
{
    static ::HIR::SimplePath    empty_path;
    auto it = this->m_lang_items.find( name );
    if( it == this->m_lang_items.end() ) {
        return empty_path;
    }
    return it->second;
}

const ::HIR::TypeItem& ::HIR::Crate::get_typeitem_by_path(const Span& sp, const ::HIR::SimplePath& path, bool ignore_crate_name, bool ignore_last_node) const
{
    ASSERT_BUG(sp, path.m_components.size() > 0u, "get_typeitem_by_path received invalid path - " << path);
    ASSERT_BUG(sp, path.m_components.size() > (ignore_last_node ? 1u : 0u), "get_typeitem_by_path received invalid path - " << path);

    const ::HIR::Module* mod;
    if( !ignore_crate_name && path.m_crate_name != m_crate_name ) {
        ASSERT_BUG(sp, m_ext_crates.count(path.m_crate_name) > 0, "Crate '" << path.m_crate_name << "' not loaded for " << path);
        mod = &m_ext_crates.at(path.m_crate_name).m_data->m_root_module;
    }
    else {
        mod =  &this->m_root_module;
    }
    for( unsigned int i = 0; i < path.m_components.size() - (ignore_last_node ? 2 : 1); i ++ )
    {
        const auto& pc = path.m_components[i];
        auto it = mod->m_mod_items.find( pc );
        if( it == mod->m_mod_items.end() ) {
            BUG(sp, "Couldn't find component " << i << " of " << path);
        }
        TU_IFLET(::HIR::TypeItem, it->second->ent, Module, e,
            mod = &e;
        )
        else {
            BUG(sp, "Node " << i << " of path " << path << " wasn't a module");
        }
    }
    auto it = mod->m_mod_items.find( ignore_last_node ? path.m_components[path.m_components.size()-2] : path.m_components.back() );
    if( it == mod->m_mod_items.end() ) {
        BUG(sp, "Could not find type name in " << path);
    }

    return it->second->ent;
}

const ::HIR::Module& ::HIR::Crate::get_mod_by_path(const Span& sp, const ::HIR::SimplePath& path, bool ignore_last_node/*=false*/) const
{
    if( ignore_last_node )
    {
        ASSERT_BUG(sp, path.m_components.size() > 0, "get_mod_by_path received invalid path with ignore_last_node=true - " << path);
    }
    if( path.m_components.size() == (ignore_last_node ? 1 : 0) )
    {
        if( path.m_crate_name != m_crate_name )
        {
            ASSERT_BUG(sp, m_ext_crates.count(path.m_crate_name) > 0, "Crate '" << path.m_crate_name << "' not loaded");
            return m_ext_crates.at(path.m_crate_name).m_data->m_root_module;
        }
        else
        {
            return this->m_root_module;
        }
    }
    else
    {
        const auto& ti = this->get_typeitem_by_path(sp, path, false, ignore_last_node);
        TU_IFLET(::HIR::TypeItem, ti, Module, e,
            return e;
        )
        else {
            if( ignore_last_node )
                BUG(sp, "Parent path of " << path << " didn't point to a module");
            else
                BUG(sp, "Module path " << path << " didn't point to a module");
        }
    }
}
const ::HIR::Trait& ::HIR::Crate::get_trait_by_path(const Span& sp, const ::HIR::SimplePath& path) const
{
    const auto& ti = this->get_typeitem_by_path(sp, path);
    TU_IFLET(::HIR::TypeItem, ti, Trait, e,
        return e;
    )
    else {
        BUG(sp, "Trait path " << path << " didn't point to a trait");
    }
}
const ::HIR::Struct& ::HIR::Crate::get_struct_by_path(const Span& sp, const ::HIR::SimplePath& path) const
{
    const auto& ti = this->get_typeitem_by_path(sp, path);
    TU_IFLET(::HIR::TypeItem, ti, Struct, e,
        return e;
    )
    else {
        BUG(sp, "Struct path " << path << " didn't point to a struct");
    }
}
const ::HIR::Union& ::HIR::Crate::get_union_by_path(const Span& sp, const ::HIR::SimplePath& path) const
{
    const auto& ti = this->get_typeitem_by_path(sp, path);
    TU_IFLET(::HIR::TypeItem, ti, Union, e,
        return e;
    )
    else {
        BUG(sp, "Path " << path << " didn't point to a union");
    }
}
const ::HIR::Enum& ::HIR::Crate::get_enum_by_path(const Span& sp, const ::HIR::SimplePath& path) const
{
    const auto& ti = this->get_typeitem_by_path(sp, path);
    TU_IFLET(::HIR::TypeItem, ti, Enum, e,
        return e;
    )
    else {
        BUG(sp, "Enum path " << path << " didn't point to an enum");
    }
}

const ::HIR::ValueItem& ::HIR::Crate::get_valitem_by_path(const Span& sp, const ::HIR::SimplePath& path, bool ignore_crate_name) const
{
    if( path.m_components.size() == 0) {
        BUG(sp, "get_valitem_by_path received invalid path");
    }
    const ::HIR::Module* mod;
    if( !ignore_crate_name && path.m_crate_name != m_crate_name ) {
        ASSERT_BUG(sp, m_ext_crates.count(path.m_crate_name) > 0, "Crate '" << path.m_crate_name << "' not loaded");
        mod = &m_ext_crates.at(path.m_crate_name).m_data->m_root_module;
    }
    else {
        mod =  &this->m_root_module;
    }
    for( unsigned int i = 0; i < path.m_components.size() - 1; i ++ )
    {
        const auto& pc = path.m_components[i];
        auto it = mod->m_mod_items.find( pc );
        if( it == mod->m_mod_items.end() ) {
            BUG(sp, "Couldn't find component " << i << " of " << path);
        }
        TU_IFLET(::HIR::TypeItem, it->second->ent, Module, e,
            mod = &e;
        )
        else {
            BUG(sp, "Node " << i << " of path " << path << " wasn't a module");
        }
    }
    auto it = mod->m_value_items.find( path.m_components.back() );
    if( it == mod->m_value_items.end() ) {
        BUG(sp, "Could not find value name " << path);
    }

    return it->second->ent;
}
const ::HIR::Function& ::HIR::Crate::get_function_by_path(const Span& sp, const ::HIR::SimplePath& path) const
{
    const auto& ti = this->get_valitem_by_path(sp, path);
    TU_IFLET(::HIR::ValueItem, ti, Function, e,
        return e;
    )
    else {
        BUG(sp, "Function path " << path << " didn't point to an function");
    }
}

bool ::HIR::Crate::find_trait_impls(const ::HIR::SimplePath& trait, const ::HIR::TypeRef& type, t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::TraitImpl&)> callback) const
{
    auto its = this->m_trait_impls.equal_range( trait );
    for( auto it = its.first; it != its.second; ++ it )
    {
        const auto& impl = it->second;
        if( impl.matches_type(type, ty_res) ) {
            if( callback(impl) ) {
                return true;
            }
        }
    }
    for( const auto& ec : this->m_ext_crates )
    {
        if( ec.second.m_data->find_trait_impls(trait, type, ty_res, callback) ) {
            return true;
        }
    }
    return false;
}
bool ::HIR::Crate::find_auto_trait_impls(const ::HIR::SimplePath& trait, const ::HIR::TypeRef& type, t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::MarkerImpl&)> callback) const
{
    auto its = this->m_marker_impls.equal_range( trait );
    for( auto it = its.first; it != its.second; ++ it )
    {
        const auto& impl = it->second;
        if( impl.matches_type(type, ty_res) ) {
            if( callback(impl) ) {
                return true;
            }
        }
    }
    for( const auto& ec : this->m_ext_crates )
    {
        if( ec.second.m_data->find_auto_trait_impls(trait, type, ty_res, callback) ) {
            return true;
        }
    }
    return false;
}
bool ::HIR::Crate::find_type_impls(const ::HIR::TypeRef& type, t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::TypeImpl&)> callback) const
{
    // TODO: Restrict which crate is searched based on the type.
    for( const auto& impl : this->m_type_impls )
    {
        if( impl.matches_type(type, ty_res) ) {
            if( callback(impl) ) {
                return true;
            }
        }
    }
    for( const auto& ec : this->m_ext_crates )
    {
        //DEBUG("- " << ec.first);
        if( ec.second.m_data->find_type_impls(type, ty_res, callback) ) {
            return true;
        }
    }
    return false;
}

const ::MIR::Function* HIR::Crate::get_or_gen_mir(const ::HIR::ItemPath& ip, const ::HIR::ExprPtr& ep, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_ty) const
{
    if( !ep )
    {
        return &*ep.m_mir;
    }
    else
    {
        if( !ep.m_mir )
        {
            ASSERT_BUG(Span(), ep.m_state, "No ExprState for " << ip);

            auto& ep_mut = const_cast<::HIR::ExprPtr&>(ep);

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
                ms.m_impl_generics = ep.m_state->m_impl_generics;
                ms.m_item_generics = ep.m_state->m_item_generics;
                ms.m_traits = ep.m_state->m_traits;
                Typecheck_Code(ms, const_cast<::HIR::Function::args_t&>(args), ret_ty, ep_mut);
                //Debug_SetStagePre("Expand HIR Annotate");
                HIR_Expand_AnnotateUsage_Expr(*this, ep_mut);
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
