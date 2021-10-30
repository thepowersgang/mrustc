/*
 * mrustc common tools
 * - by John Hodge (Mutabah)
 *
 * tools/common/toml.cpp
 * - A very basic (and probably incomplete) streaming TOML parser
 */
#define NOLOG   // Disable logging
#include "toml.h"
#include "debug.h"
#include <cassert>
#include <string>

// Representation of a syntatic token in a TOML file
struct Token
{
    enum class Type
    {
        Eof,
        SquareOpen,
        SquareClose,
        BraceOpen,
        BraceClose,
        Assign,
        Newline,
        Comma,
        Dot,

        Ident,
        String,
        Integer,
    };

    Type    m_type;
    ::std::string   m_data;
    int64_t m_intval = 0;

    Token(Type ty):
        m_type(ty)
    {
    }
    Token(Type ty, ::std::string s):
        m_type(ty),
        m_data(s)
    {
    }
    Token(Type ty, int64_t i):
        m_type(ty),
        m_intval(i)
    {
    }


    static Token lex_from(::std::ifstream& is);
    static Token lex_from_inner(::std::ifstream& is);

    const ::std::string& as_string() const {
        assert(m_type == Type::Ident || m_type == Type::String);
        return m_data;
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const Token& x) {
        switch(x.m_type)
        {
        case Type::Eof:   os << "Eof";    break;
        case Type::SquareOpen:  os << "SquareOpen"; break;
        case Type::SquareClose: os << "SquareClose"; break;
        case Type::BraceOpen:   os << "BraceOpen"; break;
        case Type::BraceClose:  os << "BraceClose"; break;
        case Type::Assign:      os << "Assign";   break;
        case Type::Newline:     os << "Newline";  break;
        case Type::Comma:       os << "Comma";    break;
        case Type::Dot:         os << "Dot";      break;
        case Type::Ident:  os << "Ident(" << x.m_data << ")";  break;
        case Type::String: os << "String(" << x.m_data << ")"; break;
        case Type::Integer: os << "Integer(" << x.m_intval << ")"; break;
        }
        return os;
    }
};

TomlFile::TomlFile(const ::std::string& filename):
    m_lexer(filename)
{
}
TomlFileIter TomlFile::begin()
{
    TomlFileIter rv { *this };
    ++rv;
    return rv;
}
TomlFileIter TomlFile::end()
{
    return TomlFileIter { *this };
}

TomlKeyValue TomlFile::get_next_value()
{
    auto t = m_lexer.get_token();

    if(m_current_composite.empty())
    {
        while( t.m_type == Token::Type::Newline )
        {
            t = m_lexer.get_token();
        }

        // Expect '[', a string, or an identifier
        switch(t.m_type)
        {
        case Token::Type::Eof:
            // Empty return indicates the end of the list
            return TomlKeyValue {};
        case Token::Type::SquareOpen:
            m_current_block.clear();
            do
            {
                t = m_lexer.get_token();
                bool is_array = false;
                if(t.m_type == Token::Type::SquareOpen)
                {
                    is_array = true;
                    t = m_lexer.get_token();
                }
                assert(t.m_type == Token::Type::Ident || t.m_type == Token::Type::String);
                m_current_block.push_back(t.as_string());
                if(is_array)
                {
                    m_current_block.push_back(::format(m_array_counts[t.as_string()]++));
                    t = m_lexer.get_token();
                    assert(t.m_type == Token::Type::SquareClose);
                }

                t = m_lexer.get_token();
            } while(t.m_type == Token::Type::Dot);
            if( t.m_type != Token::Type::SquareClose )
            {
                throw ::std::runtime_error(::format(m_lexer, ": Unexpected token in block header - ", t));
            }
            t = m_lexer.get_token();
            if (t.m_type != Token::Type::Newline)
            {
                throw ::std::runtime_error(::format(m_lexer, ": Unexpected token after block block - ", t));
            }
            DEBUG("Start block " << m_current_block);
            // Recurse!
            return get_next_value();
        default:
            break;
        }
    }
    else
    {
        // Expect a string or an identifier
        if( t.m_type == Token::Type::Eof )
        {
            // EOF isn't allowed here
            throw ::std::runtime_error(::format(m_lexer, ": Unexpected EOF in composite"));
        }
    }
    std::vector<std::string>    key_name;
    for(;;)
    {
        switch (t.m_type)
        {
        case Token::Type::String:
        case Token::Type::Ident:
            break;
        default:
            throw ::std::runtime_error(::format(m_lexer, ": Unexpected token for key - ", t));
        }
        key_name.push_back(t.as_string());
        t = m_lexer.get_token();
        if(t.m_type == Token::Type::Assign)
        {
            break;
        }

        if(t.m_type != Token::Type::Dot)
            throw ::std::runtime_error(::format(m_lexer, ": Unexpected token after key - ", t));
        t = m_lexer.get_token();
    }

    assert(t.m_type == Token::Type::Assign);
    t = m_lexer.get_token();

    // --- Value ---
    TomlKeyValue    rv;
    switch(t.m_type)
    {
    // String: Return the string value
    case Token::Type::String:
        rv.path = this->get_path(std::move(key_name));
        rv.value = TomlValue { t.m_data };
        break;
    // Array: Parse the entire list and return as Type::List
    case Token::Type::SquareOpen:
        rv.path = this->get_path(std::move(key_name));
        rv.value.m_type = TomlValue::Type::List;
        while( (t = m_lexer.get_token()).m_type != Token::Type::SquareClose )
        {
            while( t.m_type == Token::Type::Newline )
            {
                t = m_lexer.get_token();
            }
            if( t.m_type == Token::Type::SquareClose )
                break;

            // TODO: Recursively parse a value
            // TODO: OR, support other value types
            switch(t.m_type)
            {
            case Token::Type::String:
                rv.value.m_sub_values.push_back(TomlValue { t.as_string() });
                break;
            default:
                throw ::std::runtime_error(::format(m_lexer, ": Unexpected token in array value position - ", t));
            }

            t = m_lexer.get_token();
            if(t.m_type != Token::Type::Comma)
                break;
        }
        while( t.m_type == Token::Type::Newline )
        {
            t = m_lexer.get_token();
        }
        if(t.m_type != Token::Type::SquareClose)
            throw ::std::runtime_error(::format(m_lexer, ": Unexpected token after array - ", t));
        break;
    case Token::Type::BraceOpen:
        m_current_composite.push_back(std::move(key_name));
        DEBUG("Enter composite block " << m_current_block << ", " << m_current_composite);
        // Recurse to restart parse
        return get_next_value();
    case Token::Type::Integer:
        rv.path = this->get_path(std::move(key_name));
        rv.value = TomlValue { t.m_intval };
        break;
    case Token::Type::Ident:
        if( t.m_data == "true" )
        {
            rv.path = this->get_path(std::move(key_name));
            rv.value = TomlValue { true };
        }
        else if( t.m_data == "false" )
        {
            rv.path = this->get_path(std::move(key_name));

            rv.value = TomlValue { false };
        }
        else
        {
            throw ::std::runtime_error(::format(m_lexer, ": Unexpected identifier in value position - ", t));
        }
        break;
    default:
        throw ::std::runtime_error(::format(m_lexer, ": Unexpected token in value position - ", t));
    }

    t = m_lexer.get_token();
    while (!m_current_composite.empty() && t.m_type == Token::Type::BraceClose)
    {
        DEBUG("Leave composite block " << m_current_block << ", " << m_current_composite);
        m_current_composite.pop_back();
        t = m_lexer.get_token();
    }
    if( m_current_composite.empty() )
    {
        if(t.m_type != Token::Type::Newline && t.m_type != Token::Type::Eof)
            throw ::std::runtime_error(::format(m_lexer, ": Unexpected token in TOML file after entry - ", t));
    }
    else
    {
        if( t.m_type != Token::Type::Comma )
            throw ::std::runtime_error(::format(m_lexer, ": Unexpected token in TOML file after composite entry - ", t));
    }
    return rv;
}

std::vector<std::string> TomlFile::get_path(std::vector<std::string> tail) const
{
    std::vector<std::string>    path;
    path = m_current_block;
    for(const auto& composite_ent : m_current_composite)
        path.insert(path.end(), composite_ent.begin(), composite_ent.end());
    path.insert(path.end(), std::make_move_iterator(tail.begin()), std::make_move_iterator(tail.end()));
    return path;
}

TomlLexer::TomlLexer(const ::std::string& filename)
    :m_if(filename)
    ,m_filename(filename)
    ,m_line(1)
{
    if( !m_if.is_open() ) {
        throw ::std::runtime_error("Unable to open file '" + filename + "'");
    }
}
Token TomlLexer::get_token()
{
    auto rv = Token::lex_from(m_if);
    if( rv.m_type == Token::Type::Newline )
    {
        m_line ++;
    }
    return rv;
}
::std::ostream& operator<<(::std::ostream& os, const TomlLexer& x)
{
    os << x.m_filename << ":" << x.m_line;
    return os;
}

Token Token::lex_from(::std::ifstream& is)
{
    auto rv = Token::lex_from_inner(is);
    //DEBUG("lex_from: " << rv);
    return rv;
}
Token Token::lex_from_inner(::std::ifstream& is)
{
    int c;
    do
    {
        c = is.get();
    } while( c != EOF && c != '\n' && isspace(c) );

    ::std::string   str;
    switch(c)
    {
    case EOF:   return Token { Type::Eof };
    case '[':   return Token { Type::SquareOpen };
    case ']':   return Token { Type::SquareClose };
    case '{':   return Token { Type::BraceOpen };
    case '}':   return Token { Type::BraceClose };
    case ',':   return Token { Type::Comma };
    case '.':   return Token { Type::Dot };
    case '=':   return Token { Type::Assign };
    case '\n':  return Token { Type::Newline };
    case '#':
        while(c != '\n')
        {
            c = is.get();
            if(c == EOF)
                return Token { Type::Eof };
        }
        return Token { Type::Newline };
    case '\'':
        c = is.get();
        while (c != '\'')
        {
            if (c == EOF)
                throw ::std::runtime_error("Unexpected EOF in single-quoted string");
            if (c == '\\')
            {
                // TODO: Escaped strings
                throw ::std::runtime_error("TODO: Escaped sequences in strings (single)");
            }
            str += (char)c;
            c = is.get();
        }
        return Token { Type::String, str };
    case '"':
        c = is.get();
        if(c == '"')
        {
            c = is.get();
            if( c != '"' )
            {
                is.putback(c);
                return Token { Type::String, "" };
            }
            else
            {
                // Keep reading until """
                for(;;)
                {
                    c = is.get();
                    if(c == '"')
                    {
                        c = is.get();
                        if(c == '"')
                        {
                            c = is.get();
                            if(c == '"')
                            {
                                break;
                            }
                            str += '"';
                        }
                        str += '"';
                    }
                    if( c == EOF )
                        throw ::std::runtime_error("Unexpected EOF in triple-quoted string");
                    if(c == '\\')
                    {
                        // TODO: Escaped strings
                        throw ::std::runtime_error("TODO: Escaped sequences in strings (triple)");
                    }
                    str += (char)c;
                }
            }
        }
        else
        {
            while(c != '"')
            {
                if (c == EOF)
                    throw ::std::runtime_error("Unexpected EOF in double-quoted string");
                if (c == '\\')
                {
                    // TODO: Escaped strings
                    c = is.get();
                    switch(c)
                    {
                    case '"':  str += '"'; break;
                    case 'n':  str += '\n'; break;
                    default:
                        throw ::std::runtime_error("TODO: Escaped sequences in strings");
                    }
                    c = is.get();
                    continue ;
                }
                str += (char)c;
                c = is.get();
            }
        }
        return Token { Type::String, str };
    default:
        if(isalpha(c) || c == '_')
        {
            // Identifier
            while(isalnum(c) || c == '-' || c == '_')
            {
                str += (char)c;
                c = is.get();
            }
            is.putback(c);
            return Token { Type::Ident, str };
        }
        else if( c == '0' )
        {
            c = is.get();
            if( c == 'x' )
            {
                c = is.get();
                int64_t val = 0;
                while(isxdigit(c))
                {
                    val *= 16;
                    val += (c <= '9' ? c - '0' : (c & ~0x20) - 'A' + 10);
                    c = is.get();
                }
                is.putback(c);
                return Token { Type::Integer, val };
            }
            else if( c == 'o' )
            {
                c = is.get();
                int64_t val = 0;
                while(isdigit(c))
                {
                    val *= 8;
                    val += c - '0';
                    c = is.get();
                }
                is.putback(c);
                return Token { Type::Integer, val };
            }
            else if( c == 'b' )
            {
                c = is.get();
                int64_t val = 0;
                while(isdigit(c))
                {
                    val *= 2;
                    val += c - '0';
                    c = is.get();
                }
                is.putback(c);
                return Token { Type::Integer, val };
            }
            else
            {
                is.putback(c);
                return Token { Type::Integer, 0 };
            }
        }
        else if( isdigit(c) )
        {
            int64_t val = 0;
            while(isdigit(c))
            {
                val *= 10;
                val += c - '0';
                c = is.get();
            }
            is.putback(c);
            return Token { Type::Integer, val };
        }
        else
        {
            throw ::std::runtime_error(::format("Unexpected chracter '", (char)c, "' in file"));
        }
    }
}
