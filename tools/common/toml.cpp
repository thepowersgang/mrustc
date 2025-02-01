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

/// Representation of a syntatic token in a TOML file
struct TomlToken
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

    TomlToken(Type ty):
        m_type(ty)
    {
    }
    TomlToken(Type ty, ::std::string s):
        m_type(ty),
        m_data(s)
    {
    }
    TomlToken(Type ty, int64_t i):
        m_type(ty),
        m_intval(i)
    {
    }


    static TomlToken lex_from(::std::ifstream& is, unsigned& line);
    static TomlToken lex_from_inner(::std::ifstream& is, unsigned& line);

    const ::std::string& as_string() const {
        assert(m_type == Type::Ident || m_type == Type::String);
        return m_data;
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const TomlToken& x) {
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
        while( t.m_type == TomlToken::Type::Newline )
        {
            t = m_lexer.get_token();
        }

        // Expect '[', a string, or an identifier
        switch(t.m_type)
        {
        case TomlToken::Type::Eof:
            // Empty return indicates the end of the list
            return TomlKeyValue {};
        case TomlToken::Type::SquareOpen: {
            m_current_block.clear();

            t = m_lexer.get_token();
            bool is_array = false;
            if(t.m_type == TomlToken::Type::SquareOpen)
            {
                is_array = true;
                t = m_lexer.get_token();
            }
            for(;;)
            {
                if( !(t.m_type == TomlToken::Type::Ident || t.m_type == TomlToken::Type::String) ) {
                    throw ::std::runtime_error(::format(m_lexer, ": Unexpected token in block name - ", t));
                }
                m_current_block.push_back(t.as_string());

                t = m_lexer.get_token();
                if( t.m_type != TomlToken::Type::Dot )
                    break;
                t = m_lexer.get_token();
            }
            if(is_array)
            {
                m_current_block.push_back(::format(m_array_counts[m_current_block.back()]++));
                if( t.m_type != TomlToken::Type::SquareClose ) {
                    throw ::std::runtime_error(::format(m_lexer, ": Unexpected token after array name - ", t));
                }
                t = m_lexer.get_token();
            }
            if( t.m_type != TomlToken::Type::SquareClose )
            {
                throw ::std::runtime_error(::format(m_lexer, ": Unexpected token in block header - ", t));
            }
            t = m_lexer.get_token();
            if (t.m_type != TomlToken::Type::Newline)
            {
                throw ::std::runtime_error(::format(m_lexer, ": Unexpected token after block block - ", t));
            }
            DEBUG("Start block " << m_current_block);
            // Recurse!
            return get_next_value(); }
        default:
            break;
        }
    }
    else
    {
        // Expect a string or an identifier
        if( t.m_type == TomlToken::Type::Eof )
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
        case TomlToken::Type::String:
        case TomlToken::Type::Ident:
            break;
        default:
            throw ::std::runtime_error(::format(m_lexer, ": Unexpected token for key - ", t));
        }
        key_name.push_back(t.as_string());
        t = m_lexer.get_token();
        if(t.m_type == TomlToken::Type::Assign)
        {
            break;
        }

        if(t.m_type != TomlToken::Type::Dot)
            throw ::std::runtime_error(::format(m_lexer, ": Unexpected token after key - ", t));
        t = m_lexer.get_token();
    }

    // Note: Should be impossible, as it's the break condition above
    assert(t.m_type == TomlToken::Type::Assign);
    t = m_lexer.get_token();

    // --- Value ---
    TomlKeyValue    rv;
    switch(t.m_type)
    {
    // String: Return the string value
    case TomlToken::Type::String:
        rv.path = this->get_path(std::move(key_name));
        rv.value = TomlValue { t.m_data };
        break;
    // Array: Parse the entire list and return as Type::List
    case TomlToken::Type::SquareOpen:
        rv.path = this->get_path(std::move(key_name));
        rv.value.m_type = TomlValue::Type::List;
        while( (t = m_lexer.get_token()).m_type != TomlToken::Type::SquareClose )
        {
            while( t.m_type == TomlToken::Type::Newline )
            {
                t = m_lexer.get_token();
            }
            if( t.m_type == TomlToken::Type::SquareClose )
                break;

            // TODO: Recursively parse a value
            // TODO: OR, support other value types
            switch(t.m_type)
            {
            case TomlToken::Type::String:
                rv.value.m_sub_values.push_back(TomlValue { t.as_string() });
                break;
            default:
                throw ::std::runtime_error(::format(m_lexer, ": Unexpected token in array value position - ", t));
            }

            t = m_lexer.get_token();
            if(t.m_type != TomlToken::Type::Comma)
                break;
        }
        while( t.m_type == TomlToken::Type::Newline )
        {
            t = m_lexer.get_token();
        }
        if(t.m_type != TomlToken::Type::SquareClose)
            throw ::std::runtime_error(::format(m_lexer, ": Unexpected token after array - ", t));
        break;
    case TomlToken::Type::BraceOpen:
        m_current_composite.push_back(std::move(key_name));
        DEBUG("Enter composite block " << m_current_block << ", " << m_current_composite);
        // Recurse to restart parse
        return get_next_value();
    case TomlToken::Type::Integer:
        rv.path = this->get_path(std::move(key_name));
        rv.value = TomlValue { t.m_intval };
        break;
    case TomlToken::Type::Ident:
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
    while (!m_current_composite.empty() && t.m_type == TomlToken::Type::BraceClose)
    {
        DEBUG("Leave composite block " << m_current_block << ", " << m_current_composite);
        m_current_composite.pop_back();
        t = m_lexer.get_token();
    }
    if( m_current_composite.empty() )
    {
        if(t.m_type != TomlToken::Type::Newline && t.m_type != TomlToken::Type::Eof)
            throw ::std::runtime_error(::format(m_lexer, ": Unexpected token in TOML file after entry - ", t));
    }
    else
    {
        if( t.m_type != TomlToken::Type::Comma )
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
TomlToken TomlLexer::get_token()
{
    auto rv = TomlToken::lex_from(m_if, m_line);
    if( rv.m_type == TomlToken::Type::Newline )
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

TomlToken TomlToken::lex_from(::std::ifstream& is, unsigned& m_line)
{
    auto rv = TomlToken::lex_from_inner(is, m_line);
    //DEBUG("lex_from: " << rv);
    return rv;
}
namespace {
    void handle_escape(::std::string& str, ::std::ifstream& is) {
        char c = is.get();
        switch(c)
        {
        case '"':  str += '"'; break;
        case '\\': str += '\\'; break;
        case 'n':  str += '\n'; break;
        default:
            throw ::std::runtime_error(format("toml.cpp handle_escape: TODO: Escape sequences in strings - `", c, "`"));
        }
    }
}
TomlToken TomlToken::lex_from_inner(::std::ifstream& is, unsigned& m_line)
{
    int c;
    do
    {
        c = is.get();
    } while( c != EOF && c != '\n' && isspace(c) );

    ::std::string   str;
    switch(c)
    {
    case EOF:   return TomlToken { Type::Eof };
    case '[':   return TomlToken { Type::SquareOpen };
    case ']':   return TomlToken { Type::SquareClose };
    case '{':   return TomlToken { Type::BraceOpen };
    case '}':   return TomlToken { Type::BraceClose };
    case ',':   return TomlToken { Type::Comma };
    case '.':   return TomlToken { Type::Dot };
    case '=':   return TomlToken { Type::Assign };
    case '\n':  return TomlToken { Type::Newline };
    case '#':
        while(c != '\n')
        {
            c = is.get();
            if(c == EOF)
                return TomlToken { Type::Eof };
        }
        return TomlToken { Type::Newline };
    // Literal string: No escaping
    case '\'':
        c = is.get();
        if( c == '\'' ) {
            c = is.get();
            // Empty literal string
            if( c != '\'' ) {
                str = "";
            }
            else {
                // If the first character is a newline, strip it
                c = is.get();
                if( c == '\n' ) {
                    m_line ++;
                    c = is.get();
                }
                // Multi-line literal string
                for( ;; ) {
                    if(c == '\'') {
                        c = is.get();
                        if(c == '\'')
                        {
                            c = is.get();
                            if(c == '\'')
                            {
                                break;
                            }
                            str += '\'';
                        }
                        str += '\'';
                    }
                    if( c == '\n' ) {
                        m_line ++;
                    }
                    if( c == EOF )
                        throw ::std::runtime_error("Unexpected EOF in triple-quoted string");
                    c = is.get();
                }
            }
        }
        else {
            while (c != '\'')
            {
                if (c == EOF)
                    throw ::std::runtime_error("Unexpected EOF in single-quoted string");
                // Technically not allowed
                if( c == '\n' ) {
                    m_line ++;
                }
                str += (char)c;
                c = is.get();
            }
        }
        return TomlToken { Type::String, str };
    // Basic string: has escape sequences
    case '"':
        c = is.get();
        if(c == '"')
        {
            c = is.get();
            if( c != '"' )
            {
                is.putback(c);
                return TomlToken { Type::String, "" };
            }
            else
            {
                // Strip newline if it's the first character
                c = is.get();
                if( c == '\n' ) {
                    m_line ++;
                    c = is.get();
                }
                // Keep reading until """
                for(;;)
                {
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
                    if(c == '\\') {
                        handle_escape(str, is);
                    }
                    else {
                        str += (char)c;
                        if( c == '\n' ) {
                            m_line ++;
                        }
                    }
                    c = is.get();
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
                    handle_escape(str, is);
                    c = is.get();
                    continue ;
                }
                // Technically not allowed
                if( c == '\n' ) {
                    m_line ++;
                }
                str += (char)c;
                c = is.get();
            }
        }
        return TomlToken { Type::String, str };
    default:
        if(isalnum(c) || c == '_')
        {
            // Identifier
            while(isalnum(c) || c == '-' || c == '_')
            {
                str += (char)c;
                c = is.get();
            }
            is.putback(c);

            int64_t val = 0;
            bool is_all_digit = true;
            if( str.size() > 2 && str[0] == '0' ) {
                if(str[1] == 'x') {
                    for(size_t i = 2; i < str.size(); i ++) {
                        c = str[i];
                        if( !isxdigit(c) ) {
                            is_all_digit = false;
                            break;
                        }
                        val *= 16;
                        val += (c <= '9' ? c - '0' : (c & ~0x20) - 'A' + 10);
                    }
                }
                else if(str[1] == 'o') {
                    for(size_t i = 2; i < str.size(); i ++) {
                        c = str[i];
                        if( !('0' <= c && c <= '7') ) {
                            is_all_digit = false;
                            break;
                        }
                        val *= 8;
                        val += c - '0';
                    }
                }
                else if(str[1] == 'b') {
                    for(size_t i = 2; i < str.size(); i ++) {
                        c = str[i];
                        if( !('0' <= c && c <= '1') ) {
                            is_all_digit = false;
                            break;
                        }
                        val *= 2;
                        val += c - '0';
                    }
                }
                else {
                    // Literal `0` is handled below
                }
            }
            else {
                for(char c : str) {
                    if( !isdigit(c) ) {
                        is_all_digit = false;
                        break;
                    }
                    val *= 10;
                    val += c - '0';
                }
            }
            if( is_all_digit ) {
                return TomlToken { Type::Integer, val };
            }
            return TomlToken { Type::Ident, str };
        }
        else
        {
            throw ::std::runtime_error(::format("Unexpected chracter '", (char)c, "' in file"));
        }
    }
}
