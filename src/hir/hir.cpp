/*
 */
#include "hir.hpp"

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
        (Integer,
            os << e;
            ),
        (Float,
            os << e;
            ),
        (BorrowOf,
            os << "&" << e;
            ),
        (String,
            os << "\"" << e << "\"";
            )
        )
        return os;
    }
}

namespace {
    bool matches_genericpath(const ::HIR::GenericParams& params, const ::HIR::GenericPath& left, const ::HIR::GenericPath& right, ::HIR::t_cb_resolve_type ty_res, bool expand_generic);
    
    bool matches_type_int(const ::HIR::GenericParams& params, const ::HIR::TypeRef& left,  const ::HIR::TypeRef& right_in, ::HIR::t_cb_resolve_type ty_res, bool expand_generic)
    {
        assert(! left.m_data.is_Infer() );
        const auto& right = (right_in.m_data.is_Infer() || right_in.m_data.is_Generic() == expand_generic ? ty_res(right_in) : right_in);
        if( right_in.m_data.is_Generic() )
            expand_generic = false;

        //DEBUG("left = " << left << ", right = " << right);
        
        // TODO: What indicates what out of ty_res?
        
        if( right.m_data.is_Infer() ) {
            //DEBUG("left = " << left << ", right = " << right);
            switch(right.m_data.as_Infer().ty_class)
            {
            case ::HIR::InferClass::None:
            case ::HIR::InferClass::Diverge:
                return left.m_data.is_Generic();
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
            return left.m_data.is_Generic();
            //return true;
        }
        if( right.m_data.is_Generic() ) {
            return left.m_data.is_Generic();
        }
        
        if( left.m_data.is_Generic() ) {
            // True? (TODO: Check bounds?)
            return true;
        }
        
        if( left.m_data.tag() != right.m_data.tag() ) {
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
            DEBUG("TODO: Compare " << left << " and " << right);
            return false;
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
        
        if( left.m_params.m_types.size() > 0 || right.m_params.m_types.size() > 0 ) {
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

bool ::HIR::TraitImpl::matches_type(const ::HIR::TypeRef& type, ::HIR::t_cb_resolve_type ty_res) const
{
    return matches_type_int(m_params, m_type, type, ty_res, true);
    // TODO: Using this would be nice, but may be better to handle impl params better
    //return m_type.compare_with_placeholders(Span(), type, ty_res) != ::HIR::Compare::Unequal;
}
bool ::HIR::TypeImpl::matches_type(const ::HIR::TypeRef& type, ::HIR::t_cb_resolve_type ty_res) const
{
    return matches_type_int(m_params, m_type, type, ty_res, true);
}
bool ::HIR::MarkerImpl::matches_type(const ::HIR::TypeRef& type, ::HIR::t_cb_resolve_type ty_res) const
{
    return matches_type_int(m_params, m_type, type, ty_res, true);
}

namespace {
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
            TODO(sp, "Path - " << le.path);
            ),
        (TraitObject,
            TODO(sp, "TraitObject - " << left);
            ),
        (Function,
            TU_IFLET(::HIR::TypeRef::Data, right.m_data, Function, re,
                TODO(sp, "Function");
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
        for(unsigned int i = 0; i < le.size(); i ++) {
            auto a = type_ord_specific(sp, le[i], re[i]);
            if( a != ::OrdEqual ) {
                if( rv != ::OrdEqual && a != rv )
                    BUG(sp, "Inconsistent ordering between type lists");
                rv = a;
            }
        }
        return rv;
    }
}

bool ::HIR::TraitImpl::more_specific_than(const ::HIR::TraitImpl& other) const
{
    static const Span   _sp;
    const Span& sp = _sp;
    
    // >> https://github.com/rust-lang/rfcs/blob/master/text/1210-impl-specialization.md#defining-the-precedence-rules
    // 1. If this->m_type is less specific than other.m_type: return false
    if( type_ord_specific(sp, this->m_type, other.m_type) == ::OrdLess ) {
        return false;
    }
    // 2. If any in te.impl->m_params is less specific than oe.impl->m_params: return false
    if( typelist_ord_specific(sp, this->m_trait_args.m_types, other.m_trait_args.m_types) == ::OrdLess ) {
        return false;
    }
    
    //assert(m_params.m_types.size() == other.m_params.m_types.size());
    
    if( other.m_params.m_bounds.size() == 0 ) {
        return m_params.m_bounds.size() > 0;
    }
    // 3. Compare bound set, if there is a rule in oe that is missing from te; return false
    // 3a. Compare for rules in te that are missing from oe

    TODO(Span(), "TraitImpl - " << m_params.fmt_bounds() << " VERSUS " << other.m_params.fmt_bounds());// ( `" << *this << "` '>' `" << other << "`)");
}



const ::HIR::SimplePath& ::HIR::Crate::get_lang_item_path(const Span& sp, const char* name) const
{
    // TODO: have map stored in crate populated by (or from) the #[lang] attribute handler
    auto it = this->m_lang_items.find( name );
    if( it == this->m_lang_items.end() ) {
        ERROR(sp, E0000, "Undefined language item '" << name << "' required");
    }
    return it->second;
}

const ::HIR::TypeItem& ::HIR::Crate::get_typeitem_by_path(const Span& sp, const ::HIR::SimplePath& path) const
{
    if( path.m_components.size() == 0) {
        BUG(sp, "get_typeitem_by_path received invalid path");
    }
    if( path.m_crate_name != "" )
        TODO(sp, "::HIR::Crate::get_typeitem_by_path in extern crate");
    
    const ::HIR::Module* mod = &this->m_root_module;
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
    auto it = mod->m_mod_items.find( path.m_components.back() );
    if( it == mod->m_mod_items.end() ) {
        BUG(sp, "Could not find type name in " << path);
    }
    
    return it->second->ent;
}

const ::HIR::Module& ::HIR::Crate::get_mod_by_path(const Span& sp, const ::HIR::SimplePath& path) const
{
    const auto& ti = this->get_typeitem_by_path(sp, path);
    TU_IFLET(::HIR::TypeItem, ti, Module, e,
        return e;
    )
    else {
        BUG(sp, "Module path " << path << " didn't point to a module");
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

const ::HIR::ValueItem& ::HIR::Crate::get_valitem_by_path(const Span& sp, const ::HIR::SimplePath& path) const
{
    if( path.m_components.size() == 0) {
        BUG(sp, "get_valitem_by_path received invalid path");
    }
    if( path.m_crate_name != "" )
        TODO(sp, "::HIR::Crate::get_valitem_by_path in extern crate");
    
    const ::HIR::Module* mod = &this->m_root_module;
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
        BUG(sp, "Could not find type name in " << path);
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
        BUG(sp, "Enum path " << path << " didn't point to an enum");
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
    return false;
}
bool ::HIR::Crate::find_type_impls(const ::HIR::TypeRef& type, t_cb_resolve_type ty_res, ::std::function<bool(const ::HIR::TypeImpl&)> callback) const
{
    for( const auto& impl : this->m_type_impls )
    {
        if( impl.matches_type(type, ty_res) ) {
            if( callback(impl) ) {
                return true;
            }
        }
    }
    return false;
}
