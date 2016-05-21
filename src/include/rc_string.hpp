/*
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
    RcString(const char* s, unsigned int len):
        m_ptr( new unsigned int[1 + (len+1 + sizeof(unsigned int)-1) / sizeof(unsigned int)] ),
        m_len(len)
    {
        *m_ptr = 1;
        char* data_mut = reinterpret_cast<char*>(m_ptr + 1);
        for(unsigned int j = 0; j < len; j ++ )
            data_mut[j] = s[j];
        data_mut[len] = '\0';
    }
    RcString(const char* s):
        RcString(s, ::std::strlen(s))
    {
    }
    RcString(const ::std::string& s):
        RcString(s.data(), s.size())
    {
    }
    
    RcString(const RcString& x):
        m_ptr(x.m_ptr),
        m_len(x.m_len)
    {
        *m_ptr += 1;
    }
    RcString(RcString&& x):
        m_ptr(x.m_ptr),
        m_len(x.m_len)
    {
        x.m_ptr = nullptr;
        x.m_len = 0;
    }
    
    ~RcString()
    {
        if(m_ptr)
        {
            *m_ptr -= 1;
            if( *m_ptr == 0 )
            {
                delete[] m_ptr;
                m_ptr = nullptr;
            }
        }
    }
    
    RcString& operator=(const RcString& x)
    {
        if( &x != this )
        {
            this->~RcString();
            m_ptr = x.m_ptr;
            m_len = x.m_len;
            *m_ptr += 1;
        }
        return *this;
    }
    RcString& operator=(RcString&& x)
    {
        if( &x != this )
        {
            this->~RcString();
            m_ptr = x.m_ptr;
            m_len = x.m_len;
            x.m_ptr = nullptr;
            x.m_len = 0;
        }
        return *this;
    }
    
    
    const char* c_str() const {
        return reinterpret_cast<const char*>(m_ptr + 1);
    }
    bool operator==(const char* s) const {
        if( m_len == 0 )
            return *s == '\0';
        auto m = this->c_str();
        do {
            if( *m != *s )
                return false;
        } while( *m++ != '\0' && *s++ != '\0' );
        return true;
    }
    friend ::std::ostream& operator<<(::std::ostream& os, const RcString& x) {
        return os << x.c_str();
    }
};
