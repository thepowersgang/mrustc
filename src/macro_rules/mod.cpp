/*
 */
#include <common.hpp>
#include "macro_rules.hpp"
#include <parse/parseerror.hpp>
#include <parse/tokentree.hpp>
#include <parse/common.hpp>
#include <limits.h>

#include "pattern_checks.hpp"

bool is_token_path(eTokenType tt) {
    switch(tt)
    {
    case TOK_IDENT:
    case TOK_DOUBLE_COLON:
    case TOK_LT:
    case TOK_DOUBLE_LT:
    case TOK_RWORD_SELF:
    case TOK_RWORD_SUPER:
    case TOK_INTERPOLATED_PATH:
        return true;
    default:
        return false;
    }
}
bool is_token_pat(eTokenType tt) {
    if( is_token_path(tt) )
        return true;
    switch( tt )
    {
    case TOK_PAREN_OPEN:
    case TOK_SQUARE_OPEN:
    
    case TOK_AMP:
    case TOK_RWORD_BOX:
    case TOK_RWORD_REF:
    case TOK_RWORD_MUT:
    case TOK_STRING:
    case TOK_INTEGER:
    case TOK_CHAR:
    case TOK_INTERPOLATED_PATTERN:
        return true;
    default:
        return false;
    }
}
bool is_token_type(eTokenType tt) {
    if( is_token_path(tt) )
        return true;
    switch( tt )
    {
    case TOK_PAREN_OPEN:
    case TOK_SQUARE_OPEN:
    case TOK_STAR:
    case TOK_AMP:
    case TOK_RWORD_EXTERN:
    case TOK_RWORD_UNSAFE:
    case TOK_RWORD_FN:
    case TOK_INTERPOLATED_TYPE:
        return true;
    default:
        return false;
    }
}
bool is_token_expr(eTokenType tt) {
    if( is_token_path(tt) )
        return true;
    switch( tt )
    {
    case TOK_AMP:   // Borrow
    case TOK_STAR:  // Deref
    case TOK_PAREN_OPEN:    // Parenthesised
    case TOK_SQUARE_OPEN:   // Array
    case TOK_MACRO:
    case TOK_DASH:  // Negate
    case TOK_EXCLAM:    // Invert
    case TOK_RWORD_BOX: // Box
    
    // Closures
    case TOK_RWORD_MOVE:
    case TOK_PIPE:
    case TOK_DOUBLE_PIPE:
    
    // Literal tokens
    case TOK_INTEGER:
    case TOK_FLOAT:
    case TOK_STRING:
    case TOK_RWORD_TRUE:
    case TOK_RWORD_FALSE:
    case TOK_INTERPOLATED_EXPR:
        return true;
    default:
        return false;
    }
}
bool is_token_stmt(eTokenType tt) {
    if( is_token_expr(tt) )
        return true;
    switch( tt )
    {
    case TOK_BRACE_OPEN:
    case TOK_RWORD_LET:
    case TOK_INTERPOLATED_STMT:
        return true;
    default:
        return false;
    }
}

MacroRulesPtr::~MacroRulesPtr()
{
    if(m_ptr)
    {
        delete m_ptr;
        m_ptr = nullptr;
    }
}

SERIALISE_TYPE_S(MacroRulesArm, {
})

void operator%(Serialiser& s, MacroPatEnt::Type c) {
    switch(c) {
    #define _(v) case MacroPatEnt::v: s << #v; return
    _(PAT_TOKEN);
    _(PAT_TT);
    _(PAT_PAT);
    _(PAT_TYPE);
    _(PAT_EXPR);
    _(PAT_LOOP);
    _(PAT_STMT);
    _(PAT_PATH);
    _(PAT_BLOCK);
    _(PAT_META);
    _(PAT_IDENT);
    #undef _
    }
}
void operator%(::Deserialiser& s, MacroPatEnt::Type& c) {
    ::std::string   n;
    s.item(n);
    #define _(v) else if(n == #v) c = MacroPatEnt::v
    if(0) ;
    _(PAT_TOKEN);
    _(PAT_TT);
    _(PAT_PAT);
    _(PAT_TYPE);
    _(PAT_EXPR);
    _(PAT_LOOP);
    //_(PAT_OPTLOOP);
    _(PAT_STMT);
    _(PAT_PATH);
    _(PAT_BLOCK);
    _(PAT_META);
    _(PAT_IDENT);
    else
        throw ::std::runtime_error( FMT("No conversion for '" << n << "'") );
    #undef _
}
SERIALISE_TYPE_S(MacroPatEnt, {
    s % type;
    s.item(name);
    s.item(tok);
    s.item(subpats);
});
::std::ostream& operator<<(::std::ostream& os, const MacroPatEnt& x)
{
    switch(x.type)
    {
    case MacroPatEnt::PAT_TOKEN: os << "=" << x.tok; break;
    case MacroPatEnt::PAT_LOOP:  os << "loop w/ "  << x.tok << " [" << x.subpats << "]";  break;
    default:
        os << "$" << x.name << ":";
        switch(x.type)
        {
        case MacroPatEnt::PAT_TOKEN: throw "";
        case MacroPatEnt::PAT_LOOP:  throw "";
        case MacroPatEnt::PAT_TT:    os << "tt";    break;
        case MacroPatEnt::PAT_PAT:   os << "pat";   break;
        case MacroPatEnt::PAT_IDENT: os << "ident"; break;
        case MacroPatEnt::PAT_PATH:  os << "path";  break;
        case MacroPatEnt::PAT_TYPE:  os << "type";  break;
        case MacroPatEnt::PAT_EXPR:  os << "expr";  break;
        case MacroPatEnt::PAT_STMT:  os << "stmt";  break;
        case MacroPatEnt::PAT_BLOCK: os << "block"; break;
        case MacroPatEnt::PAT_META:  os << "meta"; break;
        }
        break;
    }
    return os;
}
::std::ostream& operator<<(::std::ostream& os, const MacroPatEnt::Type& x)
{
    switch(x)
    {
    case MacroPatEnt::PAT_TOKEN: os << "PAT_TOKEN"; break;
    case MacroPatEnt::PAT_LOOP:  os << "PAT_LOOP";  break;
    case MacroPatEnt::PAT_TT:    os << "PAT_TT";    break;
    case MacroPatEnt::PAT_PAT:   os << "PAT_PAT";   break;
    case MacroPatEnt::PAT_IDENT: os << "PAT_IDENT"; break;
    case MacroPatEnt::PAT_PATH:  os << "PAT_PATH";  break;
    case MacroPatEnt::PAT_TYPE:  os << "PAT_TYPE";  break;
    case MacroPatEnt::PAT_EXPR:  os << "PAT_EXPR";  break;
    case MacroPatEnt::PAT_STMT:  os << "PAT_STMT";  break;
    case MacroPatEnt::PAT_BLOCK: os << "PAT_BLOCK"; break;
    case MacroPatEnt::PAT_META:  os << "PAT_META"; break;
    }
    return os;
}

SERIALISE_TU(MacroExpansionEnt, "MacroExpansionEnt", e,
(Token,
    s.item(e);
    ),
(NamedValue,
    s.item(e);
    ),
(Loop,
    s.item(e.entries);
    s.item(e.joiner);
    //s.item(e.variables);
    )
);

::std::ostream& operator<<(::std::ostream& os, const MacroExpansionEnt& x)
{
    TU_MATCH( MacroExpansionEnt, (x), (e),
    (Token,
        os << "=" << e;
        ),
    (NamedValue,
        if( e >> 30 ) {
            os << "$crate";
        }
        else {
            os << "$" << e;
        }
        ),
    (Loop,
        os << "${" << *e.variables.begin() << "}(" << e.entries << ") " << e.joiner;
        )
    )
    return os;
}

MacroRules::~MacroRules()
{
}
SERIALISE_TYPE_S(MacroRules, {
    s.item( m_exported );
    s.item( m_rules );
});

