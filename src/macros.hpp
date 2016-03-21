#ifndef MACROS_HPP_INCLUDED
#define MACROS_HPP_INCLUDED

#include "parse/lex.hpp"
#include "parse/tokentree.hpp"
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
        tok(tok),
        name("")
    {
    }
    MacroRuleEnt(::std::string name):
        name(name)
    {
    }
    MacroRuleEnt(Token tok, ::std::vector<MacroRuleEnt> subpats):
        tok(tok),
        subpats(subpats)
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
        PAT_TOKEN,
        PAT_TT,
        PAT_PAT,
        PAT_IDENT,
        PAT_PATH,
        PAT_TYPE,
        PAT_EXPR,
        PAT_STMT,
        PAT_BLOCK,
        PAT_META,
        PAT_LOOP,   // Enables use of subpats
    } type;

    MacroPatEnt():
        tok(TOK_NULL),
        type(PAT_TOKEN)
    {
    }
    MacroPatEnt(Token tok):
        tok(tok),
        type(PAT_TOKEN)
    {
    }
    
    MacroPatEnt(::std::string name, Type type):
        name(name),
        tok(),
        type(type)
    {
    }
    
    MacroPatEnt(Token sep, bool need_once, ::std::vector<MacroPatEnt> ents):
        name( need_once ? "+" : "*" ),
        tok(sep),
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
typedef ::std::vector<MacroRule>    MacroRules;

extern ::std::unique_ptr<TokenStream>   Macro_InvokeRules(const char *name, const MacroRules& rules, TokenTree input);
extern ::std::unique_ptr<TokenStream>   Macro_Invoke(const TokenStream& lex, const ::std::string& name, TokenTree input);

#endif // MACROS_HPP_INCLUDED
