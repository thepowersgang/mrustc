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
#include <algorithm>    // std::max

RcString::RcString(const char* s, size_t len):
    m_ptr(nullptr)
{
    if( len > 0 )
    {
        size_t nwords = (len+1 + sizeof(unsigned int)-1) / sizeof(unsigned int);
        m_ptr = reinterpret_cast<Inner*>(malloc(sizeof(Inner) + (nwords - 1) * sizeof(unsigned int)));
        m_ptr->refcount = 1;
        m_ptr->size = static_cast<unsigned>(len);
        m_ptr->ordering = 0;
        char* data_mut = reinterpret_cast<char*>(m_ptr->data);
        for(unsigned int j = 0; j < len; j ++ )
            data_mut[j] = s[j];
        data_mut[len] = '\0';

        //::std::cout << "RcString(" << m_ptr << " \"" << *this << "\") - " << *m_ptr << " (creation)" << ::std::endl;
    }
}
RcString::~RcString()
{
    if(m_ptr)
    {
        m_ptr->refcount -= 1;
        //::std::cout << "RcString(" << m_ptr << " \"" << *this << "\") - " << *m_ptr << " refs left (drop)" << ::std::endl;
        if( m_ptr->refcount == 0 )
        {
            free(m_ptr);
        }
        m_ptr = nullptr;
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


struct Cmp_RcString_Raw {
    bool operator()(const RcString& a, const RcString& b) const {
        return a.ord(b.c_str(), b.size()) == OrdLess;
    }
};
// A set with a comparison function that always checks bytes (avoiding recursion with the cache)
::std::set<RcString,Cmp_RcString_Raw>    RcString_interned_strings;
bool    RcString_interned_ordering_valid;

RcString RcString::new_interned(const char* s, size_t len)
{
    if(len == 0)
        return RcString();
    auto ret = RcString_interned_strings.insert(RcString(s, len));
    // Set interned and invalidate the cache if an insert happened
    if(ret.second)
    {
        ret.first->m_ptr->ordering = 1;
        RcString_interned_ordering_valid = false;
    }
    return *ret.first;
}
Ordering RcString::ord_interned(const RcString& s) const
{
    assert(s.is_interned() && this->is_interned());
    if(!RcString_interned_ordering_valid)
    {
        // Populate cache
        unsigned i = 1;
        for(auto& e : RcString_interned_strings)
            e.m_ptr->ordering = i++;
        RcString_interned_ordering_valid = true;
    }
    return ::ord(this->m_ptr->ordering, s.m_ptr->ordering);
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
