#pragma once

#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>

class TomlFileIter;
struct TomlKeyValue;

class TomlFile
{
    /// Input file stream
    ::std::ifstream m_if;

    /// Name of the current `[]` block
    ::std::vector<::std::string>    m_current_block;

    /// Path suffix of the current composite (none if empty)
    ::std::vector<::std::string>    m_current_composite;

    /// Index of the next array field (if zero, not parsing an array)
    unsigned int m_next_array_index;

    /// Next indexes if top-level defined arrays (e.g. `[[foo]]`)
    ::std::unordered_map<::std::string,unsigned>    m_array_counts;

public:
    TomlFile(const ::std::string& filename);

    TomlFileIter begin();
    TomlFileIter end();

    TomlKeyValue get_next_value();
};

struct TomlValue
{
    enum class Type
    {
        Boolean,
        String,
        Integer,
        List,
    };
    struct TypeError:
        public ::std::exception
    {
        Type have;
        Type exp;

        TypeError(Type h, Type e):
            have(h),
            exp(e)
        {
        }

        const char* what() const override {
            return "toml type error";
        }
    };

    Type m_type;
    uint64_t    m_int_value;
    ::std::string   m_str_value;
    ::std::vector<TomlValue>    m_sub_values;

    TomlValue():
        m_type(Type::String)
    {
    }
    TomlValue(::std::string s):
        m_type( Type::String ),
        m_str_value(::std::move(s))
    {
    }
    TomlValue(bool v) :
        m_type(Type::Boolean),
        m_int_value(v ? 1 : 0)
    {
    }

    const ::std::string& as_string() const {
        if( m_type != Type::String ) {
            throw TypeError { m_type, Type::String };
        }
        return m_str_value;
    }
    bool as_bool() const {
        if(m_type != Type::Boolean) {
            throw TypeError { m_type, Type::Boolean };
        }
        return m_int_value != 0;
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const TomlValue& x) {
        switch(x.m_type)
        {
        case Type::Boolean: os << (x.m_int_value != 0 ? "true" : "false");  break;
        case Type::Integer: os << x.m_int_value; break;
        case Type::List:
            os << "[";
            for(auto& e : x.m_sub_values)
                os << e << ",";
            os << "]";
            break;
        case Type::String:
            os << "\"" << x.m_str_value << "\"";
            break;
        }
        return os;
    }
};

struct TomlKeyValue
{
    ::std::vector<::std::string>    path;
    TomlValue   value;
};

class TomlFileIter
{
    friend class TomlFile;
    TomlFile& m_reader;
    TomlKeyValue    m_cur_value;

    TomlFileIter(TomlFile& tf):
        m_reader(tf)
    {

    }

public:
    TomlKeyValue operator*() const
    {
        return m_cur_value;
    }
    void operator++()
    {
        m_cur_value = m_reader.get_next_value();
    }
    bool operator!=(const TomlFileIter& x) const
    {
        return m_cur_value.path != x.m_cur_value.path;
    }
};
