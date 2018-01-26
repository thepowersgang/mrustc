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
        os << "\"" << x.strval << "\"";
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
Token Lexer::consume()
{
    auto rv = ::std::move(m_cur);

    advance();

    return rv;
}
void Lexer::check(TokenClass tc)
{
    if( next() != tc ) {
        ::std::cerr << *this << "Syntax error: Expected token class #" << int(tc) << " - got '" << next().strval << "'" << ::std::endl;
        throw "ERROR";
    }
}
void Lexer::check(char ch)
{
    if( next() != ch ) {
        ::std::cerr << *this << "Syntax error: Expected '" << ch << "' - got '" << next().strval << "'" << ::std::endl;
        throw "ERROR";
    }
}
void Lexer::check(const char* s)
{
    if( next() != s ) {
        ::std::cerr << *this << "Syntax error: Expected '" << s << "' - got '" << next().strval << "'" << ::std::endl;
        throw "ERROR";
    }
}

void Lexer::advance()
{
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
                    ::std::cerr << *this << "TODO - Hex floats" << ::std::endl;
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
            ::std::cerr << *this << "TODO: Parse floating point numbers" << ::std::endl;
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
        case '&':   m_cur = Token { TokenClass::Symbol, "&" };  break;
        case '*':   m_cur = Token { TokenClass::Symbol, "*" };  break;
        case '/':   m_cur = Token { TokenClass::Symbol, "/" };  break;
        case '-':   m_cur = Token { TokenClass::Symbol, "-" };  break;
        case '+':   m_cur = Token { TokenClass::Symbol, "+" };  break;
        case '^':   m_cur = Token { TokenClass::Symbol, "^" };  break;
        case '|':   m_cur = Token { TokenClass::Symbol, "|" };  break;
        case '!':   m_cur = Token { TokenClass::Symbol, "!" };  break;

        case '@':   m_cur = Token { TokenClass::Symbol, "@" };  break;

        case '(':   m_cur = Token { TokenClass::Symbol, "(" };  break;
        case ')':   m_cur = Token { TokenClass::Symbol, ")" };  break;
        case '<':   m_cur = Token { TokenClass::Symbol, "<" };  break;
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

::std::ostream& operator<<(::std::ostream& os, const Lexer& x)
{
    os << x.m_filename << ":" << x.m_cur_line << ": ";
    return os;
}
