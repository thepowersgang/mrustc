/*
 */
#ifndef COMMON_HPP_INCLUDED
#define COMMON_HPP_INCLUDED

#define FOREACH(basetype, it, src)  for(basetype::const_iterator it = src.begin(); it != src.end(); ++ it)
#define FOREACH_M(basetype, it, src)  for(basetype::iterator it = src.begin(); it != src.end(); ++ it)

#include <iostream>
#include <vector>
#include <cassert>

#define DEBUG(ss)   do{ ::std::cerr << __FUNCTION__ << ": " << ss << ::std::endl; } while(0)

namespace rust {

template<typename T>
class option
{
    bool    m_set;
    T   m_data;
public:
    option(T ent):
        m_set(true),
        m_data( ::std::move(ent) )
    {}
    option():
        m_set(false)
    {}
    
    bool is_none() const { return !m_set; }
    bool is_some() const { return m_set; }
    
    const T& unwrap() const {
        assert(is_some());
        return m_data;
    }
};
template<typename T>
class option<T&>
{
    T* m_ptr;
public:
    option(T& ent):
        m_ptr(&ent)
    {}
    option():
        m_ptr(nullptr)
    {}
    
    bool is_none() const { return m_ptr == nullptr; }
    bool is_some() const { return m_ptr != nullptr; }
    T& unwrap() const {
        assert(is_some());
        return *m_ptr;
    }
};
template<typename T>
option<T> Some(T data) {
    return option<T>( ::std::move(data) );
}
template<typename T>
option<T> None() {
    return option<T>( );
}

};

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
