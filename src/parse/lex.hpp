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
    TOK_STRING,

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
    TOK_EXCLAM,
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
    TOK_EXCLAM_EQUAL,
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
    TOK_RWORD_PRIV,
    TOK_RWORD_MUT,
    TOK_RWORD_CONST,
    TOK_RWORD_STATIC,
    TOK_RWORD_UNSAFE,
    TOK_RWORD_EXTERN,

    TOK_RWORD_CRATE,
    TOK_RWORD_MOD,
    TOK_RWORD_STRUCT,
    TOK_RWORD_ENUM,
    TOK_RWORD_TRAIT,
    TOK_RWORD_FN,
    TOK_RWORD_USE,
    TOK_RWORD_IMPL,
    TOK_RWORD_TYPE,

    TOK_RWORD_WHERE,
    TOK_RWORD_AS,

    TOK_RWORD_LET,
    TOK_RWORD_MATCH,
    TOK_RWORD_IF,
    TOK_RWORD_ELSE,
    TOK_RWORD_LOOP,
    TOK_RWORD_WHILE,
    TOK_RWORD_FOR,
    TOK_RWORD_IN,
    TOK_RWORD_DO,

    TOK_RWORD_CONTINUE,
    TOK_RWORD_BREAK,
    TOK_RWORD_RETURN,
    TOK_RWORD_YIELD,
    TOK_RWORD_BOX,
    TOK_RWORD_REF,

    TOK_RWORD_FALSE,
    TOK_RWORD_TRUE,
    TOK_RWORD_SELF,
    TOK_RWORD_SUPER,

    TOK_RWORD_PROC,
    TOK_RWORD_MOVE,
    TOK_RWORD_ONCE,

    TOK_RWORD_ABSTRACT,
    TOK_RWORD_FINAL,
    TOK_RWORD_PURE,
    TOK_RWORD_OVERRIDE,
    TOK_RWORD_VIRTUAL,

    TOK_RWORD_ALIGNOF,
    TOK_RWORD_OFFSETOF,
    TOK_RWORD_SIZEOF,
    TOK_RWORD_TYPEOF,

    TOK_RWORD_BE,
    TOK_RWORD_UNSIZED,
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

    enum eTokenType type() const { return m_type; }
    const ::std::string& str() const { return m_str; }
    enum eCoreType  datatype() const { return m_datatype; }
    uint64_t intval() const { return m_intval; }
    double floatval() const { return m_floatval; }

    static const char* typestr(enum eTokenType type);
};

extern ::std::ostream&  operator<<(::std::ostream& os, Token& tok);

class TokenStream
{
    bool    m_cache_valid;
    Token   m_cache;
public:
    TokenStream();
    virtual ~TokenStream();
    Token   getToken();
    void    putback(Token tok);
protected:
    virtual Token   realGetToken() = 0;
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
