#ifndef PREPROC_H
#define PREPROC_H

#include "lex.hpp"

class Preproc:
    public TokenStream
{
    Lexer   m_lex;

public:
    Preproc(::std::string path);
    ~Preproc();

    virtual Token realGetToken();
private:
    Token getTokenInt();
};

#endif // PREPROC_H
