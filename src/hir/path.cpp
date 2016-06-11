/*
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
    ::std::ostream& operator<<(::std::ostream& os, const Path& x)
    {
        TU_MATCH(::HIR::Path::Data, (x.m_data), (e),
        (Generic,
            return os << e;
            ),
        (UfcsInherent,
            return os << "<" << *e.type << ">::" << e.item << e.params;
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


::HIR::Path::Path(::HIR::GenericPath gp):
    m_data( ::HIR::Path::Data::make_Generic( mv$(gp) ) )
{
}
::HIR::Path::Path(::HIR::SimplePath sp):
    m_data( ::HIR::Path::Data::make_Generic(::HIR::GenericPath(mv$(sp))) )
{
}
::HIR::Path HIR::Path::clone() const
{
    TU_MATCH(Data, (m_data), (e),
    (Generic,
        return Path( Data::make_Generic(e.clone()) );
        ),
    (UfcsInherent,
        return Path(Data::make_UfcsUnknown({
            box$( e.type->clone() ),
            e.item,
            e.params.clone()
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
        return Path(Data::make_UfcsInherent({
            box$( e.type->clone() ),
            e.item,
            e.params.clone()
            }));
        )
    )
    throw "";
}

namespace {
    ::HIR::Compare compare_with_paceholders(
            const Span& sp,
            const ::HIR::PathParams& l, const ::HIR::PathParams& r,
            ::HIR::t_cb_resolve_type resolve_placeholder
            )
    {
        using ::HIR::Compare;
        
        auto rv = Compare::Equal;
        if( l.m_types.size() > 0 || r.m_types.size() > 0 ) {
            if( l.m_types.size() != r.m_types.size() ) {
                return Compare::Unequal;
            }
            for( unsigned int i = 0; i < r.m_types.size(); i ++ )
            {
                auto rv2 = l.m_types[i].compare_with_paceholders( sp, r.m_types[i], resolve_placeholder );
                if( rv2 == Compare::Unequal )
                    return Compare::Unequal;
                if( rv2 == Compare::Fuzzy )
                    rv = Compare::Fuzzy;
            }
        }
        return rv;
    }
    ::HIR::Compare compare_with_paceholders(
            const Span& sp,
            const ::HIR::GenericPath& l, const ::HIR::GenericPath& r,
            ::HIR::t_cb_resolve_type resolve_placeholder
            )
    {
        using ::HIR::Compare;

        if( l.m_path.m_crate_name != r.m_path.m_crate_name )
            return Compare::Unequal;
        if( l.m_path.m_components.size() != r.m_path.m_components.size() )
            return Compare::Unequal;
        for(unsigned int i = 0; i < l.m_path.m_components.size(); i ++ )
        {
            if( l.m_path.m_components[i] != r.m_path.m_components[i] )
                return Compare::Unequal;
        }
        
        return compare_with_paceholders(sp, l.m_params, r.m_params, resolve_placeholder);
    }
}

#define CMP(rv, cmp)    do { \
    switch(cmp) {\
    case ::HIR::Compare::Unequal:   return ::HIR::Compare::Unequal; \
    case ::HIR::Compare::Fuzzy: rv = ::HIR::Compare::Fuzzy; break; \
    case ::HIR::Compare::Equal: break; \
    }\
} while(0)

::HIR::Compare HIR::Path::compare_with_paceholders(const Span& sp, const Path& x, t_cb_resolve_type resolve_placeholder) const
{
    if( this->m_data.tag() != x.m_data.tag() )
        return Compare::Unequal;
    TU_MATCH(::HIR::Path::Data, (this->m_data, x.m_data), (ple, pre),
    (Generic,
        return ::compare_with_paceholders(sp, ple, pre, resolve_placeholder);
        ),
    (UfcsUnknown,
        if( ple.item != pre.item)
            return Compare::Unequal;
        
        TODO(sp, "Path::compare_with_paceholders - UfcsUnknown");
        ),
    (UfcsInherent,
        if( ple.item != pre.item)
            return Compare::Unequal;
        ::HIR::Compare  rv = ::HIR::Compare::Equal;
        CMP(rv, ple.type->compare_with_paceholders(sp, *pre.type, resolve_placeholder));
        CMP(rv, ::compare_with_paceholders(sp, ple.params, pre.params, resolve_placeholder));
        return rv;
        ),
    (UfcsKnown,
        if( ple.item != pre.item)
            return Compare::Unequal;
        
        ::HIR::Compare  rv = ::HIR::Compare::Equal;
        CMP(rv, ple.type->compare_with_paceholders(sp, *pre.type, resolve_placeholder));
        CMP(rv, ::compare_with_paceholders(sp, ple.trait, pre.trait, resolve_placeholder));
        CMP(rv, ::compare_with_paceholders(sp, ple.params, pre.params, resolve_placeholder));
        return rv;
        )
    )
    throw "";
}

