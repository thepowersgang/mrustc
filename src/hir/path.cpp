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


