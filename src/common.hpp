/*
 */
#ifndef COMMON_HPP_INCLUDED
#define COMMON_HPP_INCLUDED

#include <iostream>
#include <vector>
#include <map>
#include <cassert>
#include <sstream>

extern int g_debug_indent_level;

#define FMT(ss)    (dynamic_cast< ::std::stringstream&>(::std::stringstream() << ss).str())
#define INDENT()    do { g_debug_indent_level += 1; } while(0)
#define UNINDENT()    do { g_debug_indent_level -= 1; } while(0)
#define DEBUG(ss)   do{ ::std::cerr << ::RepeatLitStr{" ", g_debug_indent_level} << __FUNCTION__ << ": " << ss << ::std::endl; } while(0)

struct RepeatLitStr
{
    const char *s;
    int n;
    
    friend ::std::ostream& operator<<(::std::ostream& os, const RepeatLitStr& r) {
        for(int i = 0; i < r.n; i ++ )
            os << r.s;
        return os;
    }
};

template<typename T>
class slice
{
    T*  m_first;
    unsigned int    m_len;
public:
    slice(::std::vector<T>& v):
        m_first(&v[0]),
        m_len(v.size())
    {}
    
    ::std::vector<T> to_vec() const {
        return ::std::vector<T>(begin(), end());
    }
    
    unsigned int size() const {
        return m_len;
    }
    T& operator[](unsigned int i) const {
        assert(i < m_len);
        return m_first[i];
    }
    
    T* begin() const { return m_first; }
    T* end() const { return m_first + m_len; }
};

template<typename T>
::std::ostream& operator<<(::std::ostream& os, slice<T> s) {
    if( s.size() > 0 )
    {
        bool is_first = true;
        for( const auto& i : s )
        {
            if(!is_first)
                os << ", ";
            is_first = false;
            os << i;
        }
    }
    return os;
}

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
