/*
 * minicargo - MRustC-specific clone of `cargo`
 * - By John Hodge (Mutabah)
 *
 * stringlist.h
 * - Helper classes for storing strings for use as cosnt char**
 *
 * StringList - Vector of strings
 * StringListKV - Key/Value map
 */
#pragma once

#include <vector>
#include <string>

class StringList
{
    ::std::vector<::std::string>    m_cached;
    ::std::vector<const char*>  m_strings;
public:
    StringList()
    {
    }
    StringList(const StringList&) = delete;
    StringList(StringList&&) = default;
    StringList& operator=(StringList&& ) = default;

    const ::std::vector<const char*>& get_vec() const
    {
        return m_strings;
    }

    void push_back(::std::string s)
    {
        // If the cache list is about to move, update the pointers
        if(m_cached.capacity() == m_cached.size())
        {
            // Make a bitmap of entries in `m_strings` that are pointers into `m_cached`
            ::std::vector<bool> b;
            b.reserve(m_strings.size());
            size_t j = 0;
            for(const auto* s : m_strings)
            {
                if(j == m_cached.size())
                    break;
                if(s == m_cached[j].c_str())
                {
                    j ++;
                    b.push_back(true);
                }
                else
                {
                    b.push_back(false);
                }
            }

            // Add the new one
            m_cached.push_back(::std::move(s));
            // Update pointers
            j = 0;
            for(size_t i = 0; i < b.size(); i ++)
            {
                if(b[i])
                {
                    m_strings[i] = m_cached.at(j++).c_str();
                }
            }
        }
        else
        {
            m_cached.push_back(::std::move(s));
        }
        m_strings.push_back(m_cached.back().c_str());
    }
    void push_back(const char* s)
    {
        m_strings.push_back(s);
    }
};
class StringListKV
{
    StringList  m_keys;
    StringList  m_values;
public:
    StringListKV()
    {
    }
    StringListKV(StringListKV&& x) = default;
    StringListKV& operator=(StringListKV&& ) = default;

    void push_back(const char* k, ::std::string v)
    {
        m_keys.push_back(k);
        m_values.push_back(v);
    }
    void push_back(const char* k, const char* v)
    {
        m_keys.push_back(k);
        m_values.push_back(v);
    }
    void push_back(::std::string k, ::std::string v)
    {
        m_keys.push_back(k);
        m_values.push_back(v);
    }
    void push_back(::std::string k, const char* v)
    {
        m_keys.push_back(k);
        m_values.push_back(v);
    }

    struct Iter {
        const StringListKV&   v;
        size_t  i;

        void operator++() {
            this->i++;
        }
        ::std::pair<const char*,const char*> operator*() {
            return ::std::make_pair(this->v.m_keys.get_vec()[this->i], this->v.m_values.get_vec()[this->i]);
        }
        bool operator!=(const Iter& x) const {
            return this->i != x.i;
        }
    };
    Iter begin() const {
        return Iter { *this, 0 };
    }
    Iter end() const {
        return Iter { *this, m_keys.get_vec().size() };
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const StringListKV& x) {
        os << "{ ";
        for(auto kv : x)
            os << kv.first << "=" << kv.second << " ";
        os << "}";
        return os;
    }
};

