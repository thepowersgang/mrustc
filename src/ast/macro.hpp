
#ifndef _AST_MACRO_HPP_
#define _AST_MACRO_HPP_

#include "../parse/tokentree.hpp"
#include "attrs.hpp"

namespace AST {

class MacroInvocation:
    public Serialisable
{
    ::AST::MetaItems   m_attrs;
    ::std::string   m_macro_name;
    ::std::string   m_ident;
    TokenTree   m_input;
public:
    MacroInvocation()
    {
    }
    
    MacroInvocation(MetaItems attrs, ::std::string macro, ::std::string ident, TokenTree input):
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

    SERIALISABLE_PROTOTYPES();
    
    friend ::std::ostream& operator<<(::std::ostream& os, const MacroInvocation& x) {
        os << x.m_attrs << x.m_macro_name << "! " << x.m_ident << x.m_input;
        return os;
    }
};


}

#endif

