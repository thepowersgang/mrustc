/*
 */
#pragma once

#include <cassert>

class TypeRef;
class TokenTree;
namespace AST {
    class Pattern;
    class Path;
    class ExprNode;
    class MetaItem;
};

class InterpolatedFragment
{
public:
    enum Type
    {
        TT,
        PAT,
        PATH,
        TYPE,
        
        EXPR,
        STMT,
        BLOCK,
        
        META,
    } m_type;
    
    void*   m_ptr;
    
    InterpolatedFragment(InterpolatedFragment&& );
    InterpolatedFragment& operator=(InterpolatedFragment&& );
    //InterpolatedFragment(const InterpolatedFragment& );
    InterpolatedFragment(TokenTree );
    InterpolatedFragment(::AST::Pattern);
    InterpolatedFragment(::AST::Path);
    InterpolatedFragment(::TypeRef);
    InterpolatedFragment(::AST::MetaItem );
    ~InterpolatedFragment();
    InterpolatedFragment(Type , ::AST::ExprNode*);
    
    TokenTree& as_tt() { assert(m_type == TT); return *reinterpret_cast<TokenTree*>(m_ptr); }
    
    friend ::std::ostream& operator<<(::std::ostream& os, const InterpolatedFragment& x);
};
