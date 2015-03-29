#ifndef PARSEERROR_HPP_INCLUDED
#define PARSEERROR_HPP_INCLUDED

#include <stdexcept>
#include "lex.hpp"

namespace ParseError {

using CompileError::Generic;
using CompileError::BugCheck;
using CompileError::Todo;

class BadChar:
    public CompileError::Base
{
    char    m_char;
public:
    BadChar(char character);
    virtual ~BadChar() throw ();

};

class Unexpected:
    public CompileError::Base
{
    Token   m_tok;
public:
    Unexpected(const TokenStream& lex, Token tok);
    Unexpected(const TokenStream& lex, Token tok, Token exp);
    virtual ~Unexpected() throw ();

};

#define ASSERT(lex, cnd)    do { if( !(cnd) ) throw CompileError::BugCheck(lex, "Assertion failed: "#cnd); } while(0)

}

#endif // PARSEERROR_HPP_INCLUDED
