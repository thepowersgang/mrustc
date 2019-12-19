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
    ::std::ostream& operator<<(::std::ostream& os, const Publicity& x)
    {
        if( !x.vis_path ) {
            os << "pub";
        }
        else if( *x.vis_path == *Publicity::none_path ) {
            os << "priv";
        }
        else {
            os << "pub(" << *x.vis_path << ")";
        }
        return os;
    }

    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::Literal& v)
    {
        TU_MATCH(::HIR::Literal, (v), (e),
        (Invalid,
            os << "!";
            ),
        (Defer,
            os << "?";
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
        (Defer,
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

    ::std::ostream& operator<<(::std::ostream& os, const Struct::Repr& x) {
        os << "repr(";
        switch(x)
        {
        case Struct::Repr::Rust:    os << "Rust";   break;
        case Struct::Repr::C:   os << "C";  break;
        case Struct::Repr::Packed:  os << "packed"; break;
        case Struct::Repr::Simd:    os << "simd";   break;
        case Struct::Repr::Aligned: os << "align(?)";   break;
        case Struct::Repr::Transparent: os << "transparent";    break;
        }
        os << ")";
        return os;
    }
}

HIR::Literal HIR::Literal::clone() const
{
    TU_MATCH(::HIR::Literal, (*this), (e),
    (Invalid,
        return ::HIR::Literal();
        ),
    (Defer,
        return ::HIR::Literal::make_Defer({});
        ),
    (List,
        ::std::vector< ::HIR::Literal>  vals;
        for(const auto& val : e) {
            vals.push_back( val.clone() );
        }
        return ::HIR::Literal( mv$(vals) );
        ),
    (Variant,
        return ::HIR::Literal::make_Variant({ e.idx, box$(e.val->clone()) });
        ),
    (Integer,
        return ::HIR::Literal(e);
        ),
    (Float,
        return ::HIR::Literal(e);
        ),
    (BorrowPath,
        return ::HIR::Literal(e.clone());
        ),
    (BorrowData,
        return ::HIR::Literal(box$( e->clone() ));
        ),
    (String,
        return ::HIR::Literal(e);
        )
    )
    throw "";
}

::std::shared_ptr<::HIR::SimplePath> HIR::Publicity::none_path = ::std::make_shared<HIR::SimplePath>(::HIR::SimplePath{"#", {}});

bool HIR::Publicity::is_visible(const ::HIR::SimplePath& p) const
{
    // No path = global public
    if( !vis_path )
        return true;
    // Empty simple path = full private
    if( *vis_path == *none_path ) {
        return false;
    }
    // Crate names must match
    if(p.m_crate_name != vis_path->m_crate_name)
        return false;
    // `p` must be a child of vis_path
    if(p.m_components.size() < vis_path->m_components.size())
        return false;
    for(size_t i = 0; i < vis_path->m_components.size(); i ++)
    {
        if(p.m_components[i] != vis_path->m_components[i])
            return false;
    }
    return true;
}

size_t HIR::Enum::find_variant(const RcString& name) const
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
/*static*/ ::HIR::TypeRef HIR::Enum::get_repr_type(Repr r)
{
    switch(r)
    {
    case ::HIR::Enum::Repr::Rust:
    case ::HIR::Enum::Repr::C:
        return ::HIR::CoreType::Isize;
        break;
    case ::HIR::Enum::Repr::Usize: return ::HIR::CoreType::Usize; break;
    case ::HIR::Enum::Repr::U8 : return ::HIR::CoreType::U8 ; break;
    case ::HIR::Enum::Repr::U16: return ::HIR::CoreType::U16; break;
    case ::HIR::Enum::Repr::U32: return ::HIR::CoreType::U32; break;
    case ::HIR::Enum::Repr::U64: return ::HIR::CoreType::U64; break;
    }
    throw "";
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

