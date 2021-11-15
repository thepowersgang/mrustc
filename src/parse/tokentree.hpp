/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/tokentree.hpp
 * - Token Trees (groups of tokens
 */
#ifndef TOKENTREE_HPP_INCLUDED
#define TOKENTREE_HPP_INCLUDED

#include "token.hpp"
#include <ident.hpp>
#include <vector>

namespace  AST {
    enum class Edition;
}

class TokenTree
{
    AST::Edition    m_edition;
    Ident::Hygiene m_hygiene;
    Token   m_tok;
    ::std::vector<TokenTree>    m_subtrees;
public:
    virtual ~TokenTree() {}
    TokenTree() {}
    TokenTree(TokenTree&&) = default;
    TokenTree& operator=(TokenTree&&) = default;
    TokenTree(enum eTokenType ty):
        m_tok( Token(ty) )
    {
    }
    TokenTree(Token tok):
        m_tok( ::std::move(tok) )
    {
    }
    TokenTree(AST::Edition edition, Token tok):
        m_edition(edition),
        m_tok( ::std::move(tok) )
    {
    }
    TokenTree(AST::Edition edition, Ident::Hygiene hygiene, Token tok):
        m_edition(edition),
        m_hygiene( ::std::move(hygiene) ),
        m_tok( ::std::move(tok) )
    {
    }
    TokenTree(AST::Edition edition, Ident::Hygiene hygiene, ::std::vector<TokenTree> subtrees):
        m_edition(edition),
        m_hygiene( ::std::move(hygiene) ),
        m_subtrees( ::std::move(subtrees) )
    {
    }

    TokenTree clone() const;

    bool is_token() const {
        return m_tok.type() != TOK_NULL;
    }
    size_t size() const {
        return m_subtrees.size();
    }
    const TokenTree& operator[](unsigned int idx) const { assert(idx < m_subtrees.size()); return m_subtrees[idx]; }
          TokenTree& operator[](unsigned int idx)       { assert(idx < m_subtrees.size()); return m_subtrees[idx]; }
    const Token& tok() const { return m_tok; }
          Token& tok()       { return m_tok; }
    const Ident::Hygiene& hygiene() const { return m_hygiene; }
    const AST::Edition& get_edition() const { return m_edition; }

    friend ::std::ostream& operator<<(::std::ostream& os, const TokenTree& tt);
};

#endif // TOKENTREE_HPP_INCLUDED
