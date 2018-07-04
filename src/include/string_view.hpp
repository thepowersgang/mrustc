/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/string_view.hpp
 * - Clone of the `string_view` class (introduced in C++17)
 */
#pragma once
#include <string>
#include <cstddef>  // size_t
#include <iostream> // ostream

namespace stdx {
using namespace std;

class string_view
{
    const char* m_start;
    const char* m_end;

public:
    string_view():
        m_start(nullptr), m_end(nullptr)
    {
    }
    string_view(const char* s, const char* e):
        m_start(s), m_end(e)
    {
        if(!(s <= e))
            throw ::std::invalid_argument("start must be before end for string_view");
    }
    string_view(const char* s):
        m_start(s), m_end(s)
    {
        while(*m_end)
            m_end ++;
    }
    string_view(const string& s):
        m_start(s.data()), m_end(m_start + s.size())
    {
    }

    size_t size() const {
        return m_end - m_start;
    }

    bool operator==(const string_view& x) const { return cmp(x) == 0; }
    bool operator!=(const string_view& x) const { return cmp(x) != 0; }
    bool operator< (const string_view& x) const { return cmp(x) <  0; }
    bool operator> (const string_view& x) const { return cmp(x) >  0; }
    bool operator<=(const string_view& x) const { return cmp(x) <= 0; }
    bool operator>=(const string_view& x) const { return cmp(x) >= 0; }
    bool operator==(const char* x) const { return cmp(string_view(x)) == 0; }
    bool operator!=(const char* x) const { return cmp(string_view(x)) != 0; }
    bool operator< (const char* x) const { return cmp(string_view(x)) <  0; }
    bool operator> (const char* x) const { return cmp(string_view(x)) >  0; }
    bool operator<=(const char* x) const { return cmp(string_view(x)) <= 0; }
    bool operator>=(const char* x) const { return cmp(string_view(x)) >= 0; }
    bool operator==(const string& x) const { return cmp(string_view(x)) == 0; }
    bool operator!=(const string& x) const { return cmp(string_view(x)) != 0; }
    bool operator< (const string& x) const { return cmp(string_view(x)) <  0; }
    bool operator> (const string& x) const { return cmp(string_view(x)) >  0; }
    bool operator<=(const string& x) const { return cmp(string_view(x)) <= 0; }
    bool operator>=(const string& x) const { return cmp(string_view(x)) >= 0; }

    friend ::std::ostream& operator<<(::std::ostream& os, const string_view& x) {
        for(const char* s = x.m_start; s != x.m_end; s++)
            os << *s;
        return os;
    }

private:
    int cmp(const string_view& x) const {
        const char *a, *b;
        for( a = m_start, b = x.m_start; a != m_end && b != x.m_end; a++, b++)
        {
            if( *a != *b ) {
                return *a < *b ? -1 : 1;
            }
        }
        if( a == m_end && b == x.m_end )
            return 0;
        if( a == m_end )
            return -1;
        else
            return 1;
    }
};

}
