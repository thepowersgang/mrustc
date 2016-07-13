#ifndef MACROS_HPP_INCLUDED
#define MACROS_HPP_INCLUDED

#include "parse/lex.hpp"
#include "parse/tokentree.hpp"
#include <common.hpp>
#include <map>
#include <memory>
#include <cstring>
#include "macro_rules_ptr.hpp"
#include <set>

class MacroExpander;

TAGGED_UNION_EX(MacroExpansionEnt, (: public Serialisable), Token, (
    // TODO: have a "raw" stream instead of just tokens
    (Token, Token),
    (NamedValue, unsigned int),
    (Loop, struct {
        /// Contained entries
        ::std::vector< MacroExpansionEnt>   entries;
        /// Token used to join iterations
        Token   joiner;
        /// List of variables within this loop that control its iteration count
        ::std::set< unsigned int>    variables;
        })
    ),
    (),
    (),
    (
    public:
        SERIALISABLE_PROTOTYPES();
    )
    );
extern ::std::ostream& operator<<(::std::ostream& os, const MacroExpansionEnt& x);

/// Matching pattern entry
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

/// Fragment of a match pattern
struct MacroRulesPatFrag:
    public Serialisable
{
    /// Pattern entries within this fragment
    ::std::vector<MacroPatEnt>  m_pats_ents;
    /// Index of the arm that matches if the input ends after this fragment (~0 for nothing)
    unsigned int    m_pattern_end;
    /// List of fragments that follow this fragment
    ::std::vector< MacroRulesPatFrag >  m_next_frags;
    
    MacroRulesPatFrag():
        m_pattern_end(~0)
    {}
    
    SERIALISABLE_PROTOTYPES();
};

/// An expansion arm within a macro_rules! blcok
struct MacroRulesArm:
    public Serialisable
{
    /// Names for the parameters
    ::std::vector< ::std::string>   m_param_names;
    
    /// Rule contents
    ::std::vector<MacroExpansionEnt> m_contents;
    
    MacroRulesArm()
    {}
    MacroRulesArm(::std::vector<MacroExpansionEnt> contents):
        m_contents( mv$(contents) )
    {}
    MacroRulesArm(const MacroRulesArm&) = delete;
    MacroRulesArm& operator=(const MacroRulesArm&) = delete;
    MacroRulesArm(MacroRulesArm&&) = default;
    MacroRulesArm& operator=(MacroRulesArm&&) = default;
    
    SERIALISABLE_PROTOTYPES();
};

/// A sigle 'macro_rules!' block
class MacroRules:
    public Serialisable
{
public:
    bool m_exported;
    /// Parsing patterns
    MacroRulesPatFrag  m_pattern;
    /// Expansion rules
    ::std::vector<MacroRulesArm>  m_rules;
    
    MacroRules()
    {
    }
    virtual ~MacroRules();
    MacroRules(MacroRules&&) = default;
    
    SERIALISABLE_PROTOTYPES();
};

extern ::std::unique_ptr<TokenStream>   Macro_InvokeRules(const char *name, const MacroRules& rules, const TokenTree& input);
extern MacroRulesPtr    Parse_MacroRules(TokenStream& lex);

#endif // MACROS_HPP_INCLUDED
