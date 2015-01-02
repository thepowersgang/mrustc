#ifndef MACROS_HPP_INCLUDED
#define MACROS_HPP_INCLUDED

#include "parse/lex.hpp"
#include "parse/tokentree.hpp"
#include <map>
#include <memory>
#include <cstring>

class MacroExpander;

class MacroRuleEnt
{
    friend class MacroExpander;

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
public:
    ::std::vector<MacroPatEnt>  m_pattern;
    ::std::vector<MacroRuleEnt> m_contents;
};

/// A sigle 'macro_rules!' block
typedef ::std::vector<MacroRule>    MacroRules;

struct cmp_str {
    bool operator()(const char* a, const char* b) const {
        return ::std::strcmp(a, b) < 0;
    }
};

class MacroExpander:
    public TokenStream
{
    typedef ::std::map<const char*, TokenTree, cmp_str>    t_mappings;
    const t_mappings    m_mappings;
    const ::std::vector<MacroRuleEnt>&  m_contents;
    size_t  m_ofs;

    ::std::auto_ptr<TTStream>   m_ttstream;
public:
    MacroExpander(const MacroExpander& x):
        m_mappings(x.m_mappings),
        m_contents(x.m_contents),
        m_ofs(0)
    {
    }
    MacroExpander(const ::std::vector<MacroRuleEnt>& contents, t_mappings mappings):
        m_mappings(mappings),
        m_contents(contents),
        m_ofs(0)
    {
    }
    virtual Token realGetToken();
};

extern MacroExpander    Macro_Invoke(const char* name, TokenTree input);

#endif // MACROS_HPP_INCLUDED
