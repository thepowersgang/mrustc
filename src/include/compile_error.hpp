/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/compile_error.hpp
 * - Front-end compiler error exception classes
 */
#ifndef _COMPILE_ERROR_H_
#define _COMPILE_ERROR_H_

class TokenStream;

namespace CompileError {

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
    BugCheck(const TokenStream& lex, ::std::string message);
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

}

#endif

