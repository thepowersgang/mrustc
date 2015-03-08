/*
 */
#ifndef COMMON_HPP_INCLUDED
#define COMMON_HPP_INCLUDED

#include <iostream>
#include <vector>
#include <map>
#include <cassert>
#include <sstream>

#define FMT(ss)    (dynamic_cast< ::std::stringstream&>(::std::stringstream() << ss).str())

#include "include/debug.hpp"
#include "include/rustic.hpp"	// slice and option

template <typename T>
struct LList
{
    LList*  m_prev;
    T   m_item;
    
    LList(LList* prev, T item):
        m_prev(prev),
        m_item( ::std::move(item) )
    {
    };
};

namespace std {

template <typename T>
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::vector<T*>& v) {
    if( v.size() > 0 )
    {
        bool is_first = true;
        for( const auto& i : v )
        {
            if(!is_first)
                os << ", ";
            is_first = false;
            os << *i;
        }
    }
    return os;
}

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

template <typename T, typename U>
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::map<T,U>& v) {
    if( v.size() > 0 )
    {
        bool is_first = true;
        for( const auto& i : v )
        {
            if(!is_first)
                os << ", ";
            is_first = false;
            os << i.first << ": " << i.second;
        }
    }
    return os;
}

}

#endif
