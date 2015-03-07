#ifndef PREPROC_H
#define PREPROC_H

#include "lex.hpp"

class Preproc:
    public TokenStream
{
    ::std::string   m_path;
    unsigned int m_line;
    Lexer   m_lex;

public:
    Preproc(::std::string path);
    ~Preproc();

    virtual Position getPosition() const override;
    virtual Token realGetToken() override;
private:
    Token getTokenInt();
};

#endif // PREPROC_H
