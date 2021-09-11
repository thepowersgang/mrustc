/*
 * mrustc common tools
 * - by John Hodge (Mutabah)
 *
 * tools/common/toml.h
 * - A very basic (and probably incomplete) streaming TOML parser
 */
#pragma once

#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>

class TomlFileIter;
struct TomlKeyValue;

struct Token;
class TomlLexer
{
    friend class TomlFile;
    /// Input file stream
    ::std::ifstream m_if;

    ::std::string   m_filename;
    unsigned    m_line;
protected:
    TomlLexer(const ::std::string& filename);
    Token   get_token();

public:
    friend ::std::ostream& operator<<(::std::ostream& os, const TomlLexer& x);
};

class TomlFile
{
    /// Input file stream
    TomlLexer   m_lexer;

    /// Name of the current `[]` block
    ::std::vector<::std::string>    m_current_block;

    /// Path suffix of the current composite (none if empty)
    ::std::vector< std::vector<std::string>>    m_current_composite;

    /// Index of the next array field (if zero, not parsing an array)
    unsigned int m_next_array_index;

    /// Next indexes if top-level defined arrays (e.g. `[[foo]]`)
    ::std::unordered_map<::std::string,unsigned>    m_array_counts;

public:
    TomlFile(const ::std::string& filename);

    TomlFileIter begin();
    TomlFileIter end();

    // Obtain the next value in the file
    TomlKeyValue get_next_value();

    const TomlLexer& lexer() const { return m_lexer; }

private:
    std::vector<std::string>    get_path(std::vector<std::string> tail) const;
};

struct TomlValue
{
    enum class Type
    {
        // A true/false, 1/0, yes/no value
        Boolean,
        // A double-quoted string
        String,
        // Integer
        Integer,
        // A list of other values
        List,
    };
    friend ::std::ostream& operator<<(::std::ostream& os, const Type& e) {
        switch(e)
        {
        case Type::Boolean: os << "boolean";    break;
        case Type::String:  os << "string";     break;
        case Type::Integer: os << "integer";    break;
        case Type::List:    os << "list";       break;
        }
        return os;
    }
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

        const char* what() const noexcept override {
            return "toml type error";
        }
        friend ::std::ostream& operator<<(::std::ostream& os, const TypeError& e) {
            os << "expected " << e.exp << ", got " << e.have;
            return os;
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
    TomlValue(int64_t v):
        m_type(Type::Integer),
        m_int_value(v)
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
    uint64_t as_int() const {
        if(m_type != Type::Integer) {
            throw TypeError { m_type, Type::Integer };
        }
        return m_int_value;
    }
    const ::std::vector<TomlValue>& as_list() const {
        if(m_type != Type::List) {
            throw TypeError { m_type, Type::List };
        }
        return m_sub_values;
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
    typedef ::std::vector<::std::string>    Path;
    // Path to the value (last node is the value name)
    // TODO: How are things like `[[bin]]` handled?
    Path    path;
    // Relevant value
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
