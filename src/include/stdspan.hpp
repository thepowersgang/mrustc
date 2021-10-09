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
    unsigned int    m_len;
public:
    span():
        m_first(nullptr),
        m_len(0)
    {}
    span(const ::std::vector<T>& v):
        m_first(&v[0]),
        m_len(v.size())
    {}
    span(::std::vector<T>& v):
        m_first(v.data()),
        m_len(v.size())
    {}
    span(T* ptr, unsigned int len):
        m_first(ptr),
        m_len(len)
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
    span<T> subspan(unsigned int ofs, unsigned int len) const {
        assert(ofs < m_len);
        assert(len <= m_len);
        assert(ofs + len <= m_len);
        return span { m_first + ofs, len };
    }

    T* begin() const { return m_first; }
    T* end() const { return m_first + m_len; }

    T& front() const { return m_first[0]; }
    T& back() const { return m_first[m_len-1]; }
};

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
