/*
 * mrustc common tools
 * - by John Hodge (Mutabah)
 *
 * tools/common/helpers.h
 * - General helper classes
 */
// TODO: Replace this header with src/includ/string_view.hpp
#pragma once

#include <string>
#include <cstring>
#include <iostream>

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

    char operator[](size_t n) const {
        return m_start[n];
    }

    operator ::std::string() const {
        return ::std::string { m_start, m_start + m_len };
    }
    friend ::std::string& operator+=(::std::string& x, const string_view& sv) {
        x.append(sv.m_start, sv.m_start+sv.m_len);
        return x;
    }
    friend ::std::ostream& operator<<(::std::ostream& os, const string_view& sv) {
        os.write(sv.m_start, sv.m_len);
        return os;
    }

    const char* begin() const {
        return m_start;
    }
    const char* end() const {
        return m_start+m_len;
    }
};


} // namespace helpers
