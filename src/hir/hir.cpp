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
        (String,
            os << "\"" << e << "\"";
            )
        )
        return os;
    }
}

namespace {
    bool matches_type_int(const ::HIR::GenericParams& params, const ::HIR::TypeRef& left,  const ::HIR::TypeRef& right)
    {
        assert(! left.m_data.is_Infer() );
        if( right.m_data.is_Infer() ) {
            // TODO: Why is this false? A _ type could match anything
            return false;
        }
        
        if( left.m_data.is_Generic() ) {
            // True?
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
                if( ple.m_path.m_crate_name != pre.m_path.m_crate_name )
                    return false;
                if( ple.m_path.m_components.size() != pre.m_path.m_components.size() )
                    return false;
                for(unsigned int i = 0; i < ple.m_path.m_components.size(); i ++ )
                {
                    if( ple.m_path.m_components[i] != pre.m_path.m_components[i] )
                        return false;
                }
                
                if( ple.m_params.m_types.size() > 0 || pre.m_params.m_types.size() > 0 ) {
                    if( ple.m_params.m_types.size() != pre.m_params.m_types.size() ) {
                        return true;
                        //TODO(Span(), "Match generic paths " << ple << " and " << pre << " - count mismatch");
                    }
                    for( unsigned int i = 0; i < pre.m_params.m_types.size(); i ++ )
                    {
                        if( ! matches_type_int(params, ple.m_params.m_types[i], pre.m_params.m_types[i]) )
                            return false;
                    }
                }
                return true;
                )
            )
            ),
        (Generic,
            throw "";
            ),
        (TraitObject,
            DEBUG("TODO: Compare " << left << " and " << right);
            return false;
            ),
        (Array,
            if( ! matches_type_int(params, *le.inner, *re.inner) )
                return false;
            if( le.size_val != re.size_val )
                return false;
            return true;
            ),
        (Slice,
            return matches_type_int(params, *le.inner, *re.inner);
            ),
        (Tuple,
            if( le.size() != re.size() )
                return false;
            for( unsigned int i = 0; i < le.size(); i ++ )
                if( !matches_type_int(params, le[i], re[i]) )
                    return false;
            return true;
            ),
        (Borrow,
            if( le.type != re.type )
                return false;
            return matches_type_int(params, *le.inner, *re.inner);
            ),
        (Pointer,
            if( le.is_mut != re.is_mut )
                return false;
            return matches_type_int(params, *le.inner, *re.inner);
            ),
        (Function,
            DEBUG("TODO: Compare " << left << " and " << right);
            return false;
            )
        )
        return false;
    }
}

bool ::HIR::TraitImpl::matches_type(const ::HIR::TypeRef& type) const
{
    return matches_type_int(m_params, m_type, type);
}
bool ::HIR::TypeImpl::matches_type(const ::HIR::TypeRef& type) const
{
    return matches_type_int(m_params, m_type, type);
}
bool ::HIR::MarkerImpl::matches_type(const ::HIR::TypeRef& type) const
{
    return matches_type_int(m_params, m_type, type);
}
