#ifndef MACROS_HPP_INCLUDED
#define MACROS_HPP_INCLUDED

#include "parse/lex.hpp"
#include "parse/tokentree.hpp"
#include <common.hpp>
#include <map>
#include <memory>
#include <cstring>
#include "macro_rules_ptr.hpp"

class MacroExpander;

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
    unsigned int    name_index;
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
    
    MacroPatEnt(::std::string name, unsigned int name_index, Type type):
        name( mv$(name) ),
        name_index( name_index ),
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

struct MacroRulesPatFrag:
    public Serialisable
{
    ::std::vector<MacroPatEnt>  m_pats_ents;
    unsigned int    m_pattern_end;
    ::std::vector< MacroRulesPatFrag >  m_next_frags;
    
    MacroRulesPatFrag():
        m_pattern_end(~0)
    {}
    
    SERIALISABLE_PROTOTYPES();
};

/// An arm within a macro_rules! blcok
struct MacroRulesArm:
    public Serialisable
{
    ::std::vector< ::std::string>   m_param_names;
    ::std::vector<MacroRuleEnt> m_contents;
    
    MacroRulesArm()
    {}
    MacroRulesArm(::std::vector<MacroRuleEnt> contents):
        m_contents( mv$(contents) )
    {}
    
    SERIALISABLE_PROTOTYPES();
};

/// A sigle 'macro_rules!' block
class MacroRules:
    public Serialisable
{
public:
    bool m_exported;
    MacroRulesPatFrag  m_pattern;
    ::std::vector<MacroRulesArm>  m_rules;
    
    MacroRules()
    {
    }
    virtual ~MacroRules();
    
    SERIALISABLE_PROTOTYPES();
};

extern ::std::unique_ptr<TokenStream>   Macro_InvokeRules(const char *name, const MacroRules& rules, const TokenTree& input);
extern MacroRulesPtr    Parse_MacroRules(TokenStream& lex);

#endif // MACROS_HPP_INCLUDED
