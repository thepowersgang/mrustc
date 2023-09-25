/*
 * mrustc common code
 * - by John Hodge (Mutabah)
 *
 * tools/common/path.h
 * - Generic representation of a filesystem path (HEADER)
 */
#pragma once

#include <string>
#include <stdexcept>
#include "helpers.h"

namespace helpers {

/// Path helper class (because I don't want to include boost)
class path
{
#ifdef _WIN32
    static const char SEP = '\\';
#else
    static const char SEP = '/';
#endif

    ::std::string   m_str;

public:
    path()
    {
    }
    path(const ::std::string& s):
        path(s.c_str())
    {
    }
    path(const char* s);

    bool is_valid() const {
        return m_str != "";
    }
    bool is_absolute() const {
        return m_str != "" && m_str[0] == '/';
    }

    bool operator==(const path& p) const {
        return m_str == p.m_str;
    }
    bool operator!=(const path& p) const {
        return m_str != p.m_str;
    }

    path& operator/=(const path& p)
    {
        if(!p.is_valid())
            throw ::std::runtime_error("Appending from an invalid path");

        return *this /= p.m_str.c_str();
    }
    path& operator/=(const char* o)
    {
        if(!this->is_valid())
            throw ::std::runtime_error("Appending to an invalid path");
        if(o[0] == '/')
            throw ::std::runtime_error("Appending an absolute path to another path");
        this->m_str.push_back(SEP);
        this->m_str.append(o);
        return *this;
    }
    path& operator/=(const string_view& o)
    {
        if(!this->is_valid())
            throw ::std::runtime_error("Appending to an invalid path");
        if(o[0] == '/')
            throw ::std::runtime_error("Appending an absolute path to another path");
        this->m_str.push_back(SEP);
        this->m_str += o;
        return *this;
    }

    path operator/(const path& p) const
    {
        auto rv = *this;
        rv /= p;
        return rv;
    }
    /// Append a relative path
    path operator/(const char* o) const
    {
        auto rv = *this;
        rv /= o;
        return rv;
    }
    /// Add an arbitary string to the final component
    path operator+(const char* o) const
    {
        if(!this->is_valid())
            throw ::std::runtime_error("Appending a string to an invalid path");
        if( ::std::strchr(o, SEP) != nullptr )
            throw ::std::runtime_error("Appending a string containing the path separator (with operator+)");
        auto rv = *this;
        rv.m_str.append(o);
        return rv;
    }

    bool pop_component()
    {
        if(!this->is_valid())
            throw ::std::runtime_error("Calling pop_component() on an invalid path");
        auto pos = m_str.find_last_of(SEP);
        if(pos == ::std::string::npos || pos == 0)
        {
            return false;
        }
        else
        {
            this->m_str.resize(pos);
            return true;
        }
    }
    path parent() const
    {
        if(!this->is_valid())
            throw ::std::runtime_error("Calling parent() on an invalid path");
        auto pos = m_str.find_last_of(SEP);
        if(pos == ::std::string::npos)
        {
            return path();
        }
        else
        {
            path rv;
            rv.m_str = m_str.substr(0, pos);
            return rv;
        }
    }
    path to_absolute() const;
    ::std::string basename() const
    {
        if(!this->is_valid())
            throw ::std::runtime_error("Calling basename() on an invalid path");

        auto pos = m_str.find_last_of(SEP);
        if(pos == ::std::string::npos)
        {
            return m_str;
        }
        else
        {
            return m_str.substr(pos+1);
        }
    }

    const ::std::string& str() const
    {
        return m_str;
    }
    operator ::std::string() const
    {
        return m_str;
    }

    class ComponentsIter
    {
        const path& p;
        size_t  pos;
        size_t  end;

        friend class path;
        ComponentsIter(const path& p, size_t i): p(p), pos(i) {
            end = p.m_str.find(SEP, pos);
            if(end == ::std::string::npos)
                end = p.m_str.size();
        }
    public:
        string_view operator*() const {
            return string_view(p.m_str.c_str() + pos, end - pos);
        }
        void operator++();
        bool operator!=(const ComponentsIter& x) const {
            return pos != x.pos;
        }
    };
    ComponentsIter begin() const {
        return ComponentsIter(*this, 0);
    }
    ComponentsIter end() const {
        return ComponentsIter(*this, m_str.size());
    }

    path normalise() const;
    //void normalise_in_place();

    friend ::std::ostream& operator<<(::std::ostream& os, const path& p)
    {
        return os << p.m_str;
    }
};

}
