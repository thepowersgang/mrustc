#ifndef MACROS_HPP_INCLUDED
#define MACROS_HPP_INCLUDED

#include "parse/lex.hpp"
#include <map>

class MacroRuleEnt
{
    Token   tok;
    ::std::string   name;
public:
    MacroRuleEnt(Token tok):
        tok(tok),
        name("")
    {
    }
    MacroRuleEnt(::std::string name):
        name(name)
    {
    }
};
struct MacroPatEnt
{
    Token   tok;
    ::std::string   name;
    enum Type {
        PAT_TOKEN,
        PAT_TT,
        PAT_IDENT,
        PAT_PATH,
        PAT_EXPR,
        PAT_STMT,
        PAT_BLOCK,
    } type;

    MacroPatEnt(::std::string name, Type type):
        tok(),
        name(name),
        type(type)
    {
    }
};

/// A rule within a macro_rules! blcok
class MacroRule
{
    struct MacroRuleState
    {
        ::std::map< ::std::string, TokenTree>   m_mappings;
        size_t  ofs;
    };
public:
    ::std::vector<MacroPatEnt>  m_pattern;
    ::std::vector<MacroRuleEnt> m_contents;
};

/// A sigle 'macro_rules!' block
typedef ::std::vector<MacroRule>    MacroRules;

class MacroExpander:
    public TokenStream
{
public:
    virtual Token realGetToken();
};

extern MacroExpander    Macro_Invoke(const char* name, TokenTree input);

#endif // MACROS_HPP_INCLUDED
