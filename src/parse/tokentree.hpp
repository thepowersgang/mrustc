#ifndef TOKENTREE_HPP_INCLUDED
#define TOKENTREE_HPP_INCLUDED

#include "lex.hpp"

class TokenTree
{
    Token   m_tok;
    ::std::vector<TokenTree>    m_subtrees;
public:
    virtual ~TokenTree() {}
    TokenTree() {}
    TokenTree(TokenTree&&) = default;
    TokenTree& operator=(TokenTree&&) = default;
    TokenTree(Token tok):
        m_tok( ::std::move(tok) )
    {
    }
    TokenTree(::std::vector<TokenTree> subtrees):
        m_subtrees( ::std::move(subtrees) )
    {
    }
    
    TokenTree clone() const;

    bool is_token() const {
        return m_tok.type() != TOK_NULL;
    }
    unsigned int size() const {
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
            return os << tt.m_tok;
        else {
            os << "TT([";
            bool first = true;
            for(const auto& i : tt.m_subtrees) {
                if(!first)
                    os << ", ";
                os << i;
                first = false;
            }
            os << "])";
            return os;
        }
    }
};

class TTStream:
    public TokenStream
{
    ::std::vector< ::std::pair<unsigned int, const TokenTree*> > m_stack;
public:
    TTStream(const TokenTree& input_tt);
    ~TTStream();

    TTStream& operator=(const TTStream& x) { m_stack = x.m_stack; return *this; }
    
    virtual Position getPosition() const override;

protected:
    virtual Token realGetToken() override;
};

class TTStreamO:
    public TokenStream
{
    Position    m_last_pos;
    TokenTree	m_input_tt;
    ::std::vector< ::std::pair<unsigned int, const TokenTree*> > m_stack;
public:
    TTStreamO(TokenTree input_tt);
    TTStreamO(TTStreamO&& x) = default;
    ~TTStreamO();

    TTStreamO& operator=(const TTStreamO& x) { m_stack = x.m_stack; return *this; }
    TTStreamO& operator=(TTStreamO&& x) = default;
    
    virtual Position getPosition() const override;

protected:
    virtual Token realGetToken() override;
};

// unwrapped = Exclude the enclosing brackets (used by macro parse code)
extern TokenTree Parse_TT(TokenStream& lex, bool unwrapped);
extern TokenTree Parse_TT_Pattern(TokenStream& lex);
extern TokenTree Parse_TT_Expr(TokenStream& lex);
extern TokenTree Parse_TT_Type(TokenStream& lex);
extern TokenTree Parse_TT_Stmt(TokenStream& lex);
extern TokenTree Parse_TT_Block(TokenStream& lex);
extern TokenTree Parse_TT_Path(TokenStream& lex, bool mode_expr);
extern TokenTree Parse_TT_Meta(TokenStream& lex);

#endif // TOKENTREE_HPP_INCLUDED
