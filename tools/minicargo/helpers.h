#pragma once

#include <string>
#include <cstring>

namespace helpers {

class string_view
{
    const char* m_start;
    const size_t m_len;
public:
    string_view(const char* s, size_t n):
        m_start(s), m_len(n)
    {
    }

    bool operator==(const ::std::string& s) const {
        return *this == s.c_str();
    }
    bool operator==(const char* s) const {
        if(::std::strncmp(m_start, s, m_len) != 0)
            return false;
        return s[m_len] == '\0';
    }
    friend ::std::string& operator+=(::std::string& x, const string_view& sv) {
        x.append(sv.m_start, sv.m_start+sv.m_len);
        return x;
    }
};


} // namespace helpers
