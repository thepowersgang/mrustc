/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/stdspan.hpp
 * - Clone of the C++20 span class
 */
#pragma once
#include <vector>
#include <iostream> // ostream

namespace std {

template<typename T>
class span
{
    T*  m_first;
    size_t  m_len;
public:
    span():
        m_first(nullptr),
        m_len(0)
    {}
    template< class It >
    constexpr span( It first, It last )
        : m_first( &*first )
        , m_len( last - first )
    {}
    constexpr span( const span<typename std::remove_const<T>::type>& x )
        : m_first( x.m_first )
        , m_len( x.m_len )
    {}
    constexpr span( std::vector<typename std::remove_const<T>::type>& x )
        : m_first( x.data() )
        , m_len( x.size() )
    {}
    constexpr span( const std::vector<typename std::remove_const<T>::type>& x )
        : m_first( x.data() )
        , m_len( x.size() )
    {}
    span(T* ptr, size_t len):
        m_first(ptr),
        m_len(len)
    {}

    ::std::vector<typename std::remove_const<T>::type> to_vec() const {
        return ::std::vector<typename std::remove_const<T>::type>(begin(), end());
    }

    constexpr size_t size() const { return m_len; }
    constexpr bool empty() const { return m_len == 0; }
    T& operator[](size_t i) const {
        assert(i < m_len);
        return m_first[i];
    }
    span<T> subspan(size_t ofs, size_t len) const {
        assert(ofs <= m_len);
        assert(len <= m_len);
        assert(ofs + len <= m_len);
        return span { m_first + ofs, len };
    }

    constexpr T* data() const noexcept { return m_first; }

    T* begin() const { return m_first; }
    T* end() const { return m_first + m_len; }

    T& front() const { return m_first[0]; }
    T& back() const { return m_first[m_len-1]; }
};
template<typename T> T* begin(const std::span<T>& x) { return x.begin(); }
template<typename T> T* end(const std::span<T>& x) { return x.end(); }

template<typename T>
::std::ostream& operator<<(::std::ostream& os, span<T> s) {
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

}
