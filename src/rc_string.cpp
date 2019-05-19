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
Ordering RcString::ord(const RcString& x) const
{
    if( m_ptr == x.m_ptr )
        return OrdEqual;
    // Both can't be empty/null
    if( m_ptr == nullptr )
        return OrdLess;
    if( x.m_ptr == nullptr )
        return OrdGreater;

    assert(x.size() > 0);
    assert(this->size() > 0);

    auto xp = x.c_str();
    auto tp = this->c_str();
    for(size_t i = 0; i < ::std::min(this->size(), x.size()); i ++)
    {
        if( *xp != *tp )
            return ::ord((unsigned)*xp, (unsigned)*tp);
        xp ++;
        tp ++;
    }
    return ::ord((unsigned)this->size(), (unsigned)x.size());
}
Ordering RcString::ord(const std::string& x) const
{
    if( m_ptr == nullptr && x.size() == 0 )
        return OrdEqual;
    // Both can't be empty/null
    if( m_ptr == nullptr )
        return OrdLess;
    if( x.empty() )
        return OrdGreater;

    assert(x.size() > 0);
    assert(this->size() > 0);

    auto xp = x.c_str();
    auto tp = this->c_str();
    for(size_t i = 0; i < ::std::min(this->size(), x.size()); i ++)
    {
        if( *xp != *tp )
            return ::ord((unsigned)*xp, (unsigned)*tp);
        xp ++;
        tp ++;
    }
    return ::ord((unsigned)this->size(), (unsigned)x.size());
}
bool RcString::operator==(const char* s) const
{
    if( m_ptr == nullptr )
        return *s == '\0';
    const char* ts = this->c_str();
    const char* end = ts + this->size();
    // Loop while not at the end of either
    while(s && ts != end)
    {
        if( *s != *ts )
            return false;
        s ++;
        ts ++;
    }
    // Only equal if we're at the end of both strings
    return *s == '\0' && ts == end;
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
    return *RcString_interned_strings.insert(RcString(s)).first;
#endif
}

size_t std::hash<RcString>::operator()(const RcString& s) const noexcept
{
    size_t h = 5381;
    for(auto c : s) {
        h = h * 33 + (unsigned)c;
    }
    return h;
    //return hash<std::string_view>(s.c_str(), s.size());
}
