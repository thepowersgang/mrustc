/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/path.hpp
 * - Item paths (helper code)
 */
#include <hir/path.hpp>
#include <hir/type.hpp>

::HIR::SimplePath HIR::SimplePath::operator+(const ::std::string& s) const
{
    ::HIR::SimplePath ret(m_crate_name);
    ret.m_components = m_components;

    ret.m_components.push_back( s );

    return ret;
}
namespace HIR {
    ::std::ostream& operator<<(::std::ostream& os, const ::HIR::SimplePath& x)
    {
        if( x.m_crate_name != "" ) {
            os << "::\"" << x.m_crate_name << "\"";
        }
        else if( x.m_components.size() == 0 ) {
            os << "::";
        }
        else {
        }
        for(const auto& n : x.m_components)
        {
            os << "::" << n;
        }
        return os;
    }

    ::std::ostream& operator<<(::std::ostream& os, const PathParams& x)
    {
        bool has_args = ( x.m_types.size() > 0 );

        if(has_args) {
            os << "<";
        }
        for(const auto& ty : x.m_types) {
            os << ty << ",";
        }
        if(has_args) {
            os << ">";
        }
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const GenericPath& x)
    {
        os << x.m_path << x.m_params;
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const TraitPath& x)
    {
        if( x.m_hrls.size() > 0 ) {
            os << "for<";
            for(const auto& lft : x.m_hrls)
                os << "'" << lft << ",";
            os << "> ";
        }
        os << x.m_path.m_path;
        bool has_args = ( x.m_path.m_params.m_types.size() > 0 || x.m_type_bounds.size() > 0 );

        if(has_args) {
            os << "<";
        }
        for(const auto& ty : x.m_path.m_params.m_types) {
            os << ty << ",";
        }
        for(const auto& assoc : x.m_type_bounds) {
            os << assoc.first << "=" << assoc.second << ",";
        }
        if(has_args) {
            os << ">";
        }
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const Path& x)
    {
        TU_MATCH(::HIR::Path::Data, (x.m_data), (e),
        (Generic,
            return os << e;
            ),
        (UfcsInherent,
            return os << "<" << *e.type << " /*- " << e.impl_params << "*/>::" << e.item << e.params;
            ),
        (UfcsKnown,
            return os << "<" << *e.type << " as " << e.trait << ">::" << e.item << e.params;
            ),
        (UfcsUnknown,
            return os << "<" << *e.type << " as _>::" << e.item << e.params;
            )
        )
        return os;
    }
}

::HIR::SimplePath HIR::SimplePath::clone() const
{
    return SimplePath( m_crate_name, m_components );
}

::HIR::PathParams::PathParams()
{
}
::HIR::PathParams::PathParams(::HIR::TypeRef ty0)
{
    m_types.push_back( mv$(ty0) );
}
::HIR::PathParams HIR::PathParams::clone() const
{
    PathParams  rv;
    for( const auto& t : m_types )
        rv.m_types.push_back( t.clone() );
    return rv;
}
bool ::HIR::PathParams::operator==(const ::HIR::PathParams& x) const
{
    if( m_types.size() != x.m_types.size() )
        return false;
    for( unsigned int i = 0; i < m_types.size(); i ++ )
        if( !(m_types[i] == x.m_types[i]) )
            return false;
    return true;
}

::HIR::GenericPath::GenericPath()
{
}
::HIR::GenericPath::GenericPath(::HIR::SimplePath sp):
    m_path( mv$(sp) )
{
}
::HIR::GenericPath::GenericPath(::HIR::SimplePath sp, ::HIR::PathParams params):
    m_path( mv$(sp) ),
    m_params( mv$(params) )
{
}
::HIR::GenericPath HIR::GenericPath::clone() const
{
    return GenericPath(m_path.clone(), m_params.clone());
}
bool ::HIR::GenericPath::operator==(const GenericPath& x) const
{
    if( m_path != x.m_path )
        return false;
    return m_params == x.m_params;
}

::HIR::TraitPath HIR::TraitPath::clone() const
{
    ::HIR::TraitPath    rv {
        m_path.clone(),
        m_hrls,
        {},
        m_trait_ptr
        };

    for( const auto& assoc : m_type_bounds )
        rv.m_type_bounds.insert(::std::make_pair( assoc.first, assoc.second.clone() ));

    return rv;
}
bool ::HIR::TraitPath::operator==(const ::HIR::TraitPath& x) const
{
    if( m_path != x.m_path )
        return false;
    if( m_hrls != x.m_hrls )
        return false;

    if( m_type_bounds.size() != x.m_type_bounds.size() )
        return false;

    for(auto it_l = m_type_bounds.begin(), it_r = x.m_type_bounds.begin(); it_l != m_type_bounds.end(); it_l++, it_r++ ) {
        if( it_l->first != it_r->first )
            return false;
        if( it_l->second != it_r->second )
            return false;
    }
    return true;
}

::HIR::Path::Path(::HIR::GenericPath gp):
    m_data( ::HIR::Path::Data::make_Generic( mv$(gp) ) )
{
}
::HIR::Path::Path(::HIR::SimplePath sp):
    m_data( ::HIR::Path::Data::make_Generic(::HIR::GenericPath(mv$(sp))) )
{
}
::HIR::Path::Path(TypeRef ty, ::std::string item, PathParams item_params):
    m_data(Data::make_UfcsInherent({ box$(ty), mv$(item), mv$(item_params) }))
{
}
::HIR::Path::Path(TypeRef ty, GenericPath trait, ::std::string item, PathParams item_params):
    m_data( Data::make_UfcsKnown({ box$(mv$(ty)), mv$(trait), mv$(item), mv$(item_params) }) )
{
}
::HIR::Path HIR::Path::clone() const
{
    TU_MATCH(Data, (m_data), (e),
    (Generic,
        return Path( Data::make_Generic(e.clone()) );
        ),
    (UfcsInherent,
        return Path(Data::make_UfcsInherent({
            box$( e.type->clone() ),
            e.item,
            e.params.clone(),
            e.impl_params.clone()
            }));
        ),
    (UfcsKnown,
        return Path(Data::make_UfcsKnown({
            box$( e.type->clone() ),
            e.trait.clone(),
            e.item,
            e.params.clone()
            }));
        ),
    (UfcsUnknown,
        return Path(Data::make_UfcsUnknown({
            box$( e.type->clone() ),
            e.item,
            e.params.clone()
            }));
        )
    )
    throw "";
}

::HIR::Compare HIR::PathParams::compare_with_placeholders(const Span& sp, const ::HIR::PathParams& x, ::HIR::t_cb_resolve_type resolve_placeholder) const
{
    using ::HIR::Compare;

    auto rv = Compare::Equal;
    if( this->m_types.size() > 0 || x.m_types.size() > 0 ) {
        if( this->m_types.size() != x.m_types.size() ) {
            return Compare::Unequal;
        }
        for( unsigned int i = 0; i < x.m_types.size(); i ++ )
        {
            auto rv2 = this->m_types[i].compare_with_placeholders( sp, x.m_types[i], resolve_placeholder );
            if( rv2 == Compare::Unequal )
                return Compare::Unequal;
            if( rv2 == Compare::Fuzzy )
                rv = Compare::Fuzzy;
        }
    }
    return rv;
}
::HIR::Compare HIR::PathParams::match_test_generics_fuzz(const Span& sp, const PathParams& x, t_cb_resolve_type resolve_placeholder, t_cb_match_generics match) const
{
    using ::HIR::Compare;
    auto rv = Compare::Equal;

    if( this->m_types.size() != x.m_types.size() ) {
        return Compare::Unequal;
    }
    for( unsigned int i = 0; i < x.m_types.size(); i ++ )
    {
        rv &= this->m_types[i].match_test_generics_fuzz( sp, x.m_types[i], resolve_placeholder, match );
        if( rv == Compare::Unequal )
            return Compare::Unequal;
    }
    return rv;
}
::HIR::Compare HIR::GenericPath::compare_with_placeholders(const Span& sp, const ::HIR::GenericPath& x, ::HIR::t_cb_resolve_type resolve_placeholder) const
{
    using ::HIR::Compare;

    if( this->m_path.m_crate_name != x.m_path.m_crate_name )
        return Compare::Unequal;
    if( this->m_path.m_components.size() != x.m_path.m_components.size() )
        return Compare::Unequal;
    for(unsigned int i = 0; i < this->m_path.m_components.size(); i ++ )
    {
        if( this->m_path.m_components[i] != x.m_path.m_components[i] )
            return Compare::Unequal;
    }

    return this->m_params. compare_with_placeholders(sp, x.m_params, resolve_placeholder);
}

namespace {
    ::HIR::Compare compare_with_placeholders(
            const Span& sp,
            const ::HIR::PathParams& l, const ::HIR::PathParams& r,
            ::HIR::t_cb_resolve_type resolve_placeholder
            )
    {
        return l.compare_with_placeholders(sp, r, resolve_placeholder);
    }
    ::HIR::Compare compare_with_placeholders(
            const Span& sp,
            const ::HIR::GenericPath& l, const ::HIR::GenericPath& r,
            ::HIR::t_cb_resolve_type resolve_placeholder
            )
    {
        return l.compare_with_placeholders(sp, r, resolve_placeholder);
    }
}

#define CMP(rv, cmp)    do { \
    switch(cmp) {\
    case ::HIR::Compare::Unequal:   return ::HIR::Compare::Unequal; \
    case ::HIR::Compare::Fuzzy: rv = ::HIR::Compare::Fuzzy; break; \
    case ::HIR::Compare::Equal: break; \
    }\
} while(0)

::HIR::Compare HIR::TraitPath::compare_with_placeholders(const Span& sp, const TraitPath& x, t_cb_resolve_type resolve_placeholder) const
{
    auto rv = m_path .compare_with_placeholders(sp, x.m_path, resolve_placeholder);
    if( rv == Compare::Unequal )
        return rv;

    // TODO: HRLs

    auto it_l = m_type_bounds.begin();
    auto it_r = x.m_type_bounds.begin();
    while( it_l != m_type_bounds.end() && it_r != x.m_type_bounds.end() )
    {
        if( it_l->first != it_r->first ) {
            return Compare::Unequal;
        }
        CMP( rv, it_l->second .compare_with_placeholders( sp, it_r->second, resolve_placeholder ) );
        ++ it_l;
        ++ it_r;
    }

    if( it_l != m_type_bounds.end() || it_r != x.m_type_bounds.end() )
    {
        return Compare::Unequal;
    }

    return rv;
}

::HIR::Compare HIR::Path::compare_with_placeholders(const Span& sp, const Path& x, t_cb_resolve_type resolve_placeholder) const
{
    if( this->m_data.tag() != x.m_data.tag() )
        return Compare::Unequal;
    TU_MATCH(::HIR::Path::Data, (this->m_data, x.m_data), (ple, pre),
    (Generic,
        return ::compare_with_placeholders(sp, ple, pre, resolve_placeholder);
        ),
    (UfcsUnknown,
        if( ple.item != pre.item)
            return Compare::Unequal;

        TODO(sp, "Path::compare_with_placeholders - UfcsUnknown");
        ),
    (UfcsInherent,
        if( ple.item != pre.item)
            return Compare::Unequal;
        ::HIR::Compare  rv = ::HIR::Compare::Equal;
        CMP(rv, ple.type->compare_with_placeholders(sp, *pre.type, resolve_placeholder));
        CMP(rv, ::compare_with_placeholders(sp, ple.params, pre.params, resolve_placeholder));
        return rv;
        ),
    (UfcsKnown,
        if( ple.item != pre.item)
            return Compare::Unequal;

        ::HIR::Compare  rv = ::HIR::Compare::Equal;
        CMP(rv, ple.type->compare_with_placeholders(sp, *pre.type, resolve_placeholder));
        CMP(rv, ::compare_with_placeholders(sp, ple.trait, pre.trait, resolve_placeholder));
        CMP(rv, ::compare_with_placeholders(sp, ple.params, pre.params, resolve_placeholder));
        return rv;
        )
    )
    throw "";
}

Ordering HIR::Path::ord(const ::HIR::Path& x) const
{
    ORD( (unsigned)m_data.tag(), (unsigned)x.m_data.tag() );
    TU_MATCH(::HIR::Path::Data, (this->m_data, x.m_data), (tpe, xpe),
    (Generic,
        return ::ord(tpe, xpe);
        ),
    (UfcsInherent,
        ORD(*tpe.type, *xpe.type);
        ORD(tpe.item, xpe.item);
        return ::ord(tpe.params, xpe.params);
        ),
    (UfcsKnown,
        ORD(*tpe.type, *xpe.type);
        ORD(tpe.trait, xpe.trait);
        ORD(tpe.item, xpe.item);
        return ::ord(tpe.params, xpe.params);
        ),
    (UfcsUnknown,
        ORD(*tpe.type, *xpe.type);
        ORD(tpe.item, xpe.item);
        return ::ord(tpe.params, xpe.params);
        )
    )
    throw "";
}

bool ::HIR::Path::operator==(const Path& x) const {
    return this->ord(x) == ::OrdEqual;
}

