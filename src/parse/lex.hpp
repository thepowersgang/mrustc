/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/lex.hpp
 * - Lexer header
 */
#ifndef LEX_HPP_INCLUDED
#define LEX_HPP_INCLUDED

#include <debug.hpp>
#include <serialise.hpp>
#include "../coretypes.hpp"
#include <string>
#include <fstream>

#include "../include/span.hpp"

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
    unsigned int    ofs;
    
    Position():
        filename(""),
        line(0),
        ofs(0)
    {}
    Position(::std::string filename, unsigned int line, unsigned int ofs):
        filename(filename),
        line(line),
        ofs(ofs)
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
    bool operator==(const Token& r) const {
        if(type() != r.type())
            return false;
        switch(type())
        {
        case TOK_STRING:
        case TOK_IDENT:
        case TOK_LIFETIME:
            return str() == r.str();
        case TOK_INTEGER:
            return intval() == r.intval() && datatype() == r.datatype();
        case TOK_FLOAT:
            return floatval() == r.floatval() && datatype() == r.datatype();
        default:
            return true;
        }
    }
    bool operator!=(const Token& r) { return !(*this == r); }

    ::std::string to_str() const;
    
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
    // Used for "for/if/while" to handle ambiguity
    bool disallow_struct_literal = false;
    // A debugging hook that disables expansion of macros
    bool no_expand_macros = false;
    
    friend ::std::ostream& operator<<(::std::ostream& os, const ParseState& ps) {
        os << "ParseState {";
        if(ps.disallow_struct_literal)  os << " disallow_struct_literal";
        if(ps.no_expand_macros)  os << " no_expand_macros";
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
    
    ProtoSpan   start_span() const;
    Span    end_span(ProtoSpan ps) const;
    
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

struct Codepoint {
    uint32_t    v;
    Codepoint(uint32_t v): v(v) { }
};
extern ::std::string& operator+=(::std::string& s, const Codepoint& cp);

class Lexer:
    public TokenStream
{
    ::std::string   m_path;
    unsigned int m_line;
    unsigned int m_line_ofs;

    ::std::ifstream m_istream;
    bool    m_last_char_valid;
    char    m_last_char;
    Token   m_next_token;   // Used when lexing generated two tokens
public:
    Lexer(::std::string filename);

    virtual Position getPosition() const override;
    virtual Token realGetToken() override;

private:
    Token getTokenInt();
    
    signed int getSymbol();
    Token getTokenInt_RawString(bool is_byte);
    Token getTokenInt_Identifier(char ch, char ch2='\0');
    double parseFloat(uint64_t whole);
    uint32_t parseEscape(char enclosing);

    char getc();
    char getc_num();
    Codepoint getc_codepoint();
    void ungetc();

    class EndOfFile {};
};

#endif // LEX_HPP_INCLUDED
