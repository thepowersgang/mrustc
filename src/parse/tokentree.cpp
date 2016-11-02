/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/tokentree.cpp
 * - Token Tree (collection of tokens)
 */
#include "tokentree.hpp"
#include <common.hpp>

TokenTree TokenTree::clone() const
{
    if( m_subtrees.size() == 0 ) {
        return TokenTree(m_hygiene, m_tok.clone());
    }
    else {
        ::std::vector< TokenTree>   ents;
        ents.reserve( m_subtrees.size() );
        for(const auto& sub : m_subtrees)
            ents.push_back( sub.clone() );
        return TokenTree( m_hygiene, mv$(ents) );
    }
}

::std::ostream& operator<<(::std::ostream& os, const TokenTree& tt)
{
    os << tt.m_hygiene;
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
