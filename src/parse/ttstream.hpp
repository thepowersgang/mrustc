/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/ttstrea.hpp
 * - Token tree streams (for post-lex parsing)
 */
#pragma once

#include "tokentree.hpp"
#include "tokenstream.hpp"

/// Borrowed TTStream
class TTStream:
    public TokenStream
{
    ::std::vector< ::std::pair<unsigned int, const TokenTree*> > m_stack;
    Span m_parent_span;
    AST::Edition m_edition = AST::Edition::Rust2015;
    const Ident::Hygiene*   m_hygiene_ptr = nullptr;
public:
    TTStream(Span parent, ParseState ps, const TokenTree& input_tt);
    ~TTStream();

    TTStream& operator=(const TTStream& x) { m_stack = x.m_stack; return *this; }

    Position getPosition() const override;
    Span outerSpan() const override { return m_parent_span; }

protected:
    AST::Edition realGetEdition() const override;
    Ident::Hygiene realGetHygiene() const override;
    Token realGetToken() override;
};

/// Owned TTStream
class TTStreamO:
    public TokenStream
{
    Span    m_parent_span;
    Position    m_last_pos;
    TokenTree   m_input_tt;
    ::std::vector< ::std::pair<unsigned int, TokenTree*> > m_stack;
    AST::Edition m_edition = AST::Edition::Rust2015;
    const Ident::Hygiene*   m_hygiene_ptr = nullptr;
public:
    TTStreamO(Span parent, ParseState ps, TokenTree input_tt);
    TTStreamO(TTStreamO&& x) = default;
    ~TTStreamO();

    TTStreamO& operator=(const TTStreamO& x) { m_stack = x.m_stack; return *this; }
    TTStreamO& operator=(TTStreamO&& x) = default;

    Position getPosition() const override;
    Span outerSpan() const override { return m_parent_span; }

protected:
    AST::Edition realGetEdition() const override;
    Ident::Hygiene realGetHygiene() const override;
    Token realGetToken() override;
};
