#pragma once

#include <string>

namespace helpers {


/// Path helper class (because I don't want to include boost)
class path
{
#ifdef _WIN32
    const char SEP = '\\';
#else
    const char SEP = '/';
#endif

    ::std::string   m_str;

    path()
    {
    }
public:
    path(const ::std::string& s):
        path(s.c_str())
    {
    }
    path(const char* s):
        m_str(s)
    {
        // 1. Normalise path separators to the system specified separator
        for(size_t i = 0; i < m_str.size(); i ++)
        {
            if( m_str[i] == '/' || m_str[i] == '\\' )
                m_str[i] = SEP;
        }

        // 2. Remove any trailing separators
        if( !m_str.empty() )
        {
            while(!m_str.empty() && m_str.back() == SEP )
                m_str.pop_back();
            if(m_str.empty())
            {
                m_str.push_back(SEP);
            }
        }
        else
        {
            throw ::std::runtime_error("Empty path being constructed");
        }
    }

    path operator/(const path& p) const
    {
        if(p.m_str[0] == '/')
            throw ::std::runtime_error("Appending an absolute path to another path");
        return *this / p.m_str.c_str();
    }
    /// Append a relative path
    path operator/(const char* o) const
    {
        if (o[0] == '/')
            throw ::std::runtime_error("Appending an absolute path to another path");
        auto rv = *this;
        rv.m_str.push_back(SEP);
        rv.m_str.append(o);
        return rv;
    }
    /// Add an arbitary string to the final component
    path operator+(const char* o) const
    {
        if( ::std::strchr(o, SEP) != nullptr )
            throw ::std::runtime_error("Appending a string containing the path separator (with operator+)");
        auto rv = *this;
        rv.m_str.append(o);
        return rv;
    }

    path parent() const
    {
        auto pos = m_str.find_last_of(SEP);
        if(pos == ::std::string::npos)
        {
            return *this;
        }
        else
        {
            path rv;
            rv.m_str = m_str.substr(0, pos);
            return rv;
        }
    }

    operator ::std::string() const
    {
        return m_str;
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const path& p)
    {
        return os << p.m_str;
    }
};

} // namespace helpers