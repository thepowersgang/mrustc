/*
 */
#include <rc_string.hpp>
#include <cstring>
#include <iostream>

RcString::RcString(const char* s, unsigned int len):
    m_ptr(nullptr),
    m_len(len)
{
    if( len > 0 )
    {
        m_ptr = new unsigned int[1 + (len+1 + sizeof(unsigned int)-1) / sizeof(unsigned int)];
        *m_ptr = 1;
        char* data_mut = reinterpret_cast<char*>(m_ptr + 1);
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
bool RcString::operator==(const char* s) const
{
    if( m_len == 0 )
        return *s == '\0';
    auto m = this->c_str();
    do {
        if( *m != *s )
            return false;
    } while( *m++ != '\0' && *s++ != '\0' );
    return true;
}
