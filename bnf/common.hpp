#pragma once

namespace std {

template <typename T>
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::vector<T>& v) {
    if( v.size() > 0 )
    {
        bool is_first = true;
        for( const auto& i : v )
        {
            if(!is_first)
                os << ", ";
            is_first = false;
            os << i;
        }
    }
    return os;
}

};
