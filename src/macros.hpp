#ifndef MACROS_HPP_INCLUDED
#define MACROS_HPP_INCLUDED

#include "parse/lex.hpp"
#include "parse/tokentree.hpp"
#include <common.hpp>
#include <map>
#include <memory>
#include <cstring>

class MacroExpander;

namespace AST {
    class Module;
}

class MacroRuleEnt:
    public Serialisable
{
    friend class MacroExpander;

    Token   tok;
    ::std::string   name;
    ::std::vector<MacroRuleEnt> subpats;
public:
    MacroRuleEnt():
        tok(TOK_NULL),
        name("")
    {
    }
    MacroRuleEnt(Token tok):
        tok( mv$(tok) ),
        name("")
    {
    }
    MacroRuleEnt(::std::string name):
        name( mv$(name) )
    {
    }
    MacroRuleEnt(Token tok, ::std::vector<MacroRuleEnt> subpats):
        tok( mv$(tok) ),
        subpats( mv$(subpats) )
    {
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const MacroRuleEnt& x);

    SERIALISABLE_PROTOTYPES();
};
struct MacroPatEnt:
    public Serialisable
{
    ::std::string   name;
    Token   tok;
    
    ::std::vector<MacroPatEnt>  subpats;
    
    enum Type {
        PAT_TOKEN,  // A token
        PAT_LOOP,   // $() Enables use of subpats
        
        PAT_TT, // :tt
        PAT_PAT,    // :pat
        PAT_IDENT,
        PAT_PATH,
        PAT_TYPE,
        PAT_EXPR,
        PAT_STMT,
        PAT_BLOCK,
        PAT_META,
    } type;

    MacroPatEnt():
        tok(TOK_NULL),
        type(PAT_TOKEN)
    {
    }
    MacroPatEnt(Token tok):
        tok( mv$(tok) ),
        type(PAT_TOKEN)
    {
    }
    
    MacroPatEnt(::std::string name, Type type):
        name( mv$(name) ),
        tok(),
        type(type)
    {
    }
    
    MacroPatEnt(Token sep, bool need_once, ::std::vector<MacroPatEnt> ents):
        name( need_once ? "+" : "*" ),
        tok( mv$(sep) ),
        subpats( move(ents) ),
        type(PAT_LOOP)
    {
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const MacroPatEnt& x);
    
    SERIALISABLE_PROTOTYPES();
};

/// A rule within a macro_rules! blcok
class MacroRule:
    public Serialisable
{
public:
    ::std::vector<MacroPatEnt>  m_pattern;
    ::std::vector<MacroRuleEnt> m_contents;
    
    SERIALISABLE_PROTOTYPES();
};

/// A sigle 'macro_rules!' block
class MacroRules:
    public Serialisable
{
public:
    bool m_exported;
    ::std::vector<MacroRule>  m_rules;
    
    MacroRules()
    {
    }
    MacroRules( ::std::vector<MacroRule> rules ):
        m_rules( mv$(rules) )
    {
    }
    
    SERIALISABLE_PROTOTYPES();
};

extern ::std::unique_ptr<TokenStream>   Macro_InvokeRules(const char *name, const MacroRules& rules, const TokenTree& input);
extern ::std::unique_ptr<TokenStream>   Macro_Invoke(const TokenStream& lex, const ::std::string& name, const TokenTree& input);

#endif // MACROS_HPP_INCLUDED
