#ifndef TOKENTREE_HPP_INCLUDED
#define TOKENTREE_HPP_INCLUDED

#include "lex.hpp"


class TokenTree
{
    Token   m_tok;
    ::std::vector<TokenTree>    m_subtrees;
public:
    TokenTree() {}
    TokenTree(Token tok):
        m_tok(tok)
    {
    }
    TokenTree(::std::vector<TokenTree> subtrees):
        m_subtrees(subtrees)
    {
    }

    const unsigned int size() const {
        return m_subtrees.size();
    }
    const TokenTree& operator[](unsigned int idx) const {
        return m_subtrees[idx];
    }
    const Token& tok() const {
        return m_tok;
    }
};

class TTStream:
    public TokenStream
{
    const TokenTree&    m_input_tt;
    ::std::vector< ::std::pair<unsigned int, const TokenTree*> > m_stack;
public:
    TTStream(const TokenTree& input_tt);
    ~TTStream();

    virtual Position getPosition() const override;

protected:
    virtual Token realGetToken() override;
};

extern TokenTree Parse_TT(TokenStream& lex);
extern TokenTree Parse_TT_Expr(TokenStream& lex);
extern TokenTree Parse_TT_Stmt(TokenStream& lex);

#endif // TOKENTREE_HPP_INCLUDED
