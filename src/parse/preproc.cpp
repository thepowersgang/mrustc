#include "preproc.hpp"
#include <iostream>

Preproc::Preproc(::std::string path):
    m_lex(path)
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
        //::std::cout << "getTokenInt: tok = " << tok << ::std::endl;
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

Token Preproc::realGetToken()
{
    return getTokenInt();
}
