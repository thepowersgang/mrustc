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
        m_cache_valid = false;
        return mv$(m_cache);
    }
    else if( m_lookahead.size() )
    {
        Token ret = mv$( m_lookahead.front() );
        m_lookahead.erase(m_lookahead.begin());
        if( DEBUG_PRINT_TOKENS ) {
            ::std::cout << "getToken[" << typeid(*this).name() << "] - " << ret.get_pos() << "-" << ret << ::std::endl;
        }
        return ret;
    }
    else
    {
        Token ret = this->innerGetToken();
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
        m_lookahead.push_back( this->innerGetToken() );
    }
    
    DEBUG("lookahead(" << i << ") = " << m_lookahead[i]);
    return m_lookahead[i].type();
}

ProtoSpan TokenStream::start_span() const
{
    auto p = this->getPosition();
    return ProtoSpan {
        .filename = p.filename,
        .start_line = p.line,
        .start_ofs = p.ofs,
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
