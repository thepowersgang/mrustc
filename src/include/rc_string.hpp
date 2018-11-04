/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/rc_string.hpp
 * - Reference-counted string (used for spans)
 */
#pragma once

#include <cstring>
#include <ostream>

class RcString
{
    unsigned int*   m_ptr;
    unsigned int    m_len;
public:
    RcString():
        m_ptr(nullptr),
        m_len(0)
    {}
    RcString(const char* s, unsigned int len);
    RcString(const char* s):
        RcString(s, ::std::strlen(s))
    {
    }
    RcString(const ::std::string& s):
        RcString(s.data(), s.size())
    {
    }

    RcString(const RcString& x);
    RcString(RcString&& x):
        m_ptr(x.m_ptr),
        m_len(x.m_len)
    {
        x.m_ptr = nullptr;
        x.m_len = 0;
    }

    ~RcString();

    RcString& operator=(const RcString& x)
    {
        if( !(&x != this) ) throw "";

        this->~RcString();
        new (this) RcString(x);

        return *this;
    }
    RcString& operator=(RcString&& x)
    {
        if( !(&x != this) ) throw "";

        this->~RcString();
        new (this) RcString( ::std::move(x) );
        return *this;
    }


    const char* c_str() const {
        if( m_len > 0 ) {
            return reinterpret_cast<const char*>(m_ptr + 1);
        }
        else {
            return "";
        }
    }
    bool operator==(const RcString& s) const { return *this == s.c_str(); }
    bool operator==(const char* s) const;
    friend ::std::ostream& operator<<(::std::ostream& os, const RcString& x) {
        return os << x.c_str();
    }
};
