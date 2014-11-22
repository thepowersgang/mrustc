#ifndef PREPROC_H
#define PREPROC_H

#include "lex.hpp"

class Preproc:
    public TokenStream
{
    Lexer   m_lex;

    bool    m_cache_valid;
    Token   m_cache;

public:
    Preproc(::std::string path);
    ~Preproc();

    virtual Token getToken();
    virtual void putback(Token tok);
private:
    Token getTokenInt();
};

#endif // PREPROC_H
