/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/parseerror.cpp
 * - Exceptions thrown for different types of parsing errors
 */
#include "parseerror.hpp"
#include <iostream>

CompileError::Base::~Base() throw()
{
}

CompileError::Generic::Generic(::std::string message):
    m_message(message)
{
    ::std::cout << "Generic(" << message << ")" << ::std::endl;
}
CompileError::Generic::Generic(const TokenStream& lex, ::std::string message)
{
    ::std::cout << lex.getPosition() << ": Generic(" << message << ")" << ::std::endl;
}

CompileError::BugCheck::BugCheck(const TokenStream& lex, ::std::string message):
    m_message(message)
{
    ::std::cout << lex.getPosition() << "BugCheck(" << message << ")" << ::std::endl;
}
CompileError::BugCheck::BugCheck(::std::string message):
    m_message(message)
{
    ::std::cout << "BugCheck(" << message << ")" << ::std::endl;
}

CompileError::Todo::Todo(::std::string message):
    m_message(message)
{
    ::std::cout << "Todo(" << message << ")" << ::std::endl;
}
CompileError::Todo::Todo(const TokenStream& lex, ::std::string message):
    m_message(message)
{
    ::std::cout << lex.getPosition() << ": Todo(" << message << ")" << ::std::endl;
}
CompileError::Todo::~Todo() throw()
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

ParseError::Unexpected::Unexpected(const TokenStream& lex, Token tok):
    m_tok(tok)
{
    auto pos = tok.get_pos();
    if(pos.filename == "")
        pos = lex.getPosition();
    ::std::cout << pos << ": Unexpected(" << tok << ")" << ::std::endl;
}
ParseError::Unexpected::Unexpected(const TokenStream& lex, Token tok, Token exp):
    m_tok(tok)
{
    auto pos = tok.get_pos();
    if(pos.filename == "")
        pos = lex.getPosition();
    ::std::cout << pos << ": Unexpected(" << tok << ", " << exp << ")" << ::std::endl;
}
ParseError::Unexpected::Unexpected(const TokenStream& lex, Token tok, ::std::vector<eTokenType> exp)
{
    auto pos = tok.get_pos();
    if(pos.filename == "")
        pos = lex.getPosition();
    ::std::cout << pos << ": Unexpected " << tok << ", expected ";
    ::std::cout << exp << ")" << ::std::endl;
}
ParseError::Unexpected::~Unexpected() throw()
{
}
