
#ifndef _AST_MACRO_HPP_
#define _AST_MACRO_HPP_

#include "../parse/tokentree.hpp"
#include <span.hpp>
#include "attrs.hpp"

namespace AST {

class MacroInvocation:
    public Serialisable
{
    Span    m_span;
    
    ::AST::MetaItems   m_attrs;
    ::std::string   m_macro_name;
    ::std::string   m_ident;
    TokenTree   m_input;
public:
    MacroInvocation()
    {
    }
    
    MacroInvocation(Span span, MetaItems attrs, ::std::string macro, ::std::string ident, TokenTree input):
        m_span( mv$(span) ),
        m_attrs( mv$(attrs) ),
        m_macro_name( mv$(macro) ),
        m_ident( mv$(ident) ),
        m_input( mv$(input) )
    {
    }

    static ::std::unique_ptr<MacroInvocation> from_deserialiser(Deserialiser& s) {
        auto i = new MacroInvocation;
        s.item( *i );
        return ::std::unique_ptr<MacroInvocation>(i);
    }

    void clear() {
        m_macro_name = "";
        m_ident = "";
        m_input = TokenTree();
    }
    
          ::AST::MetaItems& attrs()       { return m_attrs; }
    const ::AST::MetaItems& attrs() const { return m_attrs; }

    const Span& span() const { return m_span; }
    const ::std::string& name() const { return m_macro_name; }

    const ::std::string& input_ident() const { return m_ident; }
    const TokenTree& input_tt() const { return m_input; }


    SERIALISABLE_PROTOTYPES();
    
    friend ::std::ostream& operator<<(::std::ostream& os, const MacroInvocation& x) {
        os << x.m_attrs;
        if(x.m_attrs.m_items.size() > 0)
            os << " ";
        os << x.m_macro_name << "! " << x.m_ident << x.m_input;
        return os;
    }
};


}

#endif

