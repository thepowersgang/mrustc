#pragma once

#include <string>

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

/// Path helper class (because I don't want to include boost)
class path
{
#ifdef _WIN32
    static const char SEP = '\\';
#else
    static const char SEP = '/';
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
        void operator++() {
            if(end == p.m_str.size())
                pos = end;
            else
            {
                pos = end+1;
                end = p.m_str.find(SEP, pos);
                if(end == ::std::string::npos)
                    end = p.m_str.size();
            }
        }
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

    path normalise() const
    {
        path rv;
        rv.m_str.reserve( m_str.size()+1 );

        for(auto comp : *this)
        {
            if( comp == "." ) {
                // Ignore.
            }
            else if( comp == ".." )
            {
                // If the path is empty, OR the last element is a "..", push the element
                if( rv.m_str.empty()
                 || (rv.m_str.size() == 3 && rv.m_str[0] == '.' && rv.m_str[1] == '.' && rv.m_str[2] == SEP)
                 || (rv.m_str.size() > 4 && *(rv.m_str.end()-4) == SEP && *(rv.m_str.end()-3) == '.' && *(rv.m_str.end()-2) == '.' && *(rv.m_str.end()-1) == SEP )
                    )
                {
                    // Push
                    rv.m_str += comp;
                    rv.m_str += SEP;
                }
                else
                {
                    rv.m_str.pop_back();
                    auto pos = rv.m_str.find_last_of(SEP);
                    if(pos == ::std::string::npos)
                    {
                        rv.m_str.resize(0);
                    }
                    else if( pos == 0 )
                    {
                        // Keep.
                    }
                    else
                    {
                        rv.m_str.resize(pos+1);
                    }
                }
            }
            else {
                rv.m_str += comp;
                rv.m_str += SEP;
            }
        }
        rv.m_str.pop_back();
        return rv;
    }
#if 0
    void normalise_in_place()
    {
        size_t insert_point = 0;

        for(size_t read_pos = 0; read_pos < m_str.size(); read_pos ++)
        {
            auto pos = m_str.find_first_of(SEP, read_pos);
            if(pos == ::std::string::npos)
                pos = m_str.size();
            auto comp = string_view(m_str.c_str() + read_pos, pos - read_pos);

            bool append;
            if(comp == ".")
            {
                // Advance read without touching insert
                append = false;
            }
            else if( comp == ".." )
            {
                // Consume parent (if not a relative component already)
                // Move insertion point back to the previous separator
                auto pos = m_str.find_last_of(SEP, insert_point);
                if(pos == ::std::string::npos)
                {
                    // Only one component currently (or empty)
                    append = true;
                }
                else if(string_view(m_str.c_str() + pos+1, insert_point - pos-1) == "..")
                {
                    // Last component is ".." - keep adding
                    append = true;
                }
                else
                {
                    insert_point = pos;
                    append = false;
                }
            }
            else
            {
                append = true;
            }

            if(append)
            {
                if( read_pos != insert_point )
                {
                    //assert(read_pos > insert_point);
                    while(read_pos < pos)
                    {
                        m_str[insert_point++] = m_str[read_pos++];
                    }
                }
            }
            else
            {
                read_pos = pos;
            }
        }
    }
#endif

    friend ::std::ostream& operator<<(::std::ostream& os, const path& p)
    {
        return os << p.m_str;
    }
};

} // namespace helpers