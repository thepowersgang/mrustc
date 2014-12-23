/*
 */
#include "parseerror.hpp"
#include <iostream>

ParseError::Base::~Base() throw()
{
}

ParseError::Generic::Generic(::std::string message):
    m_message(message)
{
    ::std::cout << "Generic(" << message << ")" << ::std::endl;
}

ParseError::BugCheck::BugCheck(::std::string message):
    m_message(message)
{
    ::std::cout << "BugCheck(" << message << ")" << ::std::endl;
}

ParseError::Todo::Todo(::std::string message):
    m_message(message)
{
    ::std::cout << "Todo(" << message << ")" << ::std::endl;
}
ParseError::Todo::~Todo() throw()
{
}

ParseError::BadChar::BadChar(char character):
    m_char(character)
{
    ::std::cout << "BadChar(" << character << ")" << ::std::endl;
}
ParseError::BadChar::~BadChar() throw()
{
}

ParseError::Unexpected::Unexpected(Token tok):
    m_tok(tok)
{
    ::std::cout << "Unexpected(" << tok << ")" << ::std::endl;
}
ParseError::Unexpected::Unexpected(Token tok, Token exp)
{
    ::std::cout << "Unexpected(" << tok << ", " << exp << ")" << ::std::endl;
}
ParseError::Unexpected::~Unexpected() throw()
{
}
