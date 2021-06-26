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
#include "../common.hpp"

class RcString
{
    struct Inner {
        unsigned int    refcount;
        unsigned int    size;
        unsigned int    ordering;   // Populated only for interned strings, 0 otherwise
        unsigned int    data[1];    // Actually arbitary
    }*  m_ptr;
public:
    RcString():
        m_ptr(nullptr)
    {}
    RcString(const char* s, size_t len);
    RcString(const char* s):
        RcString(s, ::std::strlen(s))
    {
    }
    explicit RcString(const ::std::string& s):
        RcString(s.data(), s.size())
    {
    }

    static RcString new_interned(const char* s, size_t len);
    static RcString new_interned(const ::std::string& s) {
        return new_interned(s.data(), s.size());
    }
    static RcString new_interned(const char* s) {
        return new_interned(s, ::std::strlen(s));
    }

    RcString(const RcString& x):
        m_ptr(x.m_ptr)
    {
        if( m_ptr ) m_ptr->refcount += 1;
    }
    RcString(RcString&& x):
        m_ptr(x.m_ptr)
    {
        x.m_ptr = nullptr;
    }

    ~RcString();

    RcString& operator=(const RcString& x)
    {
        if( &x != this )
        {
            this->~RcString();
            m_ptr = x.m_ptr;
            if( m_ptr ) m_ptr->refcount += 1;
        }
        return *this;
    }
    RcString& operator=(RcString&& x)
    {
        if( &x != this )
        {
            this->~RcString();
            m_ptr = x.m_ptr;
            x.m_ptr = nullptr;
        }
        return *this;
    }

    const char* begin() const { return c_str(); }
    const char* end() const { return c_str() + size(); }

    bool is_interned() const { return m_ptr && m_ptr->ordering != 0; }
    size_t size() const { return m_ptr ? m_ptr->size : 0; }
    const char* c_str() const {
        if( m_ptr )
        {
            return reinterpret_cast<const char*>(m_ptr->data);
        }
        else
        {
            return "";
        }
    }

    char back() const {
        assert(size() > 0 );
        return *(c_str() + size() - 1);
    }

    Ordering ord(const char* s, size_t l) const;
    Ordering ord_interned(const RcString& s) const;

    Ordering ord(const RcString& s) const {
        if( m_ptr == s.m_ptr )
            return OrdEqual;
        if( !m_ptr || !s.m_ptr)
            return m_ptr ? OrdGreater : OrdLess;
        // If both are interned, then use stored sorting
        if( is_interned() && s.is_interned() )
            return ord_interned(s);
        return ord(s.c_str(), s.size());
    }
    bool operator==(const RcString& s) const {
        if(s.size() != this->size())
            return false;
        return this->ord(s) == OrdEqual;
    }
    bool operator!=(const RcString& s) const {
        if(s.size() != this->size())
            return true;
        return this->ord(s) != OrdEqual;
    }
    bool operator<(const RcString& s) const { return this->ord(s) == OrdLess; }
    bool operator>(const RcString& s) const { return this->ord(s) == OrdGreater; }

    Ordering ord(const std::string& s) const { return ord(s.data(), s.size()); }
    bool operator==(const std::string& s) const { return this->ord(s) == OrdEqual; }
    bool operator!=(const std::string& s) const { return this->ord(s) != OrdEqual; }
    bool operator<(const std::string& s) const { return this->ord(s) == OrdLess; }
    bool operator>(const std::string& s) const { return this->ord(s) == OrdGreater; }

    Ordering ord(const char* s) const;
    bool operator==(const char* s) const { return this->ord(s) == OrdEqual; }
    bool operator!=(const char* s) const { return this->ord(s) != OrdEqual; }

    friend ::std::ostream& operator<<(::std::ostream& os, const RcString& x);

    friend bool operator==(const char* a, const RcString& b) {
        return b == a;
    }
    friend bool operator!=(const char* a, const RcString& b) {
        return b != a;
    }

    int compare(size_t o, size_t l, const char* s) const {
        assert(o <= this->size());
        if(l <= this->size() - o) {
            return memcmp(this->c_str() + o, s, l);
        }
        else {
            if(int rv = memcmp(this->c_str() + o, s, this->size() - o))
                return rv;
            return -1;
        }
    }
};

namespace std {
    static inline bool operator==(const string& a, const ::RcString& b) {
        return b == a;
    }
    static inline bool operator!=(const string& a, const ::RcString& b) {
        return b != a;
    }
    template<> struct hash<RcString>
    {
        size_t operator()(const RcString& s) const noexcept;
    };
}
