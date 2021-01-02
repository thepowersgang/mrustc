/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * macro_rules/mod.cpp
 * - Top-level handling for macro_rules macros
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

    case TOK_UNDERSCORE:
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
    // Leading unary operators
    case TOK_AMP:   // Borrow
    case TOK_STAR:  // Deref
    case TOK_DASH:  // Negate
    case TOK_EXCLAM:    // Invert
    case TOK_RWORD_BOX: // Box
    // Composite values
    case TOK_PAREN_OPEN:    // Parenthesised
    case TOK_SQUARE_OPEN:   // Array

    // Flow
    case TOK_RWORD_RETURN:
    case TOK_RWORD_BREAK:
    case TOK_RWORD_CONTINUE:

    // Blocks
    case TOK_BRACE_OPEN:
    case TOK_RWORD_MATCH:
    case TOK_RWORD_IF:
    case TOK_RWORD_FOR:
    case TOK_RWORD_WHILE:
    case TOK_RWORD_LOOP:
    case TOK_RWORD_UNSAFE:

    // Closures
    case TOK_RWORD_MOVE:
    case TOK_PIPE:
    case TOK_DOUBLE_PIPE:

    // Literal tokens
    case TOK_INTEGER:
    case TOK_FLOAT:
    case TOK_STRING:
    case TOK_BYTESTRING:
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

bool is_token_item(eTokenType tt) {
    switch( tt )
    {
    case TOK_HASH:

    case TOK_RWORD_PUB:
    case TOK_RWORD_UNSAFE:
    case TOK_RWORD_TYPE:
    case TOK_RWORD_CONST:
    case TOK_RWORD_STATIC:
    case TOK_RWORD_FN:
    case TOK_RWORD_STRUCT:
    case TOK_RWORD_ENUM:
    case TOK_RWORD_TRAIT:
    case TOK_RWORD_MOD:
    case TOK_RWORD_USE:
    case TOK_RWORD_EXTERN:
    case TOK_RWORD_IMPL:
    // TODO: more?
    case TOK_INTERPOLATED_ITEM:
        return true;
    default:
        return false;
    }
}
bool is_token_vis(eTokenType tt) {
    switch(tt)
    {
    case TOK_RWORD_PUB:
    case TOK_RWORD_CRATE:
    case TOK_INTERPOLATED_VIS:
        return true;
    default:
        return true;    // TODO: Is this true? it can capture just nothing
    }
}

MacroRulesPtr::MacroRulesPtr(MacroRules* p):
    m_ptr(p)
{
    //::std::cout << "MRP new " << m_ptr << ::std::endl;
}
MacroRulesPtr::~MacroRulesPtr()
{
    if(m_ptr)
    {
        //::std::cout << "MRP delete " << m_ptr << ::std::endl;
        delete m_ptr;
        m_ptr = nullptr;
    }
}

::std::ostream& operator<<(::std::ostream& os, const MacroPatEnt& x)
{
    switch(x.type)
    {
    case MacroPatEnt::PAT_TOKEN: os << "=" << x.tok; break;
    case MacroPatEnt::PAT_LOOP:  os << "loop #" << x.name_index << x.name << " w/ "  << x.tok << " [" << x.subpats << "]";  break;
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
        case MacroPatEnt::PAT_ITEM:  os << "item"; break;
        case MacroPatEnt::PAT_VIS:   os << "vis"; break;
        case MacroPatEnt::PAT_LIFETIME: os << "lifetime"; break;
        case MacroPatEnt::PAT_LITERAL: os << "literal"; break;
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
    case MacroPatEnt::PAT_ITEM:  os << "PAT_ITEM"; break;
    case MacroPatEnt::PAT_VIS:   os << "PAT_VIS"; break;
    case MacroPatEnt::PAT_LIFETIME: os << "PAT_LIFETIME"; break;
    case MacroPatEnt::PAT_LITERAL: os << "PAT_LITERAL"; break;
    }
    return os;
}

::std::ostream& operator<<(::std::ostream& os, const SimplePatEnt& x)
{
    TU_MATCH_HDRA( (x), { )
    TU_ARMA(End, _e) os << "End";
    TU_ARMA(LoopStart, e) os << "LoopStart(" << e.index << ")";
    TU_ARMA(LoopNext, _e) os << "LoopNext";
    TU_ARMA(LoopEnd, _e) os << "LoopEnd";
    TU_ARMA(Jump, e) {
        os << "Jump(->" << e.jump_target << ")";
        }
    TU_ARMA(ExpectTok, e) {
        os << "Expect(" << e << ")";
        }
    TU_ARMA(ExpectPat, e) {
        os << "Expect($" << e.idx << " = " << e.type << ")";
        }
    TU_ARMA(If, e) {
        os << "If(" << (e.is_equal ? "=" : "!=") << "[";
        for(const auto& p : e.ents) {
            if(p.ty == MacroPatEnt::PAT_TOKEN)
                os << p.tok;
            else
                os << p.ty;
            os << ", ";
        }
        os << "] ->" << e.jump_target << ")";
        }
    }
    return os;
}

::std::ostream& operator<<(::std::ostream& os, const MacroExpansionEnt& x)
{
    TU_MATCH_HDRA( (x), {)
    TU_ARMA(Token, e) {
        os << "=" << e;
        }
    TU_ARMA(NamedValue, e) {
        if( e >> 30 ) {
            os << "$crate";
        }
        else {
            os << "$" << e;
        }
        }
    TU_ARMA(Loop, e) {
        os << "${" << e.controlling_input_loops << "}(" << e.entries << ") " << e.joiner;
        }
    }
    return os;
}

MacroRules::~MacroRules()
{
}
MacroRulesArm::~MacroRulesArm()
{
}

