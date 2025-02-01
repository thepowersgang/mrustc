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
// XXX: Evil hack - Define 'mv$' to be ::std::move, so there's a shorter name for it
#define mv$(...) ::std::move(__VA_ARGS__)
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

class FmtEscaped {
    const char* s;
    const char* e;
public:
    FmtEscaped(const ::std::string& s):
        s(s.c_str()),
        e(s.c_str() + s.size())
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


/// A thin vector type (single-pointer) that cannot resize, and stores the vector length in the pointed-to memory
///
/// Used for HIR structures to save significant amounts of memory
template<typename T>
class ThinVector
{
    struct Meta {
        size_t  len;
        size_t  cap;
    };
    T*    m_ptr;
public:
    ~ThinVector() {
        if(m_ptr)
        {
            auto* m = meta();
            auto len = m->len;
            m->len = 0;
            for(size_t i = 0; i < len; i ++) {
                m_ptr[i].~T();
            }
            free(m);
            m_ptr = nullptr;
        }
    }

    ThinVector(): m_ptr(nullptr) {}
    explicit ThinVector(size_t len)
        : m_ptr(nullptr)
    {
        if(len > 0)
        {
            this->reserve_init(len);
            auto* meta = this->meta();
            for(size_t i = 0; i < len; i ++) {
                new(&m_ptr[i]) T;
                meta->len ++;
            }
        }
    }
    ThinVector(const T* begin, const T* end)
        : m_ptr(nullptr)
    {
        if(begin != end)
        {
            this->reserve_init(end - begin);
            auto* meta = this->meta();
            for(auto it = begin; it != end; ++it) {
                new(&m_ptr[meta->len]) T(*it);
                meta->len ++;
            }
        }
    }
    explicit ThinVector(const std::vector<T>& x): ThinVector(x.data(), x.data() + x.size()) {}

    ThinVector(const ThinVector& x): ThinVector(x.data(), x.data() + x.size()) {}
    ThinVector(ThinVector&& x): m_ptr(x.m_ptr) { x.m_ptr = nullptr; }

    ThinVector& operator=(const ThinVector& x) { this->~ThinVector(); new(this) ThinVector(x); return *this; }
    ThinVector& operator=(ThinVector&& x) { this->~ThinVector(); this->m_ptr = x.m_ptr; x.m_ptr = nullptr; return *this; }

    void reserve(size_t new_cap) {
        if(new_cap > this->capacity())
        {
            auto saved = std::move(*this);
            this->reserve_init(new_cap);
            for(auto& v : saved) {
                this->push_back(std::move(v));
            }
        }
    }
    void resize(size_t len) {
        this->reserve(len);
        auto* m = this->meta();
        if( m )
        {
            while( m->len > len ) {
                m->len --;
                m_ptr[m->len].~T();
            }
            while( m->len < len ) {
                assert(meta() == m);
                this->push_back(T());
            }
        }
    }

    void reserve_init(size_t cap) {
        if(m_ptr)   throw std::runtime_error("Initialising an initialised ThinVector");
        if(cap > 0)
        {
            auto* p = static_cast<T*>( malloc( sizeof(T) * (cap + metadata_len()) ) );
            if(!p) {
                throw ::std::bad_alloc();
            }
            auto* meta = (Meta*)p;
            m_ptr = p + metadata_len();
            meta->cap = cap;
            meta->len = 0;
        }
    }
    template<typename... Args>
    void emplace_back(Args&&... v) {
        auto len = size();
        if(!meta() || meta()->cap == 0) {
            this->reserve(2);
        }
        else if(len >= meta()->cap) {
            this->reserve((len + 1) * 3 / 2);
        }
        new(&m_ptr[len]) T(std::move(v)...);
        this->meta()->len ++;
    }
    void push_back(T v) {
        this->emplace_back(std::move(v));
    }
    void pop_back() {
        auto* m = this->meta();
        if(m && m->len > 0 ) {
            m->len --;
            m_ptr[m->len].~T();
        }
    }
    
    const T& front() const { if(this->size() == 0) throw std::out_of_range("ThinVector::front"); return *m_ptr; }
          T& front()       { if(this->size() == 0) throw std::out_of_range("ThinVector::front"); return *m_ptr; }
    const T& back() const { if(this->size() == 0) throw std::out_of_range("ThinVector::back"); return m_ptr[size()-1]; }
          T& back()       { if(this->size() == 0) throw std::out_of_range("ThinVector::back"); return m_ptr[size()-1]; }
    const T* begin() const { return m_ptr; }
          T* begin()       { return m_ptr; }
    const T* end() const { return m_ptr + size(); }
          T* end()       { return m_ptr + size(); }

    const T& operator[](size_t i) const { return m_ptr[i]; }
          T& operator[](size_t i)       { return m_ptr[i]; }
    const T& at(size_t i) const { if(i >= this->size()) throw std::out_of_range("ThinVector::at"); return m_ptr[i]; }
          T& at(size_t i)       { if(i >= this->size()) throw std::out_of_range("ThinVector::at"); return m_ptr[i]; }
    const T* data() const { return m_ptr; }
          T* data()       { return m_ptr; }
    size_t size() const {
        if(m_ptr) {
            return meta()->len;
        }
        else {
            return 0;
        }
    }
    size_t capacity() const {
        if(m_ptr) {
            return meta()->cap;
        }
        else {
            return 0;
        }
    }
    bool empty() const {
        return m_ptr == nullptr;
    }

    Ordering ord(const ThinVector<T>& x) const {
        size_t cmp_len = this->size();
        if(cmp_len > x.size()) {
            cmp_len = x.size();
        }
        for(size_t i = 0; i < cmp_len; i ++ ) {
            auto rv = ::ord( (*this)[i], x[i] );
            if( rv != OrdEqual )
                return rv;
        }

        // Longer lists sort afer shorter ones
        if( this->size() < x.size() ) {
            return OrdLess;
        }
        else if( this->size() > x.size() ) {
            return OrdGreater;
        }
        else {
           return OrdEqual;
        }
    }
private:
    static size_t metadata_len() {
        //static_assert(sizeof(T) > 0, "");
        return (sizeof(Meta) + sizeof(T) - 1) / sizeof(T);
    }
    const Meta* meta() const { return m_ptr ? (const Meta*)(m_ptr-metadata_len()) : nullptr; }
          Meta* meta()       { return m_ptr ? (      Meta*)(m_ptr-metadata_len()) : nullptr; }
};
template <typename T>
inline ::std::ostream& operator<<(::std::ostream& os, const ThinVector<T>& v) {
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


#endif
