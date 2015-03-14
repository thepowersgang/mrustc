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
    
    friend ::std::ostream& operator<<(::std::ostream& os, const TokenTree& tt) {
        if( tt.m_subtrees.size() == 0 )
            return os << "TokenTree(" << tt.m_tok << ")";
        else
            return os << "TokenTree([" << tt.m_subtrees << "])";
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

    TTStream& operator=(const TTStream& x) { m_stack = x.m_stack; return *this; }
    
    virtual Position getPosition() const override;

protected:
    virtual Token realGetToken() override;
};

// unwrapped = Exclude the enclosing brackets (used by macro parse code)
extern TokenTree Parse_TT(TokenStream& lex, bool unwrapped);
extern TokenTree Parse_TT_Expr(TokenStream& lex);
extern TokenTree Parse_TT_Type(TokenStream& lex);
extern TokenTree Parse_TT_Stmt(TokenStream& lex);
extern TokenTree Parse_TT_Block(TokenStream& lex);
extern TokenTree Parse_TT_Path(TokenStream& lex, bool mode_expr);

#endif // TOKENTREE_HPP_INCLUDED
