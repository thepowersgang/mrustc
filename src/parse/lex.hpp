#ifndef LEX_HPP_INCLUDED
#define LEX_HPP_INCLUDED

#include "../types.hpp"
#include <string>
#include <fstream>

enum eTokenType
{
    #define _(t)    t,
    #include "eTokenType.enum.h"
    #undef _
};

struct Position
{
    ::std::string   filename;
    unsigned int    line;
    
    Position():
        filename(""),
        line(0)
    {}
    Position(::std::string filename, unsigned int line):
        filename(filename),
        line(line)
    {
    }
};
extern ::std::ostream& operator<<(::std::ostream& os, const Position& p);

class Token:
    public Serialisable
{
    enum eTokenType m_type;
    ::std::string   m_str;
    enum eCoreType  m_datatype;
    union {
        uint64_t    m_intval;
        double  m_floatval;
    };
    Position    m_pos;
public:
    Token();
    Token(const Token& t) = default;
    Token& operator =(const Token& t) = default;
    Token(Token&& t):
        m_type(t.m_type),
        m_str( ::std::move(t.m_str) ),
        m_datatype( t.m_datatype ),
        m_intval( t.m_intval ),
        m_pos( ::std::move(t.m_pos) )
    {
        t.m_type = TOK_NULL;
    }
    Token(enum eTokenType type);
    Token(enum eTokenType type, ::std::string str);
    Token(uint64_t val, enum eCoreType datatype);
    Token(double val, enum eCoreType datatype);

    enum eTokenType type() const { return m_type; }
    const ::std::string& str() const { return m_str; }
    enum eCoreType  datatype() const { return m_datatype; }
    uint64_t intval() const { return m_intval; }
    double floatval() const { return m_floatval; }

    void set_pos(Position pos) { m_pos = pos; }
    const Position& get_pos() const { return m_pos; }
    
    static const char* typestr(enum eTokenType type);
    static eTokenType typefromstr(const ::std::string& s);
    
    SERIALISABLE_PROTOTYPES();
};

extern ::std::ostream&  operator<<(::std::ostream& os, const Token& tok);

/// State the parser needs to pass down via a second channel.
struct ParseState
{
    bool disallow_struct_literal = false;
    
    friend ::std::ostream& operator<<(::std::ostream& os, const ParseState& ps) {
        os << "ParseState {";
        if(ps.disallow_struct_literal)  os << " disallow_struct_literal";
        os << " }";
        return os;
    }
};

class TokenStream
{
    friend class TTLexer;   // needs access to internals to know what was consumed
    
    bool    m_cache_valid;
    Token   m_cache;
    ::std::vector<Token>    m_lookahead;
    ParseState  m_parse_state;
public:
    TokenStream();
    virtual ~TokenStream();
    Token   getToken();
    void    putback(Token tok);
    eTokenType  lookahead(unsigned int count);
    virtual Position getPosition() const = 0;
    
    ParseState& parse_state() { return m_parse_state; }
    
protected:
    virtual Token   realGetToken() = 0;
private:
    Token innerGetToken();
};

class SavedParseState
{
    TokenStream&    m_lex;
    ParseState  m_state;
public:
    SavedParseState(TokenStream& lex, ParseState state):
        m_lex(lex),
        m_state(state)
    {
    }
    ~SavedParseState()
    {
        DEBUG("Restoring " << m_state);
        m_lex.parse_state() = m_state;
    }
};

#define SET_PARSE_FLAG(lex, flag)    SavedParseState _sps(lex, lex.parse_state()); lex.parse_state().flag = true
#define CLEAR_PARSE_FLAG(lex, flag)    SavedParseState _sps(lex, lex.parse_state()); lex.parse_state().flag = false
#define CHECK_PARSE_FLAG(lex, flag) (lex.parse_state().flag == true)

class Lexer
{
    ::std::ifstream m_istream;
    bool    m_last_char_valid;
    char    m_last_char;
    Token   m_next_token;   // Used when lexing generated two tokens
public:
    Lexer(::std::string filename);

    Token getToken();

private:
    signed int getSymbol();
    double parseFloat(uint64_t whole);
    uint32_t parseEscape(char enclosing);

    char getc();
    char getc_num();
    void putback();

    class EndOfFile {};
};

#endif // LEX_HPP_INCLUDED
