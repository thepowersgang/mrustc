/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/tokenstream.cpp
 * - TokenStream - Parser token source interface
 */
#include "tokenstream.hpp"
#include <common.hpp>
#include "parseerror.hpp"
#include <ast/crate.hpp>    // Edition lookup

const bool DEBUG_PRINT_TOKENS = false;
//const bool DEBUG_PRINT_TOKENS = true;
//#define DEBUG_PRINT_TOKENS  debug_enabled("Lexer Tokens")
#define FULL_TRACE

TokenStream::TokenStream(ParseState ps):
    m_cache_valid(false),
    m_parse_state(ps)
{
}
TokenStream::~TokenStream()
{
}

Token TokenStream::innerGetToken()
{
    Token ret = this->realGetToken();
    if( ret != TOK_EOF && ret.get_pos().filename == "" )
        ret.set_pos( this->getPosition() );
    //DEBUG("ret.get_pos() = " << ret.get_pos());
    return ret;
}
Token TokenStream::getToken()
{
    if( m_cache_valid )
    {
#ifdef FULL_TRACE
        DEBUG("<= " << m_cache << " (cache)");
#endif
        m_cache_valid = false;
        return mv$(m_cache);
    }
    else if( m_lookahead.size() )
    {
        Token ret = mv$( m_lookahead.front().tok );
        m_edition = m_lookahead.front().edition;
        m_hygiene = m_lookahead.front().hygiene;
        m_lookahead.erase(m_lookahead.begin());
#ifdef FULL_TRACE
        DEBUG("<= " << ret << " (lookahead)");
#endif
        if( DEBUG_PRINT_TOKENS ) {
            ::std::cout << "getToken[" << typeid(*this).name() << "] - " << ret.get_pos() << "-" << ret << ::std::endl;
        }
        return ret;
    }
    else
    {
        Token ret = this->innerGetToken();
        m_edition = this->realGetEdition();
        m_hygiene = this->realGetHygiene();
#ifdef FULL_TRACE
        DEBUG("<= " << ret << " (new)");
#endif
        if( DEBUG_PRINT_TOKENS ) {
            ::std::cout << "getToken[" << typeid(*this).name() << "] - " << ret.get_pos() << "-" << ret << ::std::endl;
        }
        return ret;
    }
}
Token TokenStream::getTokenCheck(eTokenType exp)
{
    auto tok = getToken();
    if(tok.type() != exp)
        throw ParseError::Unexpected(*this, tok, Token(exp));
    return tok;
}
void TokenStream::putback(Token tok)
{
    if( m_cache_valid )
    {
        DEBUG("" << getPosition() << " - Double putback: " << tok << " but " << m_cache);
        throw ParseError::BugCheck("Double putback");
    }
    else
    {
#ifdef FULL_TRACE
        DEBUG(">>> " << tok);
#endif
        m_cache_valid = true;
        m_cache = mv$(tok);
    }
}

eTokenType TokenStream::lookahead(unsigned int i)
{
    const unsigned int MAX_LOOKAHEAD = 3;

    if( m_cache_valid )
    {
        if( i == 0 )
            return m_cache.type();
        i --;
    }

    if( i >= MAX_LOOKAHEAD )
        throw ParseError::BugCheck("Excessive lookahead");

    while( i >= m_lookahead.size() )
    {
        DEBUG("lookahead - read #" << m_lookahead.size());
        auto tok = this->innerGetToken();
        auto hygiene = this->realGetHygiene();
        m_lookahead.push_back({ mv$(tok), this->realGetEdition(), mv$(hygiene) });
    }

    DEBUG("lookahead(" << i << ") = " << m_lookahead[i].tok);
    return m_lookahead[i].tok.type();
}

Ident::Hygiene TokenStream::get_hygiene() const
{
    return m_hygiene;
}

ProtoSpan TokenStream::start_span() const
{
    auto p = this->getPosition();
    return ProtoSpan {
        p.filename,
        p.line, p.ofs
        };
}
Span TokenStream::end_span(ProtoSpan ps) const
{
    auto p = this->getPosition();
    return Span( this->outerSpan(), ::std::move(ps.filename),  ps.start_line, ps.start_ofs,  p.line, p.ofs );
}
Span TokenStream::point_span() const
{
    return Span( this->outerSpan(), this->getPosition() );
}
