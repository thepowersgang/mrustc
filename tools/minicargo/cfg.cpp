/*
 * mrustc "minicargo" (minimal cargo clone)
 * - By John Hodge (Mutabah)
 *
 * cfg.cpp
 * - Handling of target configuration (in manifest nodes)
 */
#include <iostream> // cerr
#include "debug.h"
#include <cassert>
#include <algorithm>
#include <string>
#include <cstring>
#include "cfg.hpp"

// TODO: Extract this from the target at runtime (by invoking the compiler on the passed target)
#ifdef _WIN32
//# define TARGET_NAME    "i586-windows-msvc"
# define CFG_UNIX   false
# define CFG_WINDOWS true
#elif defined(__NetBSD__)
//# define TARGET_NAME "x86_64-unknown-netbsd"
# define CFG_UNIX   true
# define CFG_WINDOWS false
#else
//# define TARGET_NAME "x86_64-unknown-linux-gnu"
# define CFG_UNIX   true
# define CFG_WINDOWS false
#endif

class CfgParseLexer
{
public:
    class Tok
    {
        friend class CfgParseLexer;
    public:
        enum Type {
            EndOfStream,
            Operator,
            Ident,
            String,
        };
    private:
        Type    m_ty;
        const char* s;
        const char* e;
        ::std::string   m_val;
        Tok():
            m_ty(EndOfStream), s(nullptr),e(nullptr)
        {
        }
        Tok(const char* s):
            m_ty(Operator), s(s), e(s+1), m_val()
        {
        }
        Tok(const char* s, const char* e):
            m_ty(Ident), s(s), e(e), m_val()
        {
        }
        Tok(const char* s, const char* e, ::std::string val):
            m_ty(String), s(s), e(e), m_val(::std::move(val))
        {
        }
    public:
        bool operator==(char c) const {
            return (m_ty == Operator && *s == c);
        }
        bool operator!=(char c) const { return !(*this == c); }
        bool operator==(const char* v) const {
            return strlen(v) == static_cast<unsigned>(e - s) && memcmp(s, v, e-s) == 0;
        }
        bool operator!=(const char* v) const { return !(*this == v); }

        const Type ty() const { return m_ty; }
        const ::std::string& str() const {
            return m_val;
        }
        ::std::string to_string() const {
            return ::std::string(s, e);
        }
    };
private:
    const char* m_pos;
    Tok m_cur;

public:
    CfgParseLexer(const char* s):
        m_pos(s),
        m_cur(nullptr,nullptr)
    {
        consume();
    }
    const Tok& cur() const {
        return m_cur;
    }

    Tok consume() {
        auto rv = m_cur;
        m_cur = get_next();
        //::std::cout << "consume: " << rv.to_string() << " => " << m_cur.to_string() << ::std::endl;
        return rv;
    }
    bool consume_if(char c) {
        if( cur() == c ) {
            consume();
            return true;
        }
        else {
            return false;
        }
    }
    bool consume_if(const char* s) {
        if( cur() == s ) {
            consume();
            return true;
        }
        else {
            return false;
        }
    }
private:
    Tok get_next();
};

struct CfgChecker
{
    const char* target_env;
    const char* target_os;
    const char* target_vendor;
    const char* target_arch;

    static CfgChecker for_target(const char* compiler_path, const char* target_spec);
    bool check_cfg(const char* str) const;
private:
    bool check_cfg(CfgParseLexer& p) const;
};

CfgChecker  gCfgChecker {
    (CFG_WINDOWS ? "msvc" : "gnu"),
    (CFG_WINDOWS ? "windows" : "linux"),
    "",
    "x86"
    };

CfgParseLexer::Tok CfgParseLexer::get_next()
{
    while(*m_pos == ' ')
        m_pos ++;
    if(*m_pos == 0)
        return Tok();
    switch(*m_pos)
    {
    case '(': case ')':
    case ',': case '=':
        return Tok(m_pos++);
    case '"': {
        ::std::string   str;
        auto s = m_pos;
        m_pos ++;
        while( *m_pos != '"' )
        {
            if( *m_pos == '\\' )
            {
                TODO("Escape sequences in cfg parser");
            }
            str += *m_pos;
            m_pos ++;
        }
        m_pos ++;
        return Tok(s, m_pos, str); }
    default:
        if( isalnum(*m_pos) || *m_pos == '_' )
        {
            auto s = m_pos;
            while(isalnum(*m_pos) || *m_pos == '_')
                m_pos ++;
            return Tok(s, m_pos);
        }
        else
        {
            throw ::std::runtime_error(format("Unexpected character in cfg() - ", *m_pos));
        }
    }
}

bool Cfg_Check(const char* cfg_string)
{
    if( gCfgChecker.target_os == nullptr )
    {
        // TODO: If the checker isn't initialised, invoke the compiler and ask it to dump the current target
        // - It's pre-initialised above currently
    }
    return gCfgChecker.check_cfg(cfg_string);
}

/*static*/ CfgChecker CfgChecker::for_target(const char* compiler_path, const char* target_spec)
{
    throw "";
}

bool CfgChecker::check_cfg(const char* cfg_string) const
{
    CfgParseLexer p { cfg_string + 4 };

    bool success = this->check_cfg(p);
    if( !p.consume_if(")") )
        throw ::std::runtime_error(format("Expected ')' after cfg condition - got", p.cur().to_string()));

    return success;
}

bool CfgChecker::check_cfg(CfgParseLexer& p) const
{
    auto name = p.consume();
    if( name.ty() != CfgParseLexer::Tok::Ident )
        throw ::std::runtime_error("Expected an identifier");
    // Combinators
    if( p.consume_if('(') ) {
        bool rv;
        if( false ) {
        }
        else if( name == "not" ) {
            rv = !check_cfg(p);
        }
        else if( name == "all" ) {
            rv = true;
            do
            {
                rv &= check_cfg(p);
            } while(p.consume_if(','));
        }
        else if( name == "any" ) {
            rv = false;
            do
            {
                rv |= check_cfg(p);
            } while(p.consume_if(','));
        }
        else {
            throw std::runtime_error(format("Unknown operator in cfg `", name.to_string(), "`"));
        }
        if( !p.consume_if(')') )
            throw ::std::runtime_error("Expected ')' after combinator content");
        return rv;
    }
    // Values
    else if( p.consume_if('=') ) {
        auto t = p.consume();
        if( t.ty() != CfgParseLexer::Tok::String )
            throw ::std::runtime_error("Expected a string after `=`");
        const auto& val = t.str();

        if( false ) {
        }
        else if( name == "target_env" )
            return val == this->target_env;
        else if( name == "target_os" )
            return val == this->target_os;
        else if( name == "target_arch" )
            return val == this->target_arch;
        else if( name == "target_vendor" )
            return val == this->target_vendor;
        else {
            throw std::runtime_error(format("Unknown cfg value `", name.to_string(), "` (=\"", val, "\")"));
        }
    }
    // Flags
    else {
        if( false ) {
        }
        else if( name == "unix" ) {
            return CFG_UNIX;
        }
        else if( name == "windows" ) {
            return CFG_WINDOWS;
        }
        else if( name == "stage0" ) {
            return false;
        }
        else {
            throw std::runtime_error(format("Unknown cfg flag `", name.to_string(), "`"));
        }
    }
    throw ::std::runtime_error("Hit end of check_cfg");
}
