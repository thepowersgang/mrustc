//
//
//
#include "lex.hpp"
#include <cctype>
#include <iostream>

bool Token::operator==(TokenClass tc) const
{
    return this->type == tc;
}
bool Token::operator==(char c) const
{
    return this->strval.size() == 1 && this->strval[0] == c;
}
bool Token::operator==(const char* s) const
{
    return this->strval == s;
}

uint64_t Token::integer() const
{
    if( this->type != TokenClass::Integer )
        throw "";
    return this->numbers.int_val;
}
double Token::real() const
{
    if( this->type != TokenClass::Real )
        throw "";
    return this->numbers.real_val;
}

Lexer::Lexer(const ::std::string& path):
    m_if(path)
{
    if( !m_if.good() )
    {
        throw "ERROR";
    }

    advance();
}

const Token& Lexer::next() const
{
    return m_cur;
}
Token Lexer::consume()
{
    auto rv = ::std::move(m_cur);

    advance();

    return rv;
}
void Lexer::check(TokenClass tc)
{
    if( next() != tc ) {
        ::std::cerr << "Syntax error: Expected token class #" << int(tc) << " - got '" << next().strval << "'" << ::std::endl;
        throw "ERROR";
    }
}
void Lexer::check(char ch)
{
    if( next() != ch ) {
        ::std::cerr << "Syntax error: Expected '" << ch << "' - got '" << next().strval << "'" << ::std::endl;
        throw "ERROR";
    }
}
void Lexer::check(const char* s)
{
    if( next() != s ) {
        ::std::cerr << "Syntax error: Expected '" << s << "' - got '" << next().strval << "'" << ::std::endl;
        throw "ERROR";
    }
}

void Lexer::advance()
{
    char ch;
    while( ::std::isblank(ch = m_if.get()) || ch == '\n' || ch == '\r')
        ;
    //::std::cout << "ch=" << ch << ::std::endl;
    if( m_if.eof() )
    {
        m_cur = Token { TokenClass::Eof, "" };
    }
    else if( ::std::isalpha(ch) )
    {
        ::std::string   val;
        while(::std::isalnum(ch) || ch == '_')
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
                while(::std::isdigit(ch))
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
                    // Floats!
                    throw "TODO";
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
            throw "TODO";
        }
        m_if.unget();

        m_cur = Token { TokenClass::Integer, "" };
        m_cur.numbers.int_val = rv;
    }
    else if( ch == '"' )
    {
        ::std::string   val;
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
                    break; }
                case '"':   val.push_back('"'); break;
                default:
                    ::std::cerr << "Unexpected escape sequence '\\" << ch << "'" << ::std::endl;
                    throw "ERROR";
                }
            }
            else
            {
                val.push_back(ch);
            }
        }
        m_cur = Token { TokenClass::String, ::std::move(val) };
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

        case '(':   m_cur = Token { TokenClass::Symbol, "(" };  break;
        case ')':   m_cur = Token { TokenClass::Symbol, ")" };  break;
        case '<':   m_cur = Token { TokenClass::Symbol, "<" };  break;
        case '>':   m_cur = Token { TokenClass::Symbol, ">" };  break;
        case '[':   m_cur = Token { TokenClass::Symbol, "[" };  break;
        case ']':   m_cur = Token { TokenClass::Symbol, "]" };  break;
        case '{':   m_cur = Token { TokenClass::Symbol, "{" };  break;
        case '}':   m_cur = Token { TokenClass::Symbol, "}" };  break;
        default:
            ::std::cerr << "Unexpected chracter '" << ch << "'" << ::std::endl;
            throw "ERROR";
        }
    }
}
