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
#include <path.h>
#include "stringlist.h"
#include "build.h"  // spawn_process
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <cctype>   // toupper

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

struct CfgChecker
{
    std::unordered_set<std::string>   flags;
    std::unordered_multimap<std::string, std::string>    values;

    static CfgChecker for_target(const helpers::path& compiler_path, const char* target_spec);
    bool check_cfg(const char* str) const;
private:
    bool check_cfg(CfgParseLexer& p) const;
};

CfgChecker  gCfgChecker;

void Cfg_SetTarget(const char* target_name)
{
    gCfgChecker = CfgChecker::for_target(get_mrustc_path(), target_name);
}
bool Cfg_Check(const char* cfg_string, const std::vector<std::string>& features)
{
    auto checker = gCfgChecker;
    checker.values.insert(std::make_pair("feature", ""));
    for(const auto& feat : features) {
        checker.values.insert(std::make_pair("feature", feat));
    }
    return checker.check_cfg(cfg_string);
}
/// Dump configuration as CARGO_CFG_<name>
void Cfg_ToEnvironment(StringListKV& out)
{
    struct H {
        static std::string make_name(const std::string& n) {
            std::stringstream   ss;
            ss << "CARGO_CFG_";
            for(auto c : n)
            {
                if( isalnum(c) ) {
                    ss << char(std::toupper(c));
                }
                else if( c == '-' || c == '_' ) {
                    ss << "_";
                }
                else {
                    throw std::runtime_error("Unexpected character");
                }
            }
            return ss.str();
        }
    };
    for(const auto& v : gCfgChecker.values)
    {
        out.push_back(H::make_name(v.first), v.second);
    }
    for(const auto& flag : gCfgChecker.flags)
    {
        out.push_back(H::make_name(flag), "1");
    }
}


/*static*/ CfgChecker CfgChecker::for_target(const helpers::path& compiler_path, const char* target_spec)
{
    auto tmp_file_stdout = helpers::path(".temp.txt");
    StringList  args;
    if( target_spec )
    {
        args.push_back("--target");
        args.push_back(target_spec);
    }
    bool is_mrustc = (compiler_path.basename() == "mrustc" || compiler_path.basename() == "mrustc.exe");
    if(is_mrustc) {
        args.push_back("-Z");
        args.push_back("print-cfgs");
    }
    else {
        args.push_back("--print");
        args.push_back("cfg");
    }
    if( !spawn_process(compiler_path.str().c_str(), args, StringListKV(), tmp_file_stdout) )
        throw std::runtime_error("Unable to invoke compiler to get config options");

    ::std::ifstream ifs(tmp_file_stdout.str());
    if(!ifs.good())
        throw std::runtime_error("Unable to read compiler output");

    CfgChecker  rv;

    std::string line;
    while(!ifs.eof())
    {
        line.clear();
        ifs >> line;

        while(!line.empty() && isspace(line.back()))
        {
            line.pop_back();
        }

        if(line.empty())
            continue;

        if(!is_mrustc)
        {
            // Parse a cfg option (split on '=')
            auto pos = line.find('=');
            if(pos == std::string::npos)
            {
                rv.flags.insert(line.substr(0));
            }
            else
            {
                auto k = line.substr(0, pos);
                auto v = line.substr(pos+2,  line.size() - (pos+2) - 1);
                rv.values.insert(std::make_pair( std::move(k), std::move(v) ));
            }
        }
        else if( line[0] == '>')
        {
            // Parse a cfg option (split on '=')
            auto pos = line.find('=');
            if(pos == std::string::npos)
            {
                rv.flags.insert(line.substr(1));
            }
            else
            {
                auto k = line.substr(1, pos - 1);
                auto v = line.substr(pos+1);
                rv.values.insert(std::make_pair( std::move(k), std::move(v) ));
            }
        }
    }

    //for(const auto& f : rv.flags)
    //    std::cout << f << std::endl;
    //for(const auto& kv : rv.values)
    //    std::cout << kv.first << " = " << kv.second << std::endl;

    return rv;
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
    auto name_tok = p.consume();
    if( name_tok.ty() != CfgParseLexer::Tok::Ident )
        throw ::std::runtime_error("Expected an identifier");
    auto name = name_tok.to_string();
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
            throw std::runtime_error(format("Unknown operator in cfg `", name, "`"));
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

        auto its = this->values.equal_range(name);
        if( its.first != its.second ) {
            return std::any_of(its.first, its.second, [&](const auto& v){ return v.second == val; });
        }
        else {
            throw std::runtime_error(format("Unknown cfg value `", name, "` (=\"", val, "\")"));
        }
    }
    // Flags
    else {
        return this->flags.count(name) > 0;
    }
    throw ::std::runtime_error("Hit end of check_cfg");
}

