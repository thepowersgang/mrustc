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

#include "token.hpp"

namespace AST {
    class Module;
    class MetaItems;
}

/// State the parser needs to pass down via a second channel.
struct ParseState
{
    // Used for "for/if/while" to handle ambiguity
    bool disallow_struct_literal = false;
    // A debugging hook that disables expansion of macros
    bool no_expand_macros = false;
    
    ::AST::Module*  module = nullptr;
    ::AST::MetaItems*   parent_attrs = nullptr;
    
    ::AST::Module& get_current_mod() {
        assert(this->module);
        return *this->module;
    }
    
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

#define SET_MODULE(lex, mod)    SavedParseState _sps(lex, lex.parse_state()); lex.parse_state().module = &(mod)
#define SET_ATTRS(lex, attrs)    SavedParseState _sps(lex, lex.parse_state()); lex.parse_state().parent_attrs = &(attrs)
#define SET_PARSE_FLAG(lex, flag)    SavedParseState _sps(lex, lex.parse_state()); lex.parse_state().flag = true
#define CLEAR_PARSE_FLAG(lex, flag)    SavedParseState _sps(lex, lex.parse_state()); lex.parse_state().flag = false
#define CHECK_PARSE_FLAG(lex, flag) (lex.parse_state().flag == true)

struct Codepoint {
    uint32_t    v;
    Codepoint(): v(0) { }
    Codepoint(uint32_t v): v(v) { }
    bool isspace() const;
    bool isdigit() const;
    bool isxdigit() const;
    bool operator==(char x) { return v == static_cast<uint32_t>(x); }
    bool operator!=(char x) { return v != static_cast<uint32_t>(x); }
    bool operator==(Codepoint x) { return v == x.v; }
    bool operator!=(Codepoint x) { return v != x.v; }
};
extern ::std::string& operator+=(::std::string& s, const Codepoint& cp);
extern ::std::ostream& operator<<(::std::ostream& s, const Codepoint& cp);
typedef Codepoint   uchar;

class Lexer:
    public TokenStream
{
    RcString    m_path;
    unsigned int m_line;
    unsigned int m_line_ofs;

    ::std::ifstream m_istream;
    bool    m_last_char_valid;
    Codepoint   m_last_char;
    Token   m_next_token;   // Used when lexing generated two tokens
public:
    Lexer(const ::std::string& filename);

    virtual Position getPosition() const override;
    virtual Token realGetToken() override;

private:
    Token getTokenInt();
    
    signed int getSymbol();
    Token getTokenInt_RawString(bool is_byte);
    Token getTokenInt_Identifier(Codepoint ch, Codepoint ch2='\0');
    double parseFloat(uint64_t whole);
    uint32_t parseEscape(char enclosing);

    void ungetc();
    Codepoint getc_num();
    Codepoint getc();
    Codepoint getc_cp();
    char getc_byte();

    class EndOfFile {};
};

#endif // LEX_HPP_INCLUDED
