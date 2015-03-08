#include "preproc.hpp"
#include <iostream>
#include <algorithm>

Preproc::Preproc(::std::string path):
    m_path(path),
    m_line(1),
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
        case TOK_NEWLINE:
            m_line ++;
            //DEBUG("m_line = " << m_line << " (NL)");
            continue;
        case TOK_WHITESPACE:
            continue;
        case TOK_COMMENT: {
            ::std::string comment = tok.str();
            unsigned int c = ::std::count(comment.begin(), comment.end(), '\n');
            m_line += c;
            //DEBUG("m_line = " << m_line << " (comment w/ "<<c<<")");
            continue; }
        default:
            return tok;
        }
    }
}

Position Preproc::getPosition() const
{
    return Position(m_path, m_line);
}
Token Preproc::realGetToken()
{
    return getTokenInt();
}
