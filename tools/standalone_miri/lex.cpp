/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * lex.cpp
 * - MIR file lexer (very simple)
 */
#include "lex.hpp"
#include <cctype>
#include <sstream>
#include "debug.hpp"
#include <iostream>

bool Token::operator==(TokenClass tc) const
{
    return this->type == tc;
}
bool Token::operator==(char c) const
{
    return (this->type == TokenClass::Ident || this->type == TokenClass::Symbol) && this->strval.size() == 1 && this->strval[0] == c;
}
bool Token::operator==(const char* s) const
{
    return (this->type == TokenClass::Ident || this->type == TokenClass::Symbol) && this->strval == s;
}

uint64_t Token::integer() const
{
    if( this->type != TokenClass::Integer ) {
        ::std::cerr << "?: Syntax error: Expected [integer] - got " << *this << ::std::endl;
        throw "ERROR";
    }
    return this->numbers.int_val;
}
double Token::real() const
{
    if( this->type != TokenClass::Real ) {
        ::std::cerr << "?: Syntax error: Expected [real] - got " << *this << ::std::endl;
        throw "ERROR";
    }
    return this->numbers.real_val;
}

namespace {
    void dump_escaped(::std::ostream& os, const std::string& s) {
        os << std::hex;
        for(auto ch_s : s)
        {
            uint8_t ch = ch_s;
            switch(ch)
            {
            case '\\':  os << "\\\\";   break;
            case '\"':  os << "\\\"";   break;
            case '\'':  os << "\\\'";   break;
            default:
                if(' ' <= ch && ch < 0x7F) {
                    os << ch;
                }
                else if( ch < 16 ) {
                    os << "\\x0" << int(ch);
                }
                else {
                    os << "\\x" << int(ch);
                }
                break;
            }
        }
        os << std::dec;
    }
}
::std::ostream& operator<<(::std::ostream& os, const Token& x)
{
    switch(x.type)
    {
    case TokenClass::Eof:
        os << "-EOF-";
        break;
    case TokenClass::Symbol:
        os << "Symbol(" << x.strval << ")";
        break;
    case TokenClass::Ident:
        os << "Ident(" << x.strval << ")";
        break;
    case TokenClass::Integer:
        os << "Integer(" << x.numbers.int_val << ")";
        break;
    case TokenClass::Real:
        os << "Real(" << x.numbers.real_val << ")";
        break;
    case TokenClass::String:
        os << "\""; dump_escaped(os, x.strval); os << "\"";
        break;
    case TokenClass::ByteString:
        os << "b\"" << x.strval << "\"";
        break;
    case TokenClass::Lifetime:
        os << "'" << x.strval << "\"";
        break;
    }
    return os;
}

Lexer::Lexer(const ::std::string& path):
    m_filename(path),
    m_if(path)
{
    m_cur_line = 1;
    if( !m_if.good() )
    {
        ::std::cerr << "Unable to open file '" << path << "'" << ::std::endl;
        throw "ERROR";
    }

    advance();
}

const Token& Lexer::next() const
{
    return m_cur;
}
const Token& Lexer::lookahead()
{
    if( !m_next_valid )
    {
        auto tmp = ::std::move(m_cur);
        advance();
        m_next = ::std::move(m_cur);
        m_cur = ::std::move(tmp);
        m_next_valid = true;
    }
    return m_next;
}
Token Lexer::consume()
{
    auto rv = ::std::move(m_cur);

    advance();
    //::std::cout << *this << "Lexer::consume " << rv << " -> " << m_cur << ::std::endl;

    return rv;
}
void Lexer::check(TokenClass tc)
{
    if( next() != tc ) {
        ::std::cerr << *this << "Syntax error: Expected token class #" << int(tc) << " - got " << next() << ::std::endl;
        throw "ERROR";
    }
}
void Lexer::check(char ch)
{
    if( next() != ch ) {
        ::std::cerr << *this << "Syntax error: Expected '" << ch << "' - got " << next() << ::std::endl;
        throw "ERROR";
    }
}
void Lexer::check(const char* s)
{
    if( next() != s ) {
        ::std::cerr << *this << "Syntax error: Expected '" << s << "' - got " << next() << ::std::endl;
        throw "ERROR";
    }
}

void Lexer::advance()
{
    if( m_next_valid )
    {
        m_cur = ::std::move(m_next);
        m_next_valid = false;
        return ;
    }

    char ch;
    do
    {
        while( ::std::isblank(ch = m_if.get()) || ch == '\n' || ch == '\r')
        {
            if(ch == '\n')
                m_cur_line ++;
        }
        if( ch == '/' )
        {
            if( m_if.get() == '*' )
            {
                unsigned level = 0;
                while(1)
                {
                    ch = m_if.get();
                    if( ch == '\n' )
                        m_cur_line ++;
                    if( ch == '/' ) {
                        if( m_if.get() == '*' ) {
                            level ++;
                        }
                        else {
                            m_if.unget();
                        }
                    }
                    else if( ch == '*' ) {
                        if( m_if.get() == '/' ) {
                            if( level == 0 ) {
                                break;
                            }
                            level --;
                        }
                        else {
                            m_if.unget();
                        }
                    }
                }

                continue ;
            }
            else {
                m_if.unget();
            }
        }
        break;
    } while(1);
    //::std::cout << "ch=" << ch << ::std::endl;

    // Special hack to treat #0 as an ident
    if( ch == '#' )
    {
        ch = m_if.get();
        if( ::std::isdigit(ch) )
        {
            ::std::string   val = "#";
            while(::std::isdigit(ch))
            {
                val.push_back(ch);
                ch = m_if.get();
            }
            m_if.unget();
            m_cur = Token { TokenClass::Ident, ::std::move(val) };
            return ;
        }

        m_if.unget();
        ch = '#';
    }

    if(ch == 'b')
    {
        ch = m_if.get();
        if( ch == '"' ) {
            auto val = this->parse_string();
            m_cur = Token { TokenClass::ByteString, ::std::move(val) };
            return ;
        }
        else {
            m_if.unget();
        }
        ch = 'b';
    }

    if( m_if.eof() )
    {
        m_cur = Token { TokenClass::Eof, "" };
    }
    else if( ::std::isalpha(ch) || ch == '_' )
    {
        ::std::string   val;
        while(::std::isalnum(ch) || ch == '_' || ch == '#' || ch == '$' )    // Note '#' and '$' is allowed because mrustc them it internally
        {
            val.push_back(ch);
            ch = m_if.get();
        }
        m_if.unget();
        m_cur = Token { TokenClass::Ident, ::std::move(val) };
    }
    else if( ::std::isdigit(ch) )
    {
        if( ch == '0' )
        {
            ch = m_if.get();
            if( ch == 'x' ) {
                ch = m_if.get();
                if( !::std::isxdigit(ch) )
                    throw "ERROR";

                uint64_t    rv = 0;
                while(::std::isxdigit(ch))
                {
                    rv *= 16;
                    if( ch <= '9' )
                        rv += ch - '0';
                    else if( ch <= 'F' )
                        rv += ch - 'A' + 10;
                    else if( ch <= 'f' )
                        rv += ch - 'a' + 10;
                    else
                        throw "";
                    ch = m_if.get();
                }
                if( ch == '.' || ch == 'p' )
                {
                    uint64_t frac = 0;
                    if( ch == '.' )
                    {
                        ch = m_if.get();
                        int pos = 0;
                        while(::std::isxdigit(ch))
                        {
                            frac *= 16;
                            if( ch <= '9' )
                                frac += ch - '0';
                            else if( ch <= 'F' )
                                frac += ch - 'A' + 10;
                            else if( ch <= 'f' )
                                frac += ch - 'a' + 10;
                            else
                                throw "";
                            pos ++;
                            ch = m_if.get();
                        }
                        while(pos < 52/4)
                        {
                            frac *= 16;
                            pos ++;
                        }
                    }
                    int exp = 0;
                    if( ch == 'p' )
                    {
                        ch = m_if.get();
                        bool neg = false;
                        if( ch == '-' ) {
                            neg = true;
                            ch = m_if.get();
                        }
                        if( !::std::isdigit(ch) )
                            throw "ERROR";
                        while(::std::isdigit(ch))
                        {
                            exp *= 10;
                            exp += ch - '0';
                            ch = m_if.get();
                        }
                        if(neg)
                            exp = -exp;
                    }
                    // Floats!
                    //::std::cerr << *this << "TODO - Hex floats - " << rv << "." << frac << "p" << exp << ::std::endl;
                    if( rv != 1 ) {
                        ::std::cerr << *this << "Invalid hex float literal, whole component must be 1" << ::std::endl;
                        throw "ERROR";
                    }
                    if( frac >= (1ull << 52) ) {
                        ::std::cerr << *this << "Invalid hex float literal, fractional component is more than 52 bits" << ::std::endl;
                        throw "ERROR";
                    }
                    union {
                        double  f64;
                        uint64_t    u64;
                    } val;
                    val.u64 = (static_cast<uint64_t>(exp) << 52) | frac;
                    m_cur = Token { TokenClass::Real, "" };
                    m_cur.numbers.real_val = val.f64;
                    return ;
                }
                m_if.unget();

                m_cur = Token { TokenClass::Integer, "" };
                m_cur.numbers.int_val = rv;
                return ;
            }
            else {
                m_if.unget();
                ch = '0';
            }
        }

        uint64_t    rv = 0;
        while(::std::isdigit(ch))
        {
            rv *= 10;
            rv += ch - '0';
            ch = m_if.get();
        }
        if( ch == '.' || ch == 'e' )
        {
            // Floats!
            ::std::cerr << *this << "TODO: Parse floating point numbers" << ::std::endl;
            throw "TODO";
        }
        m_if.unget();

        m_cur = Token { TokenClass::Integer, "" };
        m_cur.numbers.int_val = rv;
    }
    else if( ch == '"' )
    {
        auto val = this->parse_string();
        m_cur = Token { TokenClass::String, ::std::move(val) };
    }
    else if( ch == '\'')
    {
        ::std::string   val;
        ch = m_if.get();
        while( ch == '_' || ::std::isalnum(ch) )
        {
            val += ch;
            ch = m_if.get();
        }
        m_if.unget();
        if( val == "" )
        {
            ::std::cerr << *this << "Empty lifetime name";
            throw "ERROR";
        }
        m_cur = Token { TokenClass::Lifetime, ::std::move(val) };
    }
    else
    {
        switch(ch)
        {
        case ':':
            switch(m_if.get())
            {
            case ':':
                m_cur = Token { TokenClass::Symbol, "::" };
                break;
            default:
                m_if.unget();
                m_cur = Token { TokenClass::Symbol, ":" };
                break;
            }
            break;
        case ';':   m_cur = Token { TokenClass::Symbol, ";" };  break;
        case '.':   m_cur = Token { TokenClass::Symbol, "." };  break;
        case ',':   m_cur = Token { TokenClass::Symbol, "," };  break;
        case '=':   m_cur = Token { TokenClass::Symbol, "=" };  break;
        case '&':   m_cur = Token { TokenClass::Symbol, "&" };  break;
        case '*':   m_cur = Token { TokenClass::Symbol, "*" };  break;
        case '/':   m_cur = Token { TokenClass::Symbol, "/" };  break;
        case '%':   m_cur = Token { TokenClass::Symbol, "%" };  break;
        case '-':   m_cur = Token { TokenClass::Symbol, "-" };  break;
        case '+':   m_cur = Token { TokenClass::Symbol, "+" };  break;
        case '^':   m_cur = Token { TokenClass::Symbol, "^" };  break;
        case '|':   m_cur = Token { TokenClass::Symbol, "|" };  break;
        case '!':   m_cur = Token { TokenClass::Symbol, "!" };  break;

        case '@':   m_cur = Token { TokenClass::Symbol, "@" };  break;

        case '(':   m_cur = Token { TokenClass::Symbol, "(" };  break;
        case ')':   m_cur = Token { TokenClass::Symbol, ")" };  break;
        case '<':
            // Combine << (note, doesn't need to happen for >>)
            ch = m_if.get();
            if( ch == '<' )
            {
                m_cur = Token { TokenClass::Symbol, "<<" };
            }
            else if( ch == '=' )
            {
                m_cur = Token { TokenClass::Symbol, "<=" };
            }
            else
            {
                m_if.unget();
                m_cur = Token { TokenClass::Symbol, "<" };
            }
            break;
        case '>':   m_cur = Token { TokenClass::Symbol, ">" };  break;
        case '[':   m_cur = Token { TokenClass::Symbol, "[" };  break;
        case ']':   m_cur = Token { TokenClass::Symbol, "]" };  break;
        case '{':   m_cur = Token { TokenClass::Symbol, "{" };  break;
        case '}':   m_cur = Token { TokenClass::Symbol, "}" };  break;
        default:
            ::std::cerr << *this << "Unexpected chracter '" << ch << "'" << ::std::endl;
            throw "ERROR";
        }
    }
}
::std::string Lexer::parse_string()
{
    ::std::string   val;
    char ch;
    while( (ch = m_if.get()) != '"' )
    {
        if( ch == '\\' )
        {
            switch( (ch = m_if.get()) )
            {
            case '0':   val.push_back(0); break;
            case 'n':   val.push_back(10); break;
            case 'x': {
                char tmp[3] = { static_cast<char>(m_if.get()), static_cast<char>(m_if.get()), 0};
                val.push_back( static_cast<char>(::std::strtol(tmp, nullptr, 16)) );
                } break;
            case 'u': {
                ch = m_if.get();
                if( ch != '{' ) {
                    ::std::cerr << *this << "Unexpected character in unicode escape - '" << ch << "'" << ::std::endl;
                    throw "ERROR";
                }
                ch = m_if.get();
                uint32_t v = 0;
                do {
                    if( !isxdigit(ch) ) {
                        ::std::cerr << *this << "Unexpected character in unicode escape - '" << ch << "'" << ::std::endl;
                        throw "ERROR";
                    }
                    v *= 16;
                    if( ch <= '9' )
                        v += ch - '0';
                    else if( ch <= 'F' )
                        v += ch - 'A' + 10;
                    else if( ch <= 'f' )
                        v += ch - 'a' + 10;
                    else
                        throw "";
                    ch = m_if.get();
                } while(ch != '}');

                if( v < 0x80 ) {
                    val.push_back(static_cast<char>(v));
                }
                else if( v < (0x1F+1)<<(1*6) ) {
                    val += (char)(0xC0 | ((v >> 6) & 0x1F));
                    val += (char)(0x80 | ((v >> 0) & 0x3F));
                }
                else if( v < (0x0F+1)<<(2*6) ) {
                    val += (char)(0xE0 | ((v >> 12) & 0x0F));
                    val += (char)(0x80 | ((v >>  6) & 0x3F));
                    val += (char)(0x80 | ((v >>  0) & 0x3F));
                }
                else if( v < (0x07+1)<<(3*6) ) {
                    val += (char)(0xF0 | ((v >> 18) & 0x07));
                    val += (char)(0x80 | ((v >> 12) & 0x3F));
                    val += (char)(0x80 | ((v >>  6) & 0x3F));
                    val += (char)(0x80 | ((v >>  0) & 0x3F));
                }
                else {
                    throw "";
                }
                } break;
            case '"':   val.push_back('"'); break;
            case '\\':  val.push_back('\\'); break;
            default:
                ::std::cerr << *this << "Unexpected escape sequence '\\" << ch << "'" << ::std::endl;
                throw "ERROR";
            }
        }
        else
        {
            val.push_back(ch);
        }
    }
    return val;
}

::std::ostream& operator<<(::std::ostream& os, const Lexer& x)
{
    os << x.m_filename << ":" << x.m_cur_line << ": ";
    return os;
}
