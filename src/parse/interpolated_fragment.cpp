/*
 */
#include <iostream>
#include "interpolated_fragment.hpp"
#include <ast/ast.hpp>

InterpolatedFragment::~InterpolatedFragment()
{
    if( m_ptr )
    {
        switch(m_type)
        {
        case InterpolatedFragment::TT:  delete reinterpret_cast<TokenTree*>(m_ptr);  break;
        case InterpolatedFragment::PAT: delete reinterpret_cast<AST::Pattern*>(m_ptr); break;
        case InterpolatedFragment::PATH:delete reinterpret_cast<AST::Path*>(m_ptr);    break;
        case InterpolatedFragment::TYPE:delete reinterpret_cast<TypeRef*>(m_ptr);    break;
        case InterpolatedFragment::EXPR:
        case InterpolatedFragment::STMT:
        case InterpolatedFragment::BLOCK:
            delete reinterpret_cast<AST::ExprNode*>(m_ptr);
            break;
        case InterpolatedFragment::META:
            delete reinterpret_cast<AST::MetaItem*>(m_ptr);
            break;
        }
    }
}

InterpolatedFragment::InterpolatedFragment(InterpolatedFragment&& x):
    m_type( x.m_type )
{
    m_ptr = x.m_ptr, x.m_ptr = nullptr;
}
InterpolatedFragment& InterpolatedFragment::operator=(InterpolatedFragment&& x)
{
    m_type = x.m_type;
    m_ptr = x.m_ptr, x.m_ptr = nullptr;
    return *this;
}

InterpolatedFragment::InterpolatedFragment(InterpolatedFragment::Type type, AST::ExprNode* ptr):
    m_type( type ),
    m_ptr( ptr )
{
}
InterpolatedFragment::InterpolatedFragment(AST::MetaItem v):
    m_type( InterpolatedFragment::META ),
    m_ptr( new AST::MetaItem(mv$(v)) )
{
}
InterpolatedFragment::InterpolatedFragment(TokenTree v):
    m_type( InterpolatedFragment::TT ),
    m_ptr( new TokenTree(mv$(v)) )
{
}
InterpolatedFragment::InterpolatedFragment(AST::Path v):
    m_type( InterpolatedFragment::PATH ),
    m_ptr( new AST::Path(mv$(v)) )
{
}
InterpolatedFragment::InterpolatedFragment(AST::Pattern v):
    m_type( InterpolatedFragment::PAT ),
    m_ptr( new AST::Pattern(mv$(v)) )
{
}
InterpolatedFragment::InterpolatedFragment(TypeRef v):
    m_type( InterpolatedFragment::TYPE ),
    m_ptr( new TypeRef(mv$(v)) )
{
}

::std::ostream& operator<<(::std::ostream& os, InterpolatedFragment const& x)
{
    return os;
}

