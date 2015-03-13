#ifndef PARSEERROR_HPP_INCLUDED
#define PARSEERROR_HPP_INCLUDED

#include <stdexcept>
#include "lex.hpp"

namespace ParseError {

class Base:
    public ::std::exception
{
public:
    virtual ~Base() throw();
};

class Generic:
    public Base
{
    ::std::string   m_message;
public:
    Generic(::std::string message);
    Generic(const TokenStream& lex, ::std::string message);
    virtual ~Generic() throw () {}
};

class BugCheck:
    public Base
{
    ::std::string   m_message;
public:
    BugCheck(::std::string message);
    virtual ~BugCheck() throw () {}
};

class Todo:
    public Base
{
    ::std::string   m_message;
public:
    Todo(::std::string message);
    Todo(const TokenStream& lex, ::std::string message);
    virtual ~Todo() throw ();

};

class BadChar:
    public Base
{
    char    m_char;
public:
    BadChar(char character);
    virtual ~BadChar() throw ();

};

class Unexpected:
    public Base
{
    Token   m_tok;
public:
    Unexpected(const TokenStream& lex, Token tok);
    Unexpected(const TokenStream& lex, Token tok, Token exp);
    virtual ~Unexpected() throw ();

};

}

#endif // PARSEERROR_HPP_INCLUDED
