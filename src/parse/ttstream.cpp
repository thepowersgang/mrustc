/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/ttstream.cpp
 * - Token-Tree backed token streams
 */
#include "ttstream.hpp"
#include <common.hpp>

TTStream::TTStream(Span parent, ParseState ps, const TokenTree& input_tt):
    TokenStream(ps),
    m_parent_span( mv$(parent) )
{
    DEBUG("input_tt = [" << input_tt << "]");
    DEBUG("Set edition " << input_tt.get_edition());
    m_edition = input_tt.get_edition();
    m_stack.push_back( ::std::make_pair(0, &input_tt) );
}
TTStream::~TTStream()
{
}
Token TTStream::realGetToken()
{
    while(m_stack.size() > 0)
    {
        // If current index is above TT size, go up
        unsigned int& idx = m_stack.back().first;
        assert( m_stack.back().second );
        const TokenTree& tree = *m_stack.back().second;

        if(idx == 0 && tree.is_token()) {
            idx ++;
            m_hygiene_ptr = &tree.hygiene();
            DEBUG(tree.tok());
            return tree.tok();
        }

        if(idx < tree.size())
        {
            const TokenTree&    subtree = tree[idx];
            idx ++;
            if( subtree.size() == 0 ) {
                m_hygiene_ptr = &subtree.hygiene();
                DEBUG(subtree.tok());
                return subtree.tok().clone();
            }
            else {
                m_stack.push_back( ::std::make_pair(0, &subtree) );
                DEBUG("Set edition " << m_edition << " -> " << subtree.get_edition());
                m_edition = subtree.get_edition();
            }
        }
        else {
            m_stack.pop_back();
            if(!m_stack.empty()) {
                DEBUG("Restore edition " << m_edition << " -> " << m_stack.back().second->get_edition());
                m_edition = m_stack.back().second->get_edition();
            }
        }
    }
    //m_hygiene = nullptr;
    return Token(TOK_EOF);
}
Position TTStream::getPosition() const
{
    // TODO: Position associated with the previous/next token?
    return Position("TTStream", 0,0);
}
AST::Edition TTStream::realGetEdition() const
{
    return m_edition;
}
Ident::Hygiene TTStream::realGetHygiene() const
{
    // Empty.
    if(!m_hygiene_ptr)
        return Ident::Hygiene();
    return *m_hygiene_ptr;
}


TTStreamO::TTStreamO(Span parent, ParseState ps, TokenTree input_tt):
    TokenStream(ps),
    m_parent_span( mv$(parent) ),
    m_input_tt( mv$(input_tt) )
{
    m_stack.push_back( ::std::make_pair(0, nullptr) );
}
TTStreamO::~TTStreamO()
{
}
Token TTStreamO::realGetToken()
{
    while(m_stack.size() > 0)
    {
        // If current index is above TT size, go up
        unsigned int& idx = m_stack.back().first;
        TokenTree& tree = (m_stack.back().second ? *m_stack.back().second : m_input_tt);

        if(idx == 0 && tree.is_token()) {
            idx ++;
            m_last_pos = tree.tok().get_pos();
            m_edition = tree.get_edition();
            m_hygiene_ptr = &tree.hygiene();
            return mv$(tree.tok());
        }

        if(idx < tree.size())
        {
            TokenTree& subtree = tree[idx];
            idx ++;
            if( subtree.size() == 0 ) {
                m_last_pos = subtree.tok().get_pos();
                m_edition = subtree.get_edition();
                m_hygiene_ptr = &subtree.hygiene();
                return mv$( subtree.tok() );
            }
            else {
                m_stack.push_back( ::std::make_pair(0, &subtree) );
            }
        }
        else {
            m_stack.pop_back();
        }
    }
    return Token(TOK_EOF);
}
AST::Edition TTStreamO::realGetEdition() const
{
    return m_edition;
}
Position TTStreamO::getPosition() const
{
    return m_last_pos;
}
Ident::Hygiene TTStreamO::realGetHygiene() const
{
    // Empty.
    if(!m_hygiene_ptr)
        return Ident::Hygiene();
    return *m_hygiene_ptr;
}
