#ifndef PARSEERROR_HPP_INCLUDED
#define PARSEERROR_HPP_INCLUDED

#include <stdexcept>
#include "lex.hpp"

namespace ParseError {

class Base:
    public ::std::exception
{
public:
    ~Base() throw();
};

class Todo:
    public Base
{
    ::std::string   m_message;
public:
    Todo(::std::string message);
    ~Todo() throw ();

};

class BadChar:
    public Base
{
    char    m_char;
public:
    BadChar(char character);
    ~BadChar() throw ();

};

class Unexpected:
    public Base
{
    Token   m_tok;
public:
    Unexpected(Token tok);
    Unexpected(Token tok, Token exp);
    ~Unexpected() throw ();

};

}

#endif // PARSEERROR_HPP_INCLUDED
