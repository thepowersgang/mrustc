/*
 */
#ifndef COMMON_HPP_INCLUDED
#define COMMON_HPP_INCLUDED

#define FOREACH(basetype, it, src)  for(basetype::const_iterator it = src.begin(); it != src.end(); ++ it)
#define FOREACH_M(basetype, it, src)  for(basetype::iterator it = src.begin(); it != src.end(); ++ it)

#include <iostream>
#include <vector>

#define DEBUG(ss)   do{ ::std::cerr << __FUNCTION__ << ": " << ss << ::std::endl; } while(0)

namespace AST {

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

}

#endif
