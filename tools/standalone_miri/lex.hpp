/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * lex.hpp
 * - Simple lexer for MIR files (HEADER)
 */
#pragma once
#include <string>
#include <fstream>
#include "../include/int128.h"

enum class TokenClass
{
    Eof,
    Symbol,
    Ident,
    Integer,
    Real,
    String,
    ByteString,
    Lifetime,
};

class Lexer;

struct Token
{
    TokenClass  type;
    ::std::string   strval;
    union Numbers {
        double  real_val;
        U128    int_val;
        Numbers(): int_val(0) {}
    } numbers;

    bool operator==(TokenClass tc) const;
    bool operator!=(TokenClass tc) const { return !(*this == tc); }
    bool operator==(char c) const;
    bool operator!=(char c) const { return !(*this == c); }
    bool operator==(const char* s) const;
    bool operator!=(const char* s) const { return !(*this == s); }

    uint64_t integer_64(const Lexer& l) const;
    U128 integer_128(const Lexer& l) const;
    double real(const Lexer& l) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const Token& x);
};

class Lexer
{
    ::std::string   m_filename;
    unsigned m_cur_line;
    ::std::ifstream m_if;
    Token   m_cur;
    bool    m_next_valid = false;
    Token   m_next;
public:
    Lexer(const ::std::string& path);

    const std::string& filename() const { return m_filename; }

    const Token& next() const;
    const Token& lookahead();
    Token consume();
    void check(TokenClass tc);
    void check(char ch);
    void check(const char* s);
    Token check_consume(TokenClass tc) { check(tc); return consume(); }
    Token check_consume(char ch) { check(ch); return consume(); }
    Token check_consume(const char* s) { check(s); return consume(); }
    bool consume_if(char ch) { if(next() == ch) { consume(); return true; } return false; }
    bool consume_if(const char* s) { if(next() == s) { consume(); return true; } return false; }

    friend ::std::ostream& operator<<(::std::ostream& os, const Lexer& x);

private:
    void advance();

    ::std::string parse_string();
};
