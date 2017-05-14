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

const bool DEBUG_PRINT_TOKENS = false;
//const bool DEBUG_PRINT_TOKENS = true;
//#define DEBUG_PRINT_TOKENS  debug_enabled("Lexer Tokens")

TokenStream::TokenStream():
    m_cache_valid(false)
{
}
TokenStream::~TokenStream()
{
}

Token TokenStream::innerGetToken()
{
    Token ret = this->realGetToken();
    if( ret.get_pos().filename == "" )
        ret.set_pos( this->getPosition() );
    //DEBUG("ret.get_pos() = " << ret.get_pos());
    return ret;
}
Token TokenStream::getToken()
{
    if( m_cache_valid )
    {
        //DEBUG("<<< " << m_cache << " (cache)");
        m_cache_valid = false;
        return mv$(m_cache);
    }
    else if( m_lookahead.size() )
    {
        Token ret = mv$( m_lookahead.front().first );
        m_hygiene = m_lookahead.front().second;
        m_lookahead.erase(m_lookahead.begin());
        //DEBUG("<<< " << ret << " (lookahead)");
        if( DEBUG_PRINT_TOKENS ) {
            ::std::cout << "getToken[" << typeid(*this).name() << "] - " << ret.get_pos() << "-" << ret << ::std::endl;
        }
        return ret;
    }
    else
    {
        Token ret = this->innerGetToken();
        m_hygiene = this->realGetHygiene();
        //DEBUG("<<< " << ret << " (new)");
        if( DEBUG_PRINT_TOKENS ) {
            ::std::cout << "getToken[" << typeid(*this).name() << "] - " << ret.get_pos() << "-" << ret << ::std::endl;
        }
        return ret;
    }
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
        //DEBUG(">>> " << tok);
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
        m_lookahead.push_back( ::std::make_pair(mv$(tok), mv$(hygiene)) );
    }

    DEBUG("lookahead(" << i << ") = " << m_lookahead[i]);
    return m_lookahead[i].first.type();
}

Ident::Hygiene TokenStream::getHygiene() const
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
    return Span(
        ps.filename,
        ps.start_line, ps.start_ofs,
        p.line, p.ofs
        );
}
Ident TokenStream::get_ident(Token tok) const
{
    if(tok.type() == TOK_IDENT) {
        return Ident(getHygiene(), tok.str());
    }
    else if( tok.type() == TOK_INTERPOLATED_IDENT ) {
        TODO(getPosition(), "");
    }
    else {
        throw ParseError::Unexpected(*this, tok);
    }
}
