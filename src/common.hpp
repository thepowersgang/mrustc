/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * common.hpp
 * - Compiler-global common header
 */
#ifndef COMMON_HPP_INCLUDED
#define COMMON_HPP_INCLUDED

#include <iostream>
#include <vector>
#include <map>
#include <set>
#include <cassert>
#include <sstream>
#include <memory>

#ifdef _MSC_VER
#define __attribute__(x)    /* no-op */
#endif

#define FMT(ss)    (static_cast<::std::ostringstream&&>(::std::ostringstream() << ss).str())
// XXX: Evil hack - Define 'mv$' to be ::std::move
#define mv$ ::std::move
#define box$(...) ::make_unique_ptr(::std::move(__VA_ARGS__))
#define rc_new$(...) ::make_shared_ptr(::std::move(__VA_ARGS__))

#include "include/debug.hpp"
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
template<typename T>
::std::vector<T> make_vec2(T v1, T v2) {
    ::std::vector<T>    rv;
    rv.reserve(2);
    rv.push_back( mv$(v1) );
    rv.push_back( mv$(v2) );
    return rv;
}
template<typename T>
::std::vector<T> make_vec3(T v1, T v2, T v3) {
    ::std::vector<T>    rv;
    rv.reserve(3);
    rv.push_back( mv$(v1) );
    rv.push_back( mv$(v2) );
    rv.push_back( mv$(v3) );
    return rv;
}

enum Ordering
{
    OrdLess = -1,
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
static inline Ordering ord(char l, char r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
}

static inline Ordering ord(unsigned char l, unsigned char r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
}
static inline Ordering ord(unsigned short l, unsigned short r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
}
static inline Ordering ord(unsigned l, unsigned r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
}
static inline Ordering ord(unsigned long l, unsigned long r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
}
static inline Ordering ord(unsigned long long l, unsigned long long r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
}
static inline Ordering ord(signed char l, signed char r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
}
static inline Ordering ord(int l, int r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
}
static inline Ordering ord(short l, short r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
}
static inline Ordering ord(long l, long r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
}
static inline Ordering ord(long long l, long long r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
}
static inline Ordering ord(float l, float r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
}
static inline Ordering ord(double l, double r)
{
    return (l == r ? OrdEqual : (l > r ? OrdGreater : OrdLess));
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

    if( i < r.size() )
        return OrdLess;
    return OrdEqual;
}
template<typename T, typename U>
Ordering ord(const ::std::map<T,U>& l, const ::std::map<T,U>& r)
{
    auto r_it = r.begin();
    for(const auto& le : l)
    {
        if( r_it == r.end() )
            return OrdGreater;
        auto rv = ::ord( le, *r_it );
        if( rv != OrdEqual )
            return rv;
        ++ r_it;
    }
    return OrdEqual;
}
#define ORD(a,b)    do { Ordering ORD_rv = ::ord(a,b); if( ORD_rv != ::OrdEqual )   return ORD_rv; } while(0)


template <typename T>
struct LList
{
    const LList*  m_prev;
    T   m_item;

    LList():
        m_prev(nullptr)
    {}
    LList(const LList* prev, T item):
        m_prev(prev),
        m_item( ::std::move(item) )
    {
    }

    LList end() const {
        return LList();
    }
    LList begin() const {
        return *this;
    }
    bool operator==(const LList& x) {
        return m_prev == x.m_prev;
    }
    bool operator!=(const LList& x) {
        return m_prev != x.m_prev;
    }
    void operator++() {
        assert(m_prev);
        *this = *m_prev;
    }
    const T& operator*() const {
        return m_item;
    }
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
inline auto operator<<(::std::ostream& os, const T& v) -> decltype(v.fmt(os)) {
    return v.fmt(os);
}

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
template <typename T>
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::set<T>& v) {
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

}   // namespace std

struct FmtEscaped {
    const char* s;
    FmtEscaped(const ::std::string& s):
        s(s.c_str())
    {}
    // See main.cpp
    friend ::std::ostream& operator<<(::std::ostream& os, const FmtEscaped& x);
};

// -------------------------------------------------------------------
// --- Reversed iterable
template <typename T>
struct reversion_wrapper { T& iterable; };

template <typename T>
//auto begin (reversion_wrapper<T> w) { return ::std::rbegin(w.iterable); }
auto begin (reversion_wrapper<T> w) { return w.iterable.rbegin(); }

template <typename T>
//auto end (reversion_wrapper<T> w) { return ::std::rend(w.iterable); }
auto end (reversion_wrapper<T> w) { return w.iterable.rend(); }

template <typename T>
reversion_wrapper<T> reverse (T&& iterable) { return { iterable }; }


template<typename T>
struct RunIterable {
    const T& list;
    unsigned int ofs;
    ::std::pair<size_t,size_t> cur;
    RunIterable(const T& list):
        list(list), ofs(0)
    {
        advance();
    }
    void advance() {
        if( ofs < list.size() )
        {
            auto start = ofs;
            while(ofs < list.size() && list[ofs] == list[start])
                ofs ++;
            cur = ::std::make_pair(start, ofs-1);
        }
        else
        {
            ofs = list.size()+1;
        }
    }
    RunIterable<T> begin() { return *this; }
    RunIterable<T> end() { auto rv = *this; rv.ofs = list.size()+1; return rv; }
    bool operator==(const RunIterable<T>& x) {
        return x.ofs == ofs;
    }
    bool operator!=(const RunIterable<T>& x) {
        return !(*this == x);
    }
    void operator++() {
        advance();
    }
    const ::std::pair<size_t,size_t>& operator*() const {
        return this->cur;
    }
    const ::std::pair<size_t,size_t>* operator->() const {
        return &this->cur;
    }
};
template<typename T>
RunIterable<T> runs(const T& x) {
    return RunIterable<T>(x);
}

template<typename T>
class NullOnDrop {
    T*& ptr;
public:
    NullOnDrop(T*& ptr):
        ptr(ptr)
    {}
    ~NullOnDrop() {
        DEBUG("NULL " << &ptr);
        ptr = nullptr;
    }
};


#endif
