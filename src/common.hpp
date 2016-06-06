/*
 */
#ifndef COMMON_HPP_INCLUDED
#define COMMON_HPP_INCLUDED

#include <iostream>
#include <vector>
#include <map>
#include <cassert>
#include <sstream>
#include <memory>

#define FMT(ss)    (dynamic_cast< ::std::stringstream&>(::std::stringstream() << ss).str())
// XXX: Evil hack - Define 'mv$' to be ::std::move
#define mv$(x)    ::std::move(x)
#define box$(x) ::make_unique_ptr(::std::move(x))
#define rc_new$(x) ::make_shared_ptr(::std::move(x))

#include "include/debug.hpp"
#include "include/rustic.hpp"	// slice and option
#include "include/compile_error.hpp"

template<typename T>
::std::unique_ptr<T> make_unique_ptr(T&& v) {
    return ::std::unique_ptr<T>(new T(mv$(v)));
}
template<typename T>
::std::shared_ptr<T> make_shared_ptr(T&& v) {
    return ::std::shared_ptr<T>(new T(mv$(v)));
}
template<typename T>
::std::vector<T> make_vec1(T&& v) {
    ::std::vector<T>    rv;
    rv.push_back( mv$(v) );
    return rv;
}

enum Ordering
{
    OrdLess,
    OrdEqual,
    OrdGreater,
};
static inline Ordering ord(bool l, bool r)
{
    if(l == r)
        return OrdEqual;
    else if( l )
        return OrdGreater;
    else
        return OrdLess;
}
static inline Ordering ord(unsigned l, unsigned r)
{
    if(l == r)
        return OrdEqual;
    else if( l > r )
        return OrdGreater;
    else
        return OrdLess;
}
static inline Ordering ord(const ::std::string& l, const ::std::string& r)
{
    if(l == r)
        return OrdEqual;
    else if( l > r )
        return OrdGreater;
    else
        return OrdLess;
}
template<typename T>
Ordering ord(const T& l, const T& r)
{
    return l.ord(r);
}
template<typename T, typename U>
Ordering ord(const ::std::pair<T,U>& l, const ::std::pair<T,U>& r)
{
    Ordering    rv;
    rv = ::ord(l.first, r.first);
    if(rv != OrdEqual)   return rv;
    rv = ::ord(l.second, r.second);
    return rv;
}
template<typename T>
Ordering ord(const ::std::vector<T>& l, const ::std::vector<T>& r)
{
    unsigned int i = 0;
    for(const auto& it : l)
    {
        if( i >= r.size() )
            return OrdGreater;
        
        auto rv = ::ord( it, r[i] );
        if( rv != OrdEqual )
            return rv;
        
        i ++;
    }
        
    return OrdEqual;
}


template <typename T>
struct LList
{
    const LList*  m_prev;
    T   m_item;
    
    LList(const LList* prev, T item):
        m_prev(prev),
        m_item( ::std::move(item) )
    {
    };
};

template<typename T>
struct Join {
    const char *sep;
    const ::std::vector<T>& v;
    friend ::std::ostream& operator<<(::std::ostream& os, const Join& j) {
        if( j.v.size() > 0 )
            os << j.v[0];
        for( unsigned int i = 1; i < j.v.size(); i ++ )
            os << j.sep << j.v[i];
        return os;
    }
};
template<typename T>
inline Join<T> join(const char *sep, const ::std::vector<T> v) {
    return Join<T>({ sep, v });
}


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
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::pair<T,U>& v) {
    os << "(" << v.first << ", " << v.second << ")";
    return os;
}

template <typename T, typename U, class Cmp>
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::map<T,U,Cmp>& v) {
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

template <typename T, typename U, class Cmp>
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::multimap<T,U,Cmp>& v) {
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

// -------------------------------------------------------------------
// --- Reversed iterable
template <typename T>
struct reversion_wrapper { T& iterable; };

template <typename T>
auto begin (reversion_wrapper<T> w) { return ::std::rbegin(w.iterable); }

template <typename T>
auto end (reversion_wrapper<T> w) { return ::std::rend(w.iterable); }

template <typename T>
reversion_wrapper<T> reverse (T&& iterable) { return { iterable }; }

#endif
