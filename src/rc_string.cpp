/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * rc_string.cpp
 * - Reference-counted string
 */
#include <rc_string.hpp>
#include <cstring>
#include <string>
#include <iostream>

RcString::RcString(const char* s, unsigned int len):
    m_ptr(nullptr)
{
    if( len > 0 )
    {
        m_ptr = new unsigned int[2 + (len+1 + sizeof(unsigned int)-1) / sizeof(unsigned int)];
        m_ptr[0] = 1;
        m_ptr[1] = len;
        char* data_mut = reinterpret_cast<char*>(m_ptr + 2);
        for(unsigned int j = 0; j < len; j ++ )
            data_mut[j] = s[j];
        data_mut[len] = '\0';
    }
}
RcString::~RcString()
{
    if(m_ptr)
    {
        *m_ptr -= 1;
        //::std::cout << "RcString(\"" << *this << "\") - " << *m_ptr << " refs left" << ::std::endl;
        if( *m_ptr == 0 )
        {
            delete[] m_ptr;
            m_ptr = nullptr;
        }
    }
}
Ordering RcString::ord(const char* s, size_t len) const
{
    if( m_ptr == nullptr )
        return (len == 0 ? OrdEqual : OrdLess);
    if( len == 0 )
        return OrdGreater;

    assert(this->size() > 0);
    assert(len > 0);

    int cmp = memcmp(this->c_str(), s, ::std::min(len, this->size()));
    if(cmp == 0)
        return ::ord(this->size(), len);
    return ::ord(cmp, 0);
}
Ordering RcString::ord(const char* s) const
{
    if( m_ptr == nullptr )
        return (*s == '\0' ? OrdEqual : OrdLess);

    int cmp = strncmp(this->c_str(), s, this->size());
    if( cmp == 0 )
    {
        if( s[this->size()] == '\0' )
            return OrdEqual;
        else
            return OrdLess;
    }
    return ::ord(cmp, 0);
}

::std::ostream& operator<<(::std::ostream& os, const RcString& x)
{
    for(size_t i = 0; i < x.size(); i ++)
    {
        os << x.c_str()[i];
    }
    return os;
}


::std::set<RcString>    RcString_interned_strings;

RcString RcString::new_interned(const ::std::string& s)
{
#if 0
    auto it = RcString_interned_strings.find(s);
    if( it == RcString_interned_strings.end() )
    {
        it = RcString_interned_strings.insert(RcString(s)).first;
    }
    return *it;
#else
    // TODO: interning flag, so comparisons can just be a pointer comparison
    return *RcString_interned_strings.insert(RcString(s)).first;
#endif
}

size_t std::hash<RcString>::operator()(const RcString& s) const noexcept
{
    // http://www.cse.yorku.ca/~oz/hash.html "djb2"
    size_t h = 5381;
    for(auto c : s) {
        h = h * 33 + (unsigned)c;
    }
    return h;
    //return hash<std::string_view>(s.c_str(), s.size());
}
