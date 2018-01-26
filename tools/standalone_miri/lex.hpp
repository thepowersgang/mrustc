//
//
//
#pragma once
#include <string>
#include <fstream>

enum class TokenClass
{
    Eof,
    Symbol,
    Ident,
    Integer,
    Real,
    String,
};

struct Token
{
    TokenClass  type;
    ::std::string   strval;
    union {
        double  real_val;
        uint64_t    int_val;
    } numbers;

    bool operator==(TokenClass tc) const;
    bool operator!=(TokenClass tc) const { return !(*this == tc); }
    bool operator==(char c) const;
    bool operator!=(char c) const { return !(*this == c); }
    bool operator==(const char* s) const;
    bool operator!=(const char* s) const { return !(*this == s); }

    uint64_t integer() const;
    double real() const;

    friend ::std::ostream& operator<<(::std::ostream& os, const Token& x);
};

class Lexer
{
    ::std::string   m_filename;
    unsigned m_cur_line;
    ::std::ifstream m_if;
    Token   m_cur;
public:
    Lexer(const ::std::string& path);

    const Token& next() const;
    Token consume();
    void check(TokenClass tc);
    void check(char ch);
    void check(const char* s);
    void check_consume(char ch) { check(ch); consume(); }
    void check_consume(const char* s) { check(s); consume(); }
    bool consume_if(char ch) { if(next() == ch) { consume(); return true; } return false; }
    bool consume_if(const char* s) { if(next() == s) { consume(); return true; } return false; }

    friend ::std::ostream& operator<<(::std::ostream& os, const Lexer& x);

private:
    void advance();
};
