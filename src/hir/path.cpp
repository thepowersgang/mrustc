/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/path.hpp
 * - Item paths (helper code)
 */
#include <hir/path.hpp>
#include <hir/type.hpp>
#include <algorithm>

::HIR::SimplePath HIR::SimplePath::operator+(const RcString& s) const
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
        bool has_args = ( x.m_lifetimes.size() > 0 || x.m_types.size() > 0 || x.m_values.size() > 0 );

        if(has_args) {
            os << "<";
        }
        for(const auto& lft : x.m_lifetimes) {
            os << lft << ",";
        }
        for(const auto& ty : x.m_types) {
            os << ty << ",";
        }
        for(const auto& v : x.m_values) {
            os << "{" << v << "},";
        }
        if(has_args) {
            os << ">";
        }
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const GenericPath& x)
    {
        if( x.m_hrls ) {
            os << "for" << x.m_hrls->fmt_args() << " ";
        }
        os << x.m_path << x.m_params;
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const TraitPath& x)
    {
        if( x.m_path.m_hrls ) {
            os << "for" << x.m_path.m_hrls->fmt_args() << " ";
        }
        os << x.m_path.m_path;
        bool has_args = ( x.m_path.m_params.m_lifetimes.size() > 0 || x.m_path.m_params.m_types.size() > 0 || x.m_type_bounds.size() > 0 || x.m_trait_bounds.size() > 0 );

        if(has_args) {
            os << "<";
        }
        for(const auto& lft : x.m_path.m_params.m_lifetimes) {
            os << lft << ",";
        }
        for(const auto& ty : x.m_path.m_params.m_types) {
            os << ty << ",";
        }
        for(const auto& assoc : x.m_type_bounds) {
            os << assoc.first << "{" << assoc.second.source_trait << "}=" << assoc.second << ",";
        }
        for(const auto& assoc : x.m_trait_bounds) {
            for(const auto& trait : assoc.second.traits) {
                os << assoc.first << "{" << assoc.second.source_trait << "}: " << trait << ",";
            }
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
            return os << "<" << e.type << " /*- " << e.impl_params << "*/>::" << e.item << e.params;
            ),
        (UfcsKnown,
            return os << "<" << e.type << " as " << e.trait << ">::" << e.item << e.params;
            ),
        (UfcsUnknown,
            return os << "<" << e.type << " as _>::" << e.item << e.params;
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
HIR::PathParams::PathParams(::HIR::LifetimeRef lft)
{
    m_lifetimes.push_back(lft);
}
::HIR::PathParams HIR::PathParams::clone() const
{
    PathParams  rv;
    for( const auto& l : m_lifetimes )
        rv.m_lifetimes.push_back( l );
    for( const auto& t : m_types )
        rv.m_types.push_back( t.clone() );
    for( const auto& t : m_values )
        rv.m_values.push_back( t.clone() );
    return rv;
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
::HIR::GenericPath::GenericPath(::HIR::GenericParams hrls, ::HIR::SimplePath sp, ::HIR::PathParams params):
    m_hrls( box$(hrls) ),
    m_path( mv$(sp) ),
    m_params( mv$(params) )
{
}
::HIR::GenericPath HIR::GenericPath::clone() const
{
    if(m_hrls)
        return GenericPath(m_hrls->clone(), m_path.clone(), m_params.clone());
    else
        return GenericPath(m_path.clone(), m_params.clone());
}
Ordering HIR::GenericPath::ord(const HIR::GenericPath& x) const
{
    ORD(m_path, x.m_path);
    //DEBUG("\n  " << *this << "\n  " << x);
    ORD(m_params, x.m_params);

    // HACK! if either of the HRLs are tagged as not having been un-elided, then assume they're equal
    // - Mostly a workaround for `lifetime_elision.cpp` fixing TraitPath ATY origins
    auto is_elision = [](const HIR::GenericPath& gp){ return gp.m_hrls && gp.m_hrls->m_lifetimes.size() >= 1 && gp.m_hrls->m_lifetimes.back().m_name == "#apply_elision"; };
    if( is_elision(*this) || is_elision(x) )
        return OrdEqual;

    // NOTE: An empty set is treated as the same as none
    ORD(m_hrls.get() && !m_hrls->is_empty(), x.m_hrls.get() && !x.m_hrls->is_empty());
    if( m_hrls && x.m_hrls ) {
        ORD(m_hrls->m_lifetimes.size(), x.m_hrls->m_lifetimes.size());
        ORD(m_hrls->m_bounds, x.m_hrls->m_bounds);
    }
    return OrdEqual;
}

::HIR::TraitPath HIR::TraitPath::clone() const
{
    ::HIR::TraitPath    rv {
        m_path.clone(),
        {},
        {},
        m_trait_ptr
        };

    for( const auto& assoc : m_type_bounds )
        rv.m_type_bounds.insert(::std::make_pair( assoc.first, assoc.second.clone() ));
    for( const auto& assoc : m_trait_bounds )
        rv.m_trait_bounds.insert(::std::make_pair( assoc.first, assoc.second.clone() ));

    return rv;
}
Ordering HIR::TraitPath::ord(const TraitPath& x) const
{
    ORD(m_path, x.m_path);
    ORD(m_trait_bounds, x.m_trait_bounds);
    ORD(m_type_bounds , x.m_type_bounds);
    return OrdEqual;
}

::HIR::Path::Path(::HIR::GenericPath gp):
    m_data( ::HIR::Path::Data::make_Generic( mv$(gp) ) )
{
}
::HIR::Path::Path(::HIR::SimplePath sp):
    m_data( ::HIR::Path::Data::make_Generic(::HIR::GenericPath(mv$(sp))) )
{
}
::HIR::Path::Path(TypeRef ty, RcString item, PathParams item_params):
    m_data(Data::make_UfcsInherent({ mv$(ty), mv$(item), mv$(item_params) }))
{
}
::HIR::Path::Path(TypeRef ty, GenericPath trait, RcString item, PathParams item_params):
    m_data( Data::make_UfcsKnown({ mv$(ty), mv$(trait), mv$(item), mv$(item_params) }) )
{
}
::HIR::Path HIR::Path::clone() const
{
    TU_MATCH_HDRA((m_data), {)
    TU_ARMA(Generic, e) {
        return Path( Data::make_Generic(e.clone()) );
        }
    TU_ARMA(UfcsInherent, e) {
        return Path(Data::make_UfcsInherent({
            e.type.clone(),
            e.item,
            e.params.clone(),
            e.impl_params.clone()
            }));
        }
    TU_ARMA(UfcsKnown, e) {
        return Path(Data::make_UfcsKnown({
            e.type.clone(),
            e.trait.clone(),
            e.item,
            e.params.clone()
            }));
        }
    TU_ARMA(UfcsUnknown, e) {
        return Path(Data::make_UfcsUnknown({
            e.type.clone(),
            e.item,
            e.params.clone()
            }));
        }
    }
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
::HIR::Compare HIR::PathParams::match_test_generics_fuzz(const Span& sp, const PathParams& x, t_cb_resolve_type resolve_placeholder, ::HIR::MatchGenerics& match) const
{
    using ::HIR::Compare;
    auto rv = Compare::Equal;
    TRACE_FUNCTION_F(*this << " with " << x);

    if( this->m_types.size() != x.m_types.size() ) {
        return Compare::Unequal;
    }
    for( unsigned int i = 0; i < x.m_types.size(); i ++ )
    {
        rv &= this->m_types[i].match_test_generics_fuzz( sp, x.m_types[i], resolve_placeholder, match );
        if( rv == Compare::Unequal )
            return Compare::Unequal;
    }

    if( this->m_values.size() != x.m_values.size() ) {
        return Compare::Unequal;
    }
    for( unsigned int i = 0; i < x.m_values.size(); i ++ )
    {
        if( const auto* ge = this->m_values[i].opt_Generic() ) {
            rv &= match.match_val(*ge, x.m_values[i]);
            if(rv == Compare::Unequal)
                return Compare::Unequal;
        }
        else {
            if( this->m_values[i] != x.m_values[i] ) {
                return Compare::Unequal;
            }
        }
    }

#if 1
    if( this->m_lifetimes.size() != x.m_lifetimes.size() ) {
        //return Compare::Unequal;
    }
    for( unsigned int i = 0; i < std::min(this->m_lifetimes.size(), x.m_lifetimes.size()); i ++ )
    {
        if( this->m_lifetimes[i].is_param() ) {
            /*rv &=*/ match.match_lft(this->m_lifetimes[i].as_param(), x.m_lifetimes[i]);
            //if(rv == Compare::Unequal)
            //    return Compare::Unequal;
        }
        else {
            //if( this->m_lifetimes[i] != x.m_lifetimes[i] ) {
            //    return Compare::Unequal;
            //}
        }
    }
#endif

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
        CMP( rv, it_l->second.type .compare_with_placeholders( sp, it_r->second.type, resolve_placeholder ) );
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
    TU_MATCH_HDRA( (this->m_data, x.m_data), {)
    TU_ARMA(Generic, ple, pre) {
        return ::compare_with_placeholders(sp, ple, pre, resolve_placeholder);
        }
    TU_ARMA(UfcsUnknown, ple, pre) {
        if( ple.item != pre.item)
            return Compare::Unequal;

        TODO(sp, "Path::compare_with_placeholders - UfcsUnknown");
        }
    TU_ARMA(UfcsInherent, ple, pre) {
        if( ple.item != pre.item)
            return Compare::Unequal;
        ::HIR::Compare  rv = ::HIR::Compare::Equal;
        CMP(rv, ple.type.compare_with_placeholders(sp, pre.type, resolve_placeholder));
        CMP(rv, ::compare_with_placeholders(sp, ple.params, pre.params, resolve_placeholder));
        return rv;
        }
    TU_ARMA(UfcsKnown, ple, pre) {
        if( ple.item != pre.item)
            return Compare::Unequal;

        ::HIR::Compare  rv = ::HIR::Compare::Equal;
        CMP(rv, ple.type.compare_with_placeholders(sp, pre.type, resolve_placeholder));
        CMP(rv, ::compare_with_placeholders(sp, ple.trait, pre.trait, resolve_placeholder));
        CMP(rv, ::compare_with_placeholders(sp, ple.params, pre.params, resolve_placeholder));
        return rv;
        }
    }
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
        ORD(tpe.type, xpe.type);
        ORD(tpe.item, xpe.item);
        return ::ord(tpe.params, xpe.params);
        ),
    (UfcsKnown,
        ORD(tpe.type, xpe.type);
        ORD(tpe.trait, xpe.trait);
        ORD(tpe.item, xpe.item);
        return ::ord(tpe.params, xpe.params);
        ),
    (UfcsUnknown,
        ORD(tpe.type, xpe.type);
        ORD(tpe.item, xpe.item);
        return ::ord(tpe.params, xpe.params);
        )
    )
    throw "";
}

bool ::HIR::Path::operator==(const Path& x) const {
    return this->ord(x) == ::OrdEqual;
}

