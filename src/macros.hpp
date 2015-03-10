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

    friend ::std::ostream& operator<<(::std::ostream& os, const MacroRuleEnt& x) {
        return os << "MacroRuleEnt( '"<<x.name<<"'" << x.tok << ", " << x.subpats << ")";
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

    friend ::std::ostream& operator<<(::std::ostream& os, const MacroPatEnt& mpe) {
        return os << "MacroPatEnt( '"<<mpe.name<<"'" << mpe.tok << ", " << mpe.subpats << ")";
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

class MacroExpander:
    public TokenStream
{
public:
    // MultiMap (layer, name) -> TokenTree
    // - Multiple values are only allowed for layer>0
    typedef ::std::pair<unsigned,const char*>   t_mapping_key;

    struct cmp_mk {
        bool operator()(const t_mapping_key& a, const t_mapping_key& b) const {
            return a.first < b.first || ::std::strcmp(a.second, b.second) < 0;
        }
    };
    typedef ::std::multimap<t_mapping_key, TokenTree, cmp_mk>    t_mappings;

private:
    const TokenTree&    m_crate_path;
    const ::std::vector<MacroRuleEnt>&  m_root_contents;
    const t_mappings    m_mappings;
    
    /// Layer states : Index and Iteration
    ::std::vector< ::std::pair<size_t,size_t> >   m_offsets;
    
    /// Cached pointer to the current layer
    const ::std::vector<MacroRuleEnt>*  m_cur_ents;  // For faster lookup.
    /// Iteration counts for each layer
    ::std::vector<size_t>   m_layer_counts;

    ::std::auto_ptr<TTStream>   m_ttstream;
    
public:
    MacroExpander(const MacroExpander& x):
        m_crate_path(x.m_crate_path),
        m_root_contents(x.m_root_contents),
        m_mappings(x.m_mappings),
        m_offsets({ {0,0} }),
        m_cur_ents(&m_root_contents)
    {
        prep_counts();
    }
    MacroExpander(const ::std::vector<MacroRuleEnt>& contents, t_mappings mappings, const TokenTree& crate_path):
        m_crate_path(crate_path),
        m_root_contents(contents),
        m_mappings(mappings),
        m_offsets({ {0,0} }),
        m_cur_ents(&m_root_contents)
    {
        prep_counts();
    }

    virtual Position getPosition() const override;
    virtual Token realGetToken() override;
private:
    const ::std::vector<MacroRuleEnt>* getCurLayer() const;
    void prep_counts();
};

extern void Macro_SetModule(const LList<AST::Module*>& mod);
extern MacroExpander    Macro_Invoke(const char* name, TokenTree input);

#endif // MACROS_HPP_INCLUDED
