#ifndef LEX_HPP_INCLUDED
#define LEX_HPP_INCLUDED

#include "../types.hpp"
#include <string>
#include <fstream>

enum eTokenType
{
    TOK_NULL,
    TOK_EOF,

    TOK_WHITESPACE,
    TOK_COMMENT,

    // Value tokens
    TOK_IDENT,
    TOK_MACRO,
    TOK_LIFETIME,
    TOK_INTEGER,
    TOK_CHAR,
    TOK_FLOAT,
    TOK_UNDERSCORE,

    TOK_CATTR_OPEN,
    TOK_ATTR_OPEN,

    // Symbols
    TOK_PAREN_OPEN, TOK_PAREN_CLOSE,
    TOK_BRACE_OPEN, TOK_BRACE_CLOSE,
    TOK_LT, TOK_GT,
    TOK_SQUARE_OPEN,TOK_SQUARE_CLOSE,
    TOK_COMMA,
    TOK_SEMICOLON,
    TOK_COLON,
    TOK_DOUBLE_COLON,
    TOK_STAR, TOK_AMP,
    TOK_PIPE,

    TOK_FATARROW,   // =>
    TOK_THINARROW,  // ->

    TOK_PLUS, TOK_DASH,
    TOK_EXLAM,
    TOK_PERCENT,
    TOK_SLASH,

    TOK_DOT,
    TOK_DOUBLE_DOT,
    TOK_TRIPLE_DOT,

    TOK_EQUAL,
    TOK_PLUS_EQUAL,
    TOK_DASH_EQUAL,
    TOK_PERCENT_EQUAL,
    TOK_SLASH_EQUAL,
    TOK_STAR_EQUAL,
    TOK_AMP_EQUAL,
    TOK_PIPE_EQUAL,

    TOK_DOUBLE_EQUAL,
    TOK_EXLAM_EQUAL,
    TOK_GTE,
    TOK_LTE,

    TOK_DOUBLE_AMP,
    TOK_DOUBLE_PIPE,
    TOK_DOUBLE_LT,
    TOK_DOUBLE_GT,

    TOK_QMARK,
    TOK_AT,
    TOK_TILDE,
    TOK_BACKSLASH,
    TOK_CARET,
    TOK_BACKTICK,

    // Reserved Words
    TOK_RWORD_PUB,
    TOK_RWORD_MUT,
    TOK_RWORD_CONST,
    TOK_RWORD_STATIC,
    TOK_RWORD_UNSAFE,

    TOK_RWORD_STRUCT,
    TOK_RWORD_ENUM,
    TOK_RWORD_TRAIT,
    TOK_RWORD_FN,
    TOK_RWORD_USE,

    TOK_RWORD_SELF,
    TOK_RWORD_AS,

    TOK_RWORD_LET,
    TOK_RWORD_MATCH,
    TOK_RWORD_IF,
    TOK_RWORD_ELSE,
    TOK_RWORD_WHILE,
    TOK_RWORD_FOR,

    TOK_RWORD_CONTINUE,
    TOK_RWORD_BREAK,
    TOK_RWORD_RETURN,
};

class Token
{
    enum eTokenType m_type;
    ::std::string   m_str;
    enum eCoreType  m_datatype;
    union {
        uint64_t    m_intval;
        double  m_floatval;
    };
public:
    Token();
    Token(enum eTokenType type);
    Token(enum eTokenType type, ::std::string str);
    Token(uint64_t val, enum eCoreType datatype);
    Token(double val, enum eCoreType datatype);

    enum eTokenType type() { return m_type; }
    const ::std::string& str() { return m_str; }

    static const char* typestr(enum eTokenType type);
};

extern ::std::ostream&  operator<<(::std::ostream& os, Token& tok);

class TokenStream
{
public:
    virtual Token   getToken() = 0;
    virtual void    putback(Token tok) = 0;
};

class Lexer
{
    ::std::ifstream m_istream;
    bool    m_last_char_valid;
    char    m_last_char;
public:
    Lexer(::std::string filename);

    Token getToken();

private:
    signed int getSymbol();
    uint32_t parseEscape(char enclosing);

    char getc();
    void putback();

    class EndOfFile {};
};

#endif // LEX_HPP_INCLUDED
