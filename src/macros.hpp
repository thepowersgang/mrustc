#ifndef MACROS_HPP_INCLUDED
#define MACROS_HPP_INCLUDED

#include "parse/lex.hpp"
#include "parse/tokentree.hpp"
#include <map>
#include <memory>
#include <cstring>

class MacroExpander;

class MacroRuleEnt:
    public Serialisable
{
    friend class MacroExpander;

    Token   tok;
    ::std::string   name;
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
        PAT_IDENT,
        PAT_PATH,
        PAT_EXPR,
        PAT_STMT,
        PAT_BLOCK,
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
    
    MacroPatEnt(Token sep, ::std::vector<MacroPatEnt> ents):
        tok(sep),
        subpats( move(ents) ),
        type(PAT_LOOP)
    {
    }
    
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

    virtual Position getPosition() const override;
    virtual Token realGetToken() override;
};

extern void Macro_SetModule(const LList<AST::Module*>& mod);
extern MacroExpander    Macro_Invoke(const char* name, TokenTree input);

#endif // MACROS_HPP_INCLUDED
