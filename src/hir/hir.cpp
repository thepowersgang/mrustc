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
#include <mir/mir.hpp>
#include <hir/expr.hpp>

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

    ::std::ostream& operator<<(::std::ostream& os, const ConstGeneric& x)
    {
        TU_MATCH_HDRA( (x), {)
        TU_ARMA(Infer, e) {
            os << "Infer";
            if(e.index != ~0u) {
                os << "(";
                os << e.index;
                os << ")";
            }
            }
        TU_ARMA(Unevaluated, e) {
            os << "Unevaluated(";
            if(e->m_mir) {
                for(const auto& b : e->m_mir->blocks) {
                    os << b.statements << "; " << b.terminator;
                }
            }
            else {
                HIR_DumpExpr(os, *e);
            }
            os << ")";
            }
        TU_ARMA(Generic, e) os << "Generic(" << e << ")";
        TU_ARMA(Evaluated, e)   os << "Evaluated(" << *e << ")";
        }
        return os;
    }
    bool ConstGeneric::operator==(const ConstGeneric& x) const
    {
        if(this->tag() != x.tag())
            return false;
        TU_MATCH_HDRA( (*this, x), {)
        TU_ARMA(Infer, te, xe) return te.index == xe.index;
        TU_ARMA(Unevaluated, te, xe)    return te == xe;
        TU_ARMA(Generic, te, xe)    return te == xe;
        TU_ARMA(Evaluated, te, xe)  return EncodedLiteralSlice(*te) == EncodedLiteralSlice(*xe);
        }
        return true;
    }

    Ordering ConstGeneric::ord(const ConstGeneric& x) const
    {
        if(auto cmp = ::ord(static_cast<int>(this->tag()), static_cast<int>(x.tag())))  return cmp;
        TU_MATCH_HDRA( (*this, x), {)
        TU_ARMA(Infer, te, xe) {
            if(auto cmp = ::ord(te.index, xe.index))    return cmp;
            }
        TU_ARMA(Unevaluated, te, xe) {
            // If only one has populated MIR, they can't be equal (sort populated MIR after)
            if( !te->m_mir != !xe->m_mir ) {
                return (te->m_mir ? OrdGreater : OrdLess);
            }

            // HACK: If the inner is a const param on both, sort based on that.
            // - Very similar to the ordering of TypeRef::Generic
            const auto* tn = dynamic_cast<const HIR::ExprNode_ConstParam*>(&**te);
            const auto* xn = dynamic_cast<const HIR::ExprNode_ConstParam*>(&**xe);
            if( tn && xn )
            {
                // Is this valid? What if they're from different scopes?
                return ::ord(tn->m_binding, xn->m_binding);
            }

            if( te->m_mir )
            {
                assert(xe->m_mir);
                // TODO: Compare MIR
                TODO(Span(), "Compare non-expanded array sizes - (w/ MIR) " << *this << " and " << x);
            }
            else
            {
                TODO(Span(), "Compare non-expanded array sizes - (wo/ MIR) " << *this << " and " << x);
            }
            }
        TU_ARMA(Generic, te, xe) {
            if(auto cmp = ::ord(te, xe))    return cmp;
            }
        TU_ARMA(Evaluated, te, xe) {
            if(auto cmp = ::ord(EncodedLiteralSlice(*te), EncodedLiteralSlice(*xe)))    return cmp;
            }
        }
        return OrdEqual;
    }

    ::std::ostream& operator<<(::std::ostream& os, const Struct::Repr& x) {
        os << "repr(";
        switch(x)
        {
        case Struct::Repr::Rust:    os << "Rust";   break;
        case Struct::Repr::C:   os << "C";  break;
        case Struct::Repr::Simd:    os << "simd";   break;
        case Struct::Repr::Transparent: os << "transparent";    break;
        }
        os << ")";
        return os;
    }
}

HIR::ConstGeneric HIR::ConstGeneric::clone() const
{
    TU_MATCH_HDRA( (*this), {)
    TU_ARMA(Infer, e) return e;
    TU_ARMA(Unevaluated, e) return e;
    TU_ARMA(Generic, e) return e;
    TU_ARMA(Evaluated, e)   return EncodedLiteralPtr(e->clone());
    }
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
    case ::HIR::Enum::Repr::Auto:
        return ::HIR::CoreType::Isize;
        break;
    case ::HIR::Enum::Repr::Usize: return ::HIR::CoreType::Usize; break;
    case ::HIR::Enum::Repr::U8 : return ::HIR::CoreType::U8 ; break;
    case ::HIR::Enum::Repr::U16: return ::HIR::CoreType::U16; break;
    case ::HIR::Enum::Repr::U32: return ::HIR::CoreType::U32; break;
    case ::HIR::Enum::Repr::U64: return ::HIR::CoreType::U64; break;
    case ::HIR::Enum::Repr::Isize: return ::HIR::CoreType::Isize; break;
    case ::HIR::Enum::Repr::I8 : return ::HIR::CoreType::I8 ; break;
    case ::HIR::Enum::Repr::I16: return ::HIR::CoreType::I16; break;
    case ::HIR::Enum::Repr::I32: return ::HIR::CoreType::I32; break;
    case ::HIR::Enum::Repr::I64: return ::HIR::CoreType::I64; break;
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

namespace {
    const ::HIR::Module& get_containing_module(const ::HIR::Crate& crate, const Span& sp, const ::HIR::SimplePath& path, bool ignore_crate_name, bool ignore_last_node)
    {
        ASSERT_BUG(sp, path.m_components.size() > 0u, "Invalid path (no nodes) - " << path);
        ASSERT_BUG(sp, path.m_components.size() > (ignore_last_node ? 1u : 0u), "Invalid path (only one node with `ignore_last_node` - " << path);

        const ::HIR::Module* mod;
        if( !ignore_crate_name && path.m_crate_name != crate.m_crate_name ) {
            ASSERT_BUG(sp, crate.m_ext_crates.count(path.m_crate_name) > 0, "Crate '" << path.m_crate_name << "' not loaded for " << path);
            mod = &crate.m_ext_crates.at(path.m_crate_name).m_data->m_root_module;
        }
        else {
            mod =  &crate.m_root_module;
        }
        for( unsigned int i = 0; i < path.m_components.size() - (ignore_last_node ? 2 : 1); i ++ )
        {
            const auto& pc = path.m_components[i];
            auto it = mod->m_mod_items.find( pc );
            if( it == mod->m_mod_items.end() ) {
                BUG(sp, "Couldn't find component " << i << " of " << path);
            }
            if(const auto* e = it->second->ent.opt_Module()) {
                mod = e;
            }
            else {
                BUG(sp, "Node " << i << " of path " << path << " wasn't a module");
            }
        }
        return *mod;
    }
}

const ::HIR::MacroItem& ::HIR::Crate::get_macroitem_by_path(const Span& sp, const ::HIR::SimplePath& path, bool ignore_crate_name, bool ignore_last_node) const
{
    const auto& mod = get_containing_module(*this, sp, path, ignore_crate_name, ignore_last_node);

    auto it = mod.m_macro_items.find( ignore_last_node ? path.m_components[path.m_components.size()-2] : path.m_components.back() );
    if( it == mod.m_macro_items.end() ) {
        BUG(sp, "Could not find macro name in " << path);
    }

    return it->second->ent;
}

const ::HIR::TypeItem& ::HIR::Crate::get_typeitem_by_path(const Span& sp, const ::HIR::SimplePath& path, bool ignore_crate_name, bool ignore_last_node) const
{
    const auto& mod = get_containing_module(*this, sp, path, ignore_crate_name, ignore_last_node);

    auto it = mod.m_mod_items.find( ignore_last_node ? path.m_components[path.m_components.size()-2] : path.m_components.back() );
    if( it == mod.m_mod_items.end() ) {
        BUG(sp, "Could not find type name in " << path);
    }

    return it->second->ent;
}

const ::HIR::Module& ::HIR::Crate::get_mod_by_path(const Span& sp, const ::HIR::SimplePath& path, bool ignore_last_node/*=false*/, bool ignore_crate_name/*=false*/) const
{
    if( ignore_last_node )
    {
        ASSERT_BUG(sp, path.m_components.size() > 0, "get_mod_by_path received invalid path with ignore_last_node=true - " << path);
    }
    // Special handling for empty paths with `ignore_last_node`
    if( path.m_components.size() == (ignore_last_node ? 1 : 0) )
    {
        if( !ignore_crate_name && path.m_crate_name != m_crate_name )
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
        const auto& ti = this->get_typeitem_by_path(sp, path, ignore_crate_name, ignore_last_node);
        if(auto* e = ti.opt_Module())
        {
            return *e;
        }
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
        BUG(sp, "Trait path " << path << " didn't point to a trait (" << ti.tag_str() << ")");
    }
}
const ::HIR::Struct& ::HIR::Crate::get_struct_by_path(const Span& sp, const ::HIR::SimplePath& path) const
{
    const auto& ti = this->get_typeitem_by_path(sp, path);
    TU_IFLET(::HIR::TypeItem, ti, Struct, e,
        return e;
    )
    else {
        BUG(sp, "Struct path " << path << " didn't point to a struct (" << ti.tag_str() << ")");
    }
}
const ::HIR::Union& ::HIR::Crate::get_union_by_path(const Span& sp, const ::HIR::SimplePath& path) const
{
    const auto& ti = this->get_typeitem_by_path(sp, path);
    TU_IFLET(::HIR::TypeItem, ti, Union, e,
        return e;
    )
    else {
        BUG(sp, "Path " << path << " didn't point to a union (" << ti.tag_str() << ")");
    }
}
const ::HIR::Enum& ::HIR::Crate::get_enum_by_path(const Span& sp, const ::HIR::SimplePath& path, bool ignore_crate_name, bool ignore_last_node) const
{
    const auto& ti = this->get_typeitem_by_path(sp, path, ignore_crate_name, ignore_last_node);
    TU_IFLET(::HIR::TypeItem, ti, Enum, e,
        return e;
    )
    else {
        BUG(sp, "Enum path " << path << " didn't point to an enum (" << ti.tag_str() << ")");
    }
}

const ::HIR::ValueItem& ::HIR::Crate::get_valitem_by_path(const Span& sp, const ::HIR::SimplePath& path, bool ignore_crate_name) const
{
    const auto& mod = get_containing_module(*this, sp, path, ignore_crate_name, /*ignore_last_node=*/false);

    auto it = mod.m_value_items.find( path.m_components.back() );
    if( it == mod.m_value_items.end() ) {
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
        BUG(sp, "Function path " << path << " didn't point to an function (" << ti.tag_str() << ")");
    }
}


const ::HIR::Static& ::HIR::Crate::get_static_by_path(const Span& sp, const ::HIR::SimplePath& path) const
{
    const auto& m = this->get_mod_by_path(sp, path, /*ignore_last*/true);
    auto it = m.m_value_items.find(path.m_components.back());
    if(it != m.m_value_items.end())
    {
        ASSERT_BUG(sp, it->second->ent.is_Static(), "`static` path " << path << " didn't point to a static - " << it->second->ent.tag_str());
        return it->second->ent.as_Static();
    }
    for(const auto& e : m.m_inline_statics)
    {
        if(e.first == path.m_components.back())
        {
            return *e.second;
        }
    }
    BUG(sp, "`static` path " << path << " can't be found");
}
