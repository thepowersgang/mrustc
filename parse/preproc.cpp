#include "preproc.hpp"
#include <iostream>

Preproc::Preproc(::std::string path):
    m_lex(path),
    m_cache_valid(false)
{
    //ctor
}

Preproc::~Preproc()
{
    //dtor
}

Token Preproc::getTokenInt()
{
    while(true)
    {
        Token tok = m_lex.getToken();
        ::std::cout << "getTokenInt: tok = " << tok << ::std::endl;
        switch(tok.type())
        {
        case TOK_WHITESPACE:
            continue;
        case TOK_COMMENT:
            continue;
        default:
            return tok;
        }
    }
}

Token Preproc::getToken()
{
    if( m_cache_valid )
    {
        m_cache_valid = false;
        return m_cache;
    }
    else
    {
        return this->getTokenInt();
    }
}
void Preproc::putback(Token tok)
{
    m_cache_valid = true;
    m_cache = tok;
}
